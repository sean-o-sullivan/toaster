#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>

#include "temperature_trend.h"

namespace {

bool near(float actual, float expected, float tolerance = 0.01F) {
  return std::fabs(actual - expected) <= tolerance;
}

float linearTemperature(float initial, float rate_per_minute, uint32_t elapsed_ms) {
  return initial + rate_per_minute * static_cast<float>(elapsed_ms) / 60000.0F;
}

void addLinear(TemperatureTrend& trend, uint32_t start_ms, uint32_t duration_ms,
               uint32_t step_ms, float initial, float rate_per_minute) {
  for (uint32_t elapsed_ms = 0U; elapsed_ms <= duration_ms; elapsed_ms += step_ms) {
    trend.update(start_ms + elapsed_ms,
                 linearTemperature(initial, rate_per_minute, elapsed_ms), true);
  }
}

void testWarmupAndSteady() {
  TemperatureTrend trend;
  assert(!trend.ready());
  assert(trend.state() == TrendState::WarmingUp);
  for (uint32_t second = 0U; second < 10U; ++second) {
    trend.update(second * 1000U, 40.0F, true);
    assert(!trend.ready());
    assert(trend.state() == TrendState::WarmingUp);
    assert(trend.rateCelsiusPerMinute() == 0.0F);
  }
  trend.update(10000U, 40.0F, true);
  assert(trend.ready());
  assert(near(trend.rateCelsiusPerMinute(), 0.0F));
  assert(trend.state() == TrendState::Stable);
}

void testQuantizedNoiseIsStable() {
  TemperatureTrend trend;
  const float readings[] = {100.00F, 100.25F, 100.00F, 99.75F};
  for (uint32_t second = 0U; second <= 30U; ++second) {
    trend.update(second * 1000U, readings[second % 4U], true);
  }
  assert(trend.ready());
  assert(std::fabs(trend.rateCelsiusPerMinute()) < 0.3F);
  assert(trend.state() == TrendState::Stable);
}

void testLinearRates() {
  TemperatureTrend rising;
  addLinear(rising, 0U, 30000U, 1000U, 20.0F, 2.4F);
  assert(near(rising.rateCelsiusPerMinute(), 2.4F));
  assert(rising.state() == TrendState::Rising);

  TemperatureTrend falling;
  addLinear(falling, 0U, 30000U, 1000U, 80.0F, -0.8F);
  assert(near(falling.rateCelsiusPerMinute(), -0.8F));
  assert(falling.state() == TrendState::Falling);
}

void testIrregularTimingAndDuplicateUpdates() {
  TemperatureTrend trend;
  const uint32_t times[] = {0U, 1200U, 2600U, 3700U, 5000U, 6200U, 7500U, 8900U, 10000U};
  for (uint32_t time_ms : times) {
    trend.update(time_ms, linearTemperature(30.0F, 1.2F, time_ms), true);
  }
  assert(trend.ready());
  assert(near(trend.rateCelsiusPerMinute(), 1.2F));

  TemperatureTrend duplicates;
  for (uint32_t second = 0U; second <= 10U; ++second) {
    const uint32_t time_ms = second * 1000U;
    duplicates.update(time_ms, linearTemperature(30.0F, 1.2F, time_ms), true);
    duplicates.update(time_ms + 500U, 500.0F, true);
  }
  assert(duplicates.ready());
  assert(near(duplicates.rateCelsiusPerMinute(), 1.2F));
}

void testResetsAndReconnect() {
  TemperatureTrend trend;
  addLinear(trend, 0U, 10000U, 1000U, 20.0F, 1.0F);
  assert(trend.ready());
  trend.update(12000U, 50.0F, true);
  assert(!trend.ready());
  assert(trend.state() == TrendState::WarmingUp);
  addLinear(trend, 12000U, 10000U, 1000U, 50.0F, -1.0F);
  assert(trend.ready());
  assert(trend.state() == TrendState::Falling);

  trend.update(23000U, 0.0F, false);
  assert(!trend.ready());
  trend.update(24000U, 60.0F, true);
  assert(!trend.ready());
  trend.update(25000U, std::numeric_limits<float>::infinity(), true);
  assert(!trend.ready());
  trend.update(26000U, std::numeric_limits<float>::quiet_NaN(), true);
  assert(!trend.ready());
  addLinear(trend, 27000U, 10000U, 1000U, 60.0F, 0.0F);
  assert(trend.ready());
  trend.reset();
  assert(!trend.ready());
  assert(trend.rateCelsiusPerMinute() == 0.0F);
}

void testTimestampRollover() {
  TemperatureTrend trend;
  const uint32_t start = UINT32_MAX - 4999U;
  for (uint32_t elapsed_ms = 0U; elapsed_ms <= 12000U; elapsed_ms += 1000U) {
    trend.update(start + elapsed_ms, linearTemperature(10.0F, 1.5F, elapsed_ms), true);
  }
  assert(trend.ready());
  assert(near(trend.rateCelsiusPerMinute(), 1.5F));
  assert(trend.state() == TrendState::Rising);
}

void testRollingWindow() {
  TemperatureTrend trend;
  for (uint32_t second = 0U; second <= 30U; ++second) {
    trend.update(second * 1000U, 20.0F, true);
  }
  assert(trend.state() == TrendState::Stable);
  for (uint32_t second = 31U; second <= 61U; ++second) {
    const uint32_t ramp_elapsed_ms = (second - 31U) * 1000U;
    trend.update(second * 1000U, linearTemperature(20.0F, 3.0F, ramp_elapsed_ms), true);
  }
  assert(near(trend.rateCelsiusPerMinute(), 3.0F));
  assert(trend.state() == TrendState::Rising);
}

void feedRate(TemperatureTrend& trend, uint32_t& now_ms, float& temperature,
              float rate_per_minute) {
  for (uint32_t second = 0U; second < 31U; ++second) {
    now_ms += 1000U;
    temperature += rate_per_minute / 60.0F;
    trend.update(now_ms, temperature, true);
  }
}

void testThresholdHysteresis() {
  TemperatureTrend trend;
  uint32_t now_ms = 0U;
  float temperature = 20.0F;
  trend.update(now_ms, temperature, true);

  feedRate(trend, now_ms, temperature, 0.49F);
  assert(trend.state() == TrendState::Stable);
  feedRate(trend, now_ms, temperature, 0.50F);
  assert(trend.state() == TrendState::Rising);
  feedRate(trend, now_ms, temperature, 0.40F);
  assert(trend.state() == TrendState::Rising);
  feedRate(trend, now_ms, temperature, 0.30F);
  assert(trend.state() == TrendState::Stable);
  feedRate(trend, now_ms, temperature, -0.50F);
  assert(trend.state() == TrendState::Falling);
  feedRate(trend, now_ms, temperature, -0.40F);
  assert(trend.state() == TrendState::Falling);
  feedRate(trend, now_ms, temperature, -0.30F);
  assert(trend.state() == TrendState::Stable);
  feedRate(trend, now_ms, temperature, 0.8F);
  assert(trend.state() == TrendState::Rising);
  feedRate(trend, now_ms, temperature, -0.8F);
  assert(trend.state() == TrendState::Falling);
}

}  // namespace

int main() {
  testWarmupAndSteady();
  testQuantizedNoiseIsStable();
  testLinearRates();
  testIrregularTimingAndDuplicateUpdates();
  testResetsAndReconnect();
  testTimestampRollover();
  testRollingWindow();
  testThresholdHysteresis();
  std::puts("Temperature trend tests passed");
}
