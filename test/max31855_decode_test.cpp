#include <cassert>
#include <cstdio>
#include <initializer_list>

#include "max31855_decode.h"

namespace {

uint32_t frame(float external, float internal) {
  const uint32_t tc = static_cast<uint32_t>(static_cast<int32_t>(external * 4.0F)) & 0x3FFFU;
  const uint32_t cj = static_cast<uint32_t>(static_cast<int32_t>(internal * 16.0F)) & 0xFFFU;
  return (tc << 18U) | (cj << 4U);
}

}  // namespace

int main() {
  // Captured on the physical bench: valid room reading and saturated bad frames.
  const auto observed = decodeMax31855(0x01881750U, 100);
  assert(observed.valid());
  assert(observed.celsius == 24.5F);
  assert(observed.internal_celsius == 23.3125F);
  assert(!decodeMax31855(0x7FFC7FF0U, 100).valid());
  assert(!decodeMax31855(0x7FFC8000U, 100).valid());
  const float temperatures[] = {-200.0F, -1.0F, -0.25F, 0.0F, 25.0F, 100.75F, 1350.0F};
  for (float temperature : temperatures) {
    const auto reading = decodeMax31855(frame(temperature, 25.0F), 1234);
    assert(reading.valid());
    assert(reading.celsius == temperature);
    assert(reading.internal_celsius == 25.0F);
    assert(reading.sample_ms == 1234);
  }
  for (float cold : {-40.0F, -0.0625F, 0.0F, 100.5625F, 125.0F}) {
    const auto reading = decodeMax31855(frame(25.0F, cold), 1);
    assert(reading.valid());
    assert(reading.internal_celsius == cold);
  }

  const uint32_t valid = frame(100.75F, 25.0F);
  const uint32_t invalid[] = {0U, UINT32_MAX, valid | (1U << 17U), valid | (1U << 3U),
                              valid | (1U << 16U), valid | 1U, valid | 2U, valid | 4U,
                              frame(-200.25F, 25.0F), frame(1350.25F, 25.0F),
                              frame(25.0F, -40.0625F), frame(25.0F, 125.0625F)};
  for (uint32_t raw : invalid) {
    const auto reading = decodeMax31855(raw, 50);
    assert(!reading.valid());
    assert(reading.fault == ThermocoupleFault::InvalidFrame);
    assert(reading.sample_ms == 50);
  }
  for (uint32_t bits = 1; bits <= 7; ++bits) {
    const auto reading = decodeMax31855(valid | (1U << 16U) | bits, 60);
    assert(!reading.valid());
    assert(reading.fault == ((bits & 1U) ? ThermocoupleFault::OpenCircuit
                           : (bits & 2U) ? ThermocoupleFault::ShortToGround
                                         : ThermocoupleFault::ShortToVcc));
  }
  std::puts("MAX31855 decoder tests passed");
}
