#pragma once

#include <cstdint>
#include <cmath>

enum class ThermocoupleFault : uint8_t {
  None = 0,
  Bus,
  InvalidFrame,
  OpenCircuit,
  ShortToGround,
  ShortToVcc,
};

struct ThermocoupleReading {
  float celsius = 0.0F;
  float internal_celsius = 0.0F;
  ThermocoupleFault fault = ThermocoupleFault::Bus;
  uint32_t sample_ms = 0;

  bool valid() const {
    return fault == ThermocoupleFault::None && std::isfinite(celsius) &&
           std::isfinite(internal_celsius);
  }
};
