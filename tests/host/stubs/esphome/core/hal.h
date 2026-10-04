// Minimal stub for host tests.
#pragma once
#include <chrono>
#include <cstdint>
namespace esphome { inline uint32_t millis(){ return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); } }
