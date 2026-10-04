// Minimal stub of esphome/components/display/display.h for host tests.
#pragma once
#include <cstdint>
namespace esphome {
struct Color {
  uint8_t r, g, b;
  Color(uint8_t r, uint8_t g, uint8_t b) : r(r), g(g), b(b) {}
  static const Color BLACK;
};
inline const Color Color::BLACK(0, 0, 0);
namespace display {
enum ColorOrder : uint8_t { COLOR_ORDER_RGB = 0, COLOR_ORDER_BGR = 1, COLOR_ORDER_GRB = 2 };
enum ColorBitness : uint8_t { COLOR_BITNESS_888 = 0, COLOR_BITNESS_565 = 1, COLOR_BITNESS_332 = 2 };
class Display {
 public:
  virtual ~Display() = default;
  virtual int get_width() = 0;
  virtual int get_height() = 0;
  virtual void draw_pixel_at(int x, int y, Color color) = 0;
  virtual void fill(Color color) {}
  virtual void draw_pixels_at(int x_start, int y_start, int w, int h, const uint8_t *ptr, ColorOrder order,
                              ColorBitness bitness, bool big_endian, int x_offset, int y_offset, int x_pad) {}
};
}  // namespace display
}  // namespace esphome
