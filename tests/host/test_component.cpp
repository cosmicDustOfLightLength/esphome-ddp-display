// Host (PC) tests for ddp_display, built against minimal ESPHome/lwIP/FreeRTOS stubs.
// Real UDP sockets on 127.0.0.1 are used, so this exercises the actual receive path.
//
//   ./tests/run_tests.sh

#include "ddp_display.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>
#include <algorithm>

using namespace esphome;
using namespace esphome::ddp_display;

static int failures = 0;
#define CHECK(cond, msg)                         \
  do {                                           \
    if (cond) {                                  \
      printf("  ok   %s\n", msg);                \
    } else {                                     \
      printf("  FAIL %s  (%s)\n", msg, #cond);   \
      failures++;                                \
    }                                            \
  } while (0)

static void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// Sends one full RGB frame as DDP packets (push flag on the last one).
static void send_frame(uint16_t port, const uint8_t *rgb, size_t len, size_t chunk = 1440) {
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  uint8_t pkt[1500];
  for (size_t off = 0; off < len; off += chunk) {
    size_t n = std::min(chunk, len - off);
    pkt[0] = 0x40 | (off + n >= len ? 0x01 : 0);
    pkt[1] = 0;
    pkt[2] = 0x0B;
    pkt[3] = 1;
    pkt[4] = off >> 24;
    pkt[5] = off >> 16;
    pkt[6] = off >> 8;
    pkt[7] = off;
    pkt[8] = n >> 8;
    pkt[9] = n;
    memcpy(pkt + 10, rgb + off, n);
    sendto(fd, pkt, 10 + n, 0, (sockaddr *) &a, sizeof a);
  }
  close(fd);
}

// Fake display that records what it was given.
struct FakeDisplay : display::Display {
  int w, h, delay_ms;
  int fills = 0, bulk_calls = 0, torn = 0;
  uint8_t last_first[3]{};
  FakeDisplay(int w, int h, int delay_ms = 0) : w(w), h(h), delay_ms(delay_ms) {}
  int get_width() override { return w; }
  int get_height() override { return h; }
  void draw_pixel_at(int, int, Color) override {}
  void fill(Color) override { fills++; }
  void draw_pixels_at(int, int, int dw, int dh, const uint8_t *p, display::ColorOrder, display::ColorBitness, bool,
                      int, int, int) override {
    bulk_calls++;
    if (delay_ms)
      sleep_ms(delay_ms);  // simulate slow drawing (hub75 into PSRAM)
    memcpy(last_first, p, 3);
    for (int i = 0; i < dw * dh * 3; i++)
      if (p[i] != p[0]) {
        torn++;
        break;
      }
  }
};

static void test_protocol() {
  puts("[protocol]");
  DDPPacket p;
  uint8_t bad_ver[10] = {0x00, 0, 0, 1, 0, 0, 0, 0, 0, 0};
  CHECK(!parse_ddp(bad_ver, 10, p), "rejects wrong version");
  uint8_t query[10] = {0x43, 0, 0, 1, 0, 0, 0, 0, 0, 0};
  CHECK(!parse_ddp(query, 10, p), "ignores query packets");
  uint8_t cfg[10] = {0x41, 0, 0, 250, 0, 0, 0, 0, 0, 0};
  CHECK(!parse_ddp(cfg, 10, p), "ignores config/status IDs");
  uint8_t trunc[12] = {0x41, 0, 0, 1, 0, 0, 0, 0, 0, 5, 1, 2};
  CHECK(!parse_ddp(trunc, 12, p), "rejects truncated payload");
  uint8_t good[13] = {0x41, 0, 0x0B, 1, 0, 0, 0x01, 0x00, 0, 3, 9, 8, 7};
  CHECK(parse_ddp(good, 13, p) && p.offset == 256 && p.length == 3 && p.push && p.data[0] == 9,
        "parses offset/length/push/data");
  uint8_t tc[17] = {0x51, 0, 0, 1, 0, 0, 0, 0, 0, 3, 0xAA, 0xAA, 0xAA, 0xAA, 1, 2, 3};
  CHECK(parse_ddp(tc, 17, p) && p.data[0] == 1, "skips 4-byte timecode header");
}

static void test_modes_and_frames() {
  puts("[modes / frames 64x64]");
  DDPDisplay d;
  d.set_size(64, 64);
  d.set_port(14048);
  d.set_timeout(1000);
  int redraws = 0;
  d.add_on_redraw_callback([&] { redraws++; });
  d.setup();
  CHECK(!d.is_failed(), "setup ok");

  FakeDisplay fd(64, 64);
  CHECK(!d.draw(fd), "Auto: inactive before first frame");

  std::vector<uint8_t> frame(64 * 64 * 3, 0x42);
  send_frame(14048, frame.data(), frame.size());
  sleep_ms(100);
  d.loop();
  CHECK(d.draw(fd), "Auto: active after a frame");
  CHECK(fd.last_first[0] == 0x42, "frame content delivered");
  CHECK(fd.bulk_calls == 1, "whole frame drawn with one bulk call");
  CHECK(d.get_frame_count() == 1 && d.get_packet_count() == 9, "1 frame / 9 packets counted");
  CHECK(d.get_incomplete_count() == 0, "no incomplete frames");
  CHECK(redraws == 1, "on_redraw fired on activation");

  sleep_ms(1100);
  d.loop();
  CHECK(!d.is_active(), "Auto: timeout returns to normal display");
  d.set_mode(std::string("ddp"));
  CHECK(d.is_active(), "mode DDP: always active");
  d.set_mode(std::string("Normal"));
  CHECK(!d.is_active(), "mode Normal: never active");
  d.set_mode(std::string("Auto"));

  // incomplete frame: drop the first packet
  int fdsock = socket(AF_INET, SOCK_DGRAM, 0);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(14048);
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  uint8_t pkt[1450] = {0x41, 0, 0x0B, 1, 0, 0, 0, 0, 0x05, 0xA0};  // offset 0, len 1440, push
  sendto(fdsock, pkt, 1450, 0, (sockaddr *) &a, sizeof a);
  close(fdsock);
  sleep_ms(100);
  CHECK(d.get_incomplete_count() == 1, "incomplete frame detected");
}

static void test_smaller_than_display() {
  puts("[64x64 frame on 128x128 display]");
  DDPDisplay d;
  d.set_size(64, 64);
  d.set_port(14049);
  d.set_mode(DDPMode::DDP);
  d.setup();
  FakeDisplay big(128, 128);
  d.loop();
  for (int i = 0; i < 10; i++)
    d.draw(big);
  CHECK(big.fills == 2, "outside area cleared only in the first 2 frames (both buffers)");
  d.set_mode(DDPMode::NORMAL);
  d.loop();
  d.set_mode(DDPMode::DDP);
  d.loop();
  for (int i = 0; i < 5; i++)
    d.draw(big);
  CHECK(big.fills == 4, "cleared again after re-activation");
}

static void test_slow_draw_no_loss() {
  puts("[128x128 bursts while drawing is slow]");
  DDPDisplay d;
  d.set_size(128, 128);
  d.set_port(14050);
  d.set_mode(DDPMode::DDP);
  d.setup();
  FakeDisplay slow(128, 128, 12);
  std::atomic<bool> run{true};
  std::thread drawer([&] {
    while (run) {
      d.loop();
      d.draw(slow);
      sleep_ms(28);
    }
  });
  std::vector<uint8_t> frame(128 * 128 * 3);
  for (int f = 0; f < 60; f++) {
    std::fill(frame.begin(), frame.end(), uint8_t(f * 4 + 1));
    send_frame(14050, frame.data(), frame.size());  // 35 packets as one burst
    sleep_ms(33);
  }
  sleep_ms(100);
  run = false;
  drawer.join();
  printf("  info frames=%u packets=%u incomplete=%u draws=%d\n", d.get_frame_count(), d.get_packet_count(),
         d.get_incomplete_count(), slow.bulk_calls);
  CHECK(d.get_frame_count() == 60, "all 60 frames received (RX never blocked by drawing)");
  CHECK(d.get_incomplete_count() == 0, "no packet loss");
  CHECK(slow.torn == 0, "no torn frames (triple buffering)");
}

int main() {
  test_protocol();
  test_modes_and_frames();
  test_smaller_than_display();
  test_slow_draw_no_loss();
  printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "ALL OK", failures, failures == 1 ? "" : "s");
  return failures ? 1 : 0;
}
