#include "max31855_decode.h"

namespace {

int16_t signExtend(uint16_t value, uint8_t bits) {
  const int32_t sign = 1L << (bits - 1);
  return static_cast<int16_t>((static_cast<int32_t>(value) ^ sign) - sign);
}

}  // namespace

ThermocoupleReading decodeMax31855(uint32_t raw, uint32_t sample_ms) {
  ThermocoupleReading reading;
  reading.sample_ms = sample_ms;
  reading.fault = ThermocoupleFault::InvalidFrame;

  // Fail closed: both junctions at exactly 0 C is ambiguous with a stuck-low bus.
  constexpr uint32_t kReservedBits = (1UL << 17U) | (1UL << 3U);
  if (raw == 0U || raw == UINT32_MAX || (raw & kReservedBits) != 0U) {
    return reading;
  }

  const bool fault_flag = (raw & (1UL << 16U)) != 0U;
  const uint32_t fault_bits = raw & 0x7U;
  if (fault_flag != (fault_bits != 0U)) {
    return reading;
  }
  if (fault_flag) {
    reading.fault = (fault_bits & 0x1U) ? ThermocoupleFault::OpenCircuit
                    : (fault_bits & 0x2U) ? ThermocoupleFault::ShortToGround
                                         : ThermocoupleFault::ShortToVcc;
    return reading;
  }

  reading.celsius = signExtend(static_cast<uint16_t>((raw >> 18U) & 0x3FFFU), 14) * 0.25F;
  reading.internal_celsius =
      signExtend(static_cast<uint16_t>((raw >> 4U) & 0xFFFU), 12) * 0.0625F;
  // MAX31855K measurement range and IC operating range, not oven safety limits.
  if (reading.celsius < -200.0F || reading.celsius > 1350.0F ||
      reading.internal_celsius < -40.0F || reading.internal_celsius > 125.0F) {
    return reading;
  }
  reading.fault = ThermocoupleFault::None;
  return reading;
}
