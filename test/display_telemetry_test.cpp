#include <cassert>
#include <cstdio>
#include "display_telemetry.h"

int main() {
  ThermocoupleReading reading;
  reading.fault = ThermocoupleFault::None;
  reading.celsius = 22.5F;
  reading.internal_celsius = 26.25F;
  reading.sample_ms = 1001U;
  // Read finishes after loop start (1000): use the post-read clock (1002).
  assert(std::isnan(maxBoardTemperature(reading, 1000U)));
  assert(maxBoardTemperature(reading, 1002U) == 26.25F);
  assert(maxBoardTemperature(reading, 2001U) == 26.25F);
  assert(std::isnan(maxBoardTemperature(reading, 2002U)));
  reading.sample_ms = UINT32_MAX - 50U;
  assert(maxBoardTemperature(reading, 49U) == 26.25F);
  assert(std::isnan(maxBoardTemperature(reading, 1000U)));
  reading.fault = ThermocoupleFault::Bus;
  assert(std::isnan(maxBoardTemperature(reading, 49U)));
  reading.fault = ThermocoupleFault::None;
  reading.internal_celsius = NAN;
  assert(std::isnan(maxBoardTemperature(reading, 49U)));
  std::puts("Display telemetry post-read clock, expiry, wrap and faults passed");
}
