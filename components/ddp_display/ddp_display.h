#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>

#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/components/display/display.h"

#include "ddp_protocol.h"

namespace esphome {
namespace ddp_display {

enum class DDPMode : uint8_t {
  AUTO = 0,    // show DDP while frames arrive, fall back to normal after timeout
  DDP = 1,     // always show the DDP buffer (last received frame)
  NORMAL = 2,  // ignore DDP, normal ESPHome drawing
};

class DDPDisplay : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  // --- config (from YAML) ---
  void set_size(uint16_t width, uint16_t height) {
    this->width_ = width;
    this->height_ = height;
  }
  void set_port(uint16_t port) { this->port_ = port; }
  void set_timeout(uint32_t timeout_ms) { this->timeout_ms_ = timeout_ms; }

  // --- runtime API (for lambdas) ---
  void set_mode(DDPMode mode);
  void set_mode(const std::string &mode);  // "Auto" / "DDP" / "Normal" (case-insensitive)
  DDPMode get_mode() const { return this->mode_.load(); }
  const char *get_mode_str() const;

  // True when the DDP buffer should be shown instead of the normal content.
  bool is_active() const;

  // Call at the top of your display lambda:
  //   if (id(ddp).draw(it)) return;
  // Draws the last DDP frame and returns true if DDP is active, otherwise does nothing.
  bool draw(display::Display &it);

  float get_fps() const { return this->fps_; }
  uint32_t get_frame_count() const { return this->frames_.load(); }
  uint32_t get_packet_count() const { return this->packets_.load(); }
  float get_packet_rate() const { return this->pps_; }                    // UDP packets/s received
  uint32_t get_incomplete_count() const { return this->incomplete_.load(); }  // frames shown with missing packets

  void add_on_redraw_callback(std::function<void()> &&callback) { this->redraw_callback_.add(std::move(callback)); }

 protected:
  static void rx_task_trampoline_(void *arg);
  void rx_task_();
  void handle_packet_(const uint8_t *buf, size_t len);
  void present_();

  uint16_t width_{64};
  uint16_t height_{64};
  uint16_t port_{4048};
  uint32_t timeout_ms_{5000};

  // Triple buffering - the RX task never waits for drawing:
  //   bufs_[back_idx_]  : being filled by the RX task
  //   bufs_[ready_idx_] : last complete frame, not yet picked up
  //   bufs_[front_idx_] : being drawn by the main loop
  // Only the index swaps are done under swap_mutex_ (a few instructions).
  size_t buf_size_{0};
  uint8_t *bufs_[3]{nullptr, nullptr, nullptr};
  uint8_t back_idx_{0};
  uint8_t ready_idx_{1};
  uint8_t front_idx_{2};
  bool ready_new_{false};
  std::mutex swap_mutex_;
  size_t rx_frame_bytes_{0};  // RX task only

  int sock_{-1};
  uint8_t rx_buf_[1500];

  std::atomic<DDPMode> mode_{DDPMode::AUTO};
  std::atomic<bool> new_frame_{false};
  std::atomic<bool> has_frame_{false};
  std::atomic<uint32_t> last_frame_ms_{0};
  std::atomic<uint32_t> frames_{0};
  std::atomic<uint32_t> packets_{0};
  std::atomic<uint32_t> incomplete_{0};

  // main-loop only
  bool was_active_{false};
  uint8_t clear_frames_{2};
  uint32_t fps_last_ms_{0};
  uint32_t fps_last_frames_{0};
  float fps_{0.0f};
  uint32_t pps_last_packets_{0};
  float pps_{0.0f};

  CallbackManager<void()> redraw_callback_;
};

class DDPRedrawTrigger : public Trigger<> {
 public:
  explicit DDPRedrawTrigger(DDPDisplay *parent) {
    parent->add_on_redraw_callback([this]() { this->trigger(); });
  }
};

}  // namespace ddp_display
}  // namespace esphome
