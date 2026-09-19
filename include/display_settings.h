#pragma once

#include <cstdint>

constexpr uint8_t kMinimumBrightnessPercent = 5;
constexpr uint8_t kDefaultBrightnessPercent = 100;

inline bool validBrightnessPercent(uint8_t value) {
  return value >= kMinimumBrightnessPercent && value <= 100;
}

// Separate namespace from control settings. Missing/corrupt values stay visible.
template <typename Storage>
uint8_t loadDisplayBrightness(Storage& storage) {
  if (!storage.begin("toaster-ui", true)) return kDefaultBrightnessPercent;
  const uint8_t value = storage.getUChar("brightness", kDefaultBrightnessPercent);
  storage.end();
  return validBrightnessPercent(value) ? value : kDefaultBrightnessPercent;
}

// One write per completed adjustment, never on individual drag samples.
template <typename Storage>
bool saveDisplayBrightness(Storage& storage, uint8_t value) {
  if (!validBrightnessPercent(value) || !storage.begin("toaster-ui", false)) return false;
  const bool saved = storage.putUChar("brightness", value) == sizeof(value) &&
      storage.getUChar("brightness", 0) == value;
  storage.end();
  return saved;
}
