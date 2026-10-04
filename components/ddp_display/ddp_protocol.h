#pragma once

// DDP (Distributed Display Protocol) packet parsing.
// Spec: http://www.3waylabs.com/ddp/
// No ESPHome dependencies, so it can be unit-tested on a PC.

#include <cstddef>
#include <cstdint>

namespace esphome {
namespace ddp_display {

static constexpr uint8_t DDP_FLAG_VER_MASK = 0xC0;
static constexpr uint8_t DDP_FLAG_VER1 = 0x40;
static constexpr uint8_t DDP_FLAG_TIMECODE = 0x10;
static constexpr uint8_t DDP_FLAG_STORAGE = 0x08;
static constexpr uint8_t DDP_FLAG_REPLY = 0x04;
static constexpr uint8_t DDP_FLAG_QUERY = 0x02;
static constexpr uint8_t DDP_FLAG_PUSH = 0x01;

static constexpr uint8_t DDP_ID_DISPLAY = 1;  // default output device
static constexpr uint8_t DDP_ID_ALL = 255;    // broadcast to all devices

static constexpr size_t DDP_HEADER_LEN = 10;
static constexpr size_t DDP_HEADER_LEN_TIMECODE = 14;

struct DDPPacket {
  uint32_t offset;      // byte offset into the RGB buffer
  uint16_t length;      // payload length in bytes
  const uint8_t *data;  // points into the receive buffer
  bool push;            // "show the frame now"
};

// Returns false if the packet is malformed or not meant for us (query, reply,
// config/status IDs...). On success fills `out`.
inline bool parse_ddp(const uint8_t *buf, size_t len, DDPPacket &out) {
  if (buf == nullptr || len < DDP_HEADER_LEN)
    return false;

  const uint8_t flags = buf[0];
  if ((flags & DDP_FLAG_VER_MASK) != DDP_FLAG_VER1)
    return false;
  if (flags & (DDP_FLAG_QUERY | DDP_FLAG_REPLY | DDP_FLAG_STORAGE))
    return false;

  const uint8_t id = buf[3];
  if (id != DDP_ID_DISPLAY && id != DDP_ID_ALL)
    return false;

  const size_t header = (flags & DDP_FLAG_TIMECODE) ? DDP_HEADER_LEN_TIMECODE : DDP_HEADER_LEN;
  if (len < header)
    return false;

  const uint32_t offset =
      (uint32_t(buf[4]) << 24) | (uint32_t(buf[5]) << 16) | (uint32_t(buf[6]) << 8) | uint32_t(buf[7]);
  const uint16_t length = uint16_t((uint16_t(buf[8]) << 8) | buf[9]);
  if (size_t(length) > len - header)
    return false;  // truncated packet

  out.offset = offset;
  out.length = length;
  out.data = buf + header;
  out.push = (flags & DDP_FLAG_PUSH) != 0;
  return true;
}

}  // namespace ddp_display
}  // namespace esphome
