#pragma once
#include <cstddef>
#include <cstdint>
constexpr uint16_t TFT_BLACK = 0;
extern uint16_t host_pixels[240 * 320];

// Capture the real LVGL flush. No LCD, byte-order or touch-hardware emulation.
class TFT_eSPI {
 public:
  void begin() {}
  void setRotation(int) {}
  void setSwapBytes(bool) {}
  void startWrite() {}
  void endWrite() {}
  void fillScreen(uint16_t value) {
    for (auto& pixel : host_pixels) pixel = value;
  }
  void setAddrWindow(int x, int y, int width, int) { x_ = x; y_ = y; width_ = width; index_ = 0; }
  void pushPixels(uint16_t* pixels, size_t count) { pushColors(pixels, count, false); }
  void pushColors(uint16_t* pixels, size_t count, bool) {
    for (size_t i = 0; i < count; ++i, ++index_) {
      const int x = x_ + index_ % width_;
      const int y = y_ + index_ / width_;
      if (x >= 0 && x < 240 && y >= 0 && y < 320) host_pixels[y * 240 + x] = pixels[i];
    }
  }
 private:
  int x_ = 0, y_ = 0, width_ = 240, index_ = 0;
};
