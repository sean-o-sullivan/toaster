#include "touch_ft6336.h"

#include <Arduino.h>
#include <Wire.h>

#include "board_pins.h"

namespace {

constexpr uint8_t kTouchStatusRegister = 0x02;
constexpr uint16_t kDisplayWidth = 240;
constexpr uint16_t kDisplayHeight = 320;

}  // namespace

bool Ft6336Touch::begin() {
  pinMode(board::kTouchReset, OUTPUT);
  digitalWrite(board::kTouchReset, LOW);
  delay(5);
  digitalWrite(board::kTouchReset, HIGH);
  delay(50);

  pinMode(board::kTouchInterrupt, INPUT_PULLUP);
  Wire.begin(board::kTouchSda, board::kTouchScl, 400000U);

  Wire.beginTransmission(board::kTouchAddress);
  return Wire.endTransmission() == 0;
}

bool Ft6336Touch::read(TouchPoint& point) {
  Wire.beginTransmission(board::kTouchAddress);
  Wire.write(kTouchStatusRegister);
  if (Wire.endTransmission(false) != 0) {
    return false;
  }

  constexpr uint8_t kReadLength = 5;
  if (Wire.requestFrom(board::kTouchAddress, kReadLength) != kReadLength) {
    return false;
  }

  const uint8_t touch_count = Wire.read() & 0x0F;
  const uint8_t x_high = Wire.read();
  const uint8_t x_low = Wire.read();
  const uint8_t y_high = Wire.read();
  const uint8_t y_low = Wire.read();
  if (touch_count == 0U) {
    return false;
  }

  const uint16_t x = static_cast<uint16_t>(((x_high & 0x0FU) << 8U) | x_low);
  const uint16_t y = static_cast<uint16_t>(((y_high & 0x0FU) << 8U) | y_low);
  if (x >= kDisplayWidth || y >= kDisplayHeight) {
    return false;
  }

  point.x = x;
  point.y = y;
  return true;
}
