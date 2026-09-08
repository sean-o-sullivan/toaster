#pragma once

#include <cstdint>

struct TouchPoint {
  uint16_t x = 0;
  uint16_t y = 0;
};

class Ft6336Touch {
 public:
  bool begin();
  bool read(TouchPoint& point);
};
