#include "ddp_display.h"

#include <algorithm>
#include <cctype>
#include <cstring>

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <lwip/sockets.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace esphome {
namespace ddp_display {

static const char *const TAG = "ddp_display";

void DDPDisplay::setup() {
  this->buf_size_ = size_t(this->width_) * this->height_ * 3;

  RAMAllocator<uint8_t> allocator;
  for (auto &buf : this->bufs_) {
    buf = allocator.allocate(this->buf_size_);
    if (buf == nullptr) {
      ESP_LOGE(TAG, "Could not allocate 3 x %u bytes for frame buffers", unsigned(this->buf_size_));
      this->mark_failed();
      return;
    }
    memset(buf, 0, this->buf_size_);
  }

  this->sock_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (this->sock_ < 0) {
    ESP_LOGE(TAG, "socket() failed: errno %d", errno);
    this->mark_failed();
    return;
  }

  struct sockaddr_in addr {};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(this->port_);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  if (::bind(this->sock_, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) < 0) {
    ESP_LOGE(TAG, "bind() to UDP port %u failed: errno %d", this->port_, errno);
    ::close(this->sock_);
    this->sock_ = -1;
    this->mark_failed();
    return;
  }

  // Dedicated task with a blocking recv(): packets are pulled out of lwIP as soon as
  // they arrive, independent of how busy the ESPHome main loop is.
  // Priority above the ESPHome loop (1), below lwIP/WiFi/ETH tasks (18+).
  if (xTaskCreate(&DDPDisplay::rx_task_trampoline_, "ddp_rx", 4096, this, 10, nullptr) != pdPASS) {
    ESP_LOGE(TAG, "Could not start RX task");
    this->mark_failed();
    return;
  }
}

void DDPDisplay::rx_task_trampoline_(void *arg) { static_cast<DDPDisplay *>(arg)->rx_task_(); }

void DDPDisplay::rx_task_() {
  for (;;) {
    const int n = ::recv(this->sock_, this->rx_buf_, sizeof(this->rx_buf_), 0);
    if (n <= 0) {
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    this->packets_.fetch_add(1, std::memory_order_relaxed);
    this->handle_packet_(this->rx_buf_, size_t(n));
  }
}

void DDPDisplay::handle_packet_(const uint8_t *buf, size_t len) {
  DDPPacket pkt;
  if (!parse_ddp(buf, len, pkt))
    return;

  if (pkt.length > 0 && pkt.offset < this->buf_size_) {
    const size_t n = std::min<size_t>(pkt.length, this->buf_size_ - pkt.offset);
    memcpy(this->bufs_[this->back_idx_] + pkt.offset, pkt.data, n);
    this->rx_frame_bytes_ += n;
  }

  // Push flag = "frame complete, show it". Senders that never set it still work
  // as long as their last packet reaches the end of the buffer.
  const bool reaches_end = pkt.length > 0 && size_t(pkt.offset) + pkt.length >= this->buf_size_;
  if (pkt.push || reaches_end)
    this->present_();
}

void DDPDisplay::present_() {
  // Fewer bytes than a full frame since the last push = some packets were lost.
  if (this->rx_frame_bytes_ < this->buf_size_)
    this->incomplete_.fetch_add(1, std::memory_order_relaxed);
  this->rx_frame_bytes_ = 0;
  {
    std::lock_guard<std::mutex> lock(this->swap_mutex_);
    std::swap(this->back_idx_, this->ready_idx_);
    this->ready_new_ = true;
  }
  this->last_frame_ms_.store(millis());
  this->has_frame_.store(true);
  this->frames_.fetch_add(1, std::memory_order_relaxed);
  this->new_frame_.store(true);
}

bool DDPDisplay::is_active() const {
  if (this->is_failed())
    return false;
  switch (this->mode_.load()) {
    case DDPMode::DDP:
      return true;
    case DDPMode::NORMAL:
      return false;
    case DDPMode::AUTO:
    default:
      if (!this->has_frame_.load())
        return false;
      if (this->timeout_ms_ == 0)
        return true;
      return (millis() - this->last_frame_ms_.load()) < this->timeout_ms_;
  }
}

void DDPDisplay::loop() {
  const bool active = this->is_active();
  bool redraw = false;

  if (this->new_frame_.exchange(false) && active)
    redraw = true;

  if (active != this->was_active_) {
    this->was_active_ = active;
    this->clear_frames_ = 2;  // clear the area outside the DDP frame once in each buffer (double buffering)
    ESP_LOGI(TAG, "%s", active ? "DDP active - showing network frames" : "DDP inactive - normal display");
    redraw = true;  // switch the panel immediately, don't wait for update_interval
  }

  if (redraw)
    this->redraw_callback_.call();

  const uint32_t now = millis();
  if (now - this->fps_last_ms_ >= 1000) {
    const uint32_t frames = this->frames_.load();
    this->fps_ = float(frames - this->fps_last_frames_) * 1000.0f / float(now - this->fps_last_ms_);
    this->fps_last_frames_ = frames;
    const uint32_t packets = this->packets_.load();
    this->pps_ = float(packets - this->pps_last_packets_) * 1000.0f / float(now - this->fps_last_ms_);
    this->pps_last_packets_ = packets;
    this->fps_last_ms_ = now;
  }
}

bool DDPDisplay::draw(display::Display &it) {
  if (!this->is_active())
    return false;

  const int w = std::min<int>(this->width_, it.get_width());
  const int h = std::min<int>(this->height_, it.get_height());
  // DDP frame smaller than the display: the rest must be black in BOTH buffers
  // (double buffering + auto_clear off), otherwise stale content flickers on every flip.
  // Clear only for the first 2 frames after activation - clearing every frame touches the
  // whole DMA buffer and, because hub75 flip_buffer() returns before the hardware actually
  // switches, those writes leak onto the buffer still being shown as random black stripes.
  if (this->clear_frames_ > 0 && (w < it.get_width() || h < it.get_height())) {
    it.fill(Color::BLACK);
    this->clear_frames_--;
  }

  if (w <= 0 || h <= 0)
    return true;

  // One bulk call for the whole frame. Drivers like hub75 implement this natively
  // (single conversion pass + one cache flush) instead of 4096 x draw_pixel_at,
  // which on hub75 flushes the whole DMA buffer after every single pixel.
  {
    // Pick up the newest complete frame (index swap only - RX task is never blocked by drawing)
    std::lock_guard<std::mutex> lock(this->swap_mutex_);
    if (this->ready_new_) {
      std::swap(this->ready_idx_, this->front_idx_);
      this->ready_new_ = false;
    }
  }
  it.draw_pixels_at(0, 0, w, h, this->bufs_[this->front_idx_], display::COLOR_ORDER_RGB, display::COLOR_BITNESS_888,
                    false, 0, 0, this->width_ - w);
  return true;
}

void DDPDisplay::set_mode(DDPMode mode) {
  if (this->mode_.exchange(mode) != mode)
    ESP_LOGI(TAG, "Mode: %s", this->get_mode_str());
}

void DDPDisplay::set_mode(const std::string &mode) {
  std::string m = mode;
  std::transform(m.begin(), m.end(), m.begin(), [](unsigned char c) { return std::tolower(c); });
  if (m == "auto") {
    this->set_mode(DDPMode::AUTO);
  } else if (m == "ddp") {
    this->set_mode(DDPMode::DDP);
  } else if (m == "normal" || m == "esphome") {
    this->set_mode(DDPMode::NORMAL);
  } else {
    ESP_LOGW(TAG, "Unknown mode '%s' (use Auto / DDP / Normal)", mode.c_str());
  }
}

const char *DDPDisplay::get_mode_str() const {
  switch (this->mode_.load()) {
    case DDPMode::DDP:
      return "DDP";
    case DDPMode::NORMAL:
      return "Normal";
    case DDPMode::AUTO:
    default:
      return "Auto";
  }
}

void DDPDisplay::dump_config() {
  ESP_LOGCONFIG(TAG, "DDP Display:");
  ESP_LOGCONFIG(TAG, "  Size: %ux%u (%u bytes/frame)", this->width_, this->height_, unsigned(this->buf_size_));
  ESP_LOGCONFIG(TAG, "  UDP port: %u", this->port_);
  ESP_LOGCONFIG(TAG, "  Auto timeout: %u ms", unsigned(this->timeout_ms_));
  ESP_LOGCONFIG(TAG, "  Mode: %s", this->get_mode_str());
  if (this->is_failed())
    ESP_LOGE(TAG, "  Setup FAILED");
}

}  // namespace ddp_display
}  // namespace esphome
