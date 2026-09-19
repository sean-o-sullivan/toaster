#pragma once

#include "thermocouple_types.h"

// now_ms must be captured after the sensor read completes.
inline float maxBoardTemperature(const ThermocoupleReading& reading, uint32_t now_ms) {
  return reading.valid() && now_ms - reading.sample_ms <= 1000U
      ? reading.internal_celsius : NAN;
}
