#include "relay_autotune.h"

#include <algorithm>
#include <cmath>

void RelayAutotune::begin(uint32_t now_ms, float temperature) {
  *this = RelayAutotune{};
  cycle_start_ = heat_start_ = now_ms;
  last_update_ms_ = now_ms;
  minimum_ = maximum_ = temperature;
}

void RelayAutotune::update(uint32_t now_ms, float temperature, bool heater_on) {
  if (ready_ || failed_) return;
  if (!std::isfinite(temperature)) { failed_ = true; return; }
  const uint32_t interval_ms = now_ms - last_update_ms_;
  if (heater_on) cycle_ssr_on_ms_ += interval_ms;
  last_update_ms_ = now_ms;
  minimum_ = std::min(minimum_, temperature);
  maximum_ = std::max(maximum_, temperature);
  if (heating_ && temperature >= 100.5F) {
    heating_ = false;
    if (started_cycle_) {
      const float period = static_cast<float>(now_ms - cycle_start_) / 1000.0F;
      const float heat_seconds = static_cast<float>(now_ms - heat_start_) / 1000.0F;
      if (period <= 0.0F) { failed_ = true; return; }
      TuneCycleSample cycle{};
      cycle.cycle = cycles_ + 1U;
      cycle.period = period;
      cycle.heat_seconds = heat_seconds;
      cycle.fraction = heat_seconds / period;
      cycle.minimum = minimum_;
      cycle.maximum = maximum_;
      cycle.amplitude = (maximum_ - minimum_) * 0.5F;
      cycle.midpoint = (maximum_ + minimum_) * 0.5F;
      cycle.ssr_on_ms = cycle_ssr_on_ms_;
      if (cycle.period < 40.0F) cycle.failed_checks |= TunePeriodLow;
      if (cycle.period > 300.0F) cycle.failed_checks |= TunePeriodHigh;
      if (cycle.amplitude < 2.0F) cycle.failed_checks |= TuneAmplitudeLow;
      if (cycle.amplitude > 12.0F) cycle.failed_checks |= TuneAmplitudeHigh;
      if (cycle.fraction < 0.3F) cycle.failed_checks |= TuneFractionLow;
      if (cycle.fraction > 0.7F) cycle.failed_checks |= TuneFractionHigh;
      if (std::fabs(cycle.midpoint - 100.0F) > 4.0F) cycle.failed_checks |= TuneMidpointOffset;
      // Discard two settling cycles; use the last three complete cycles.
      if (cycles_ >= 2) recent_[(cycles_ - 2) % 3] = cycle;
      ++cycles_;
      diagnostics_ = {};
      diagnostics_.cycle = cycles_;
      diagnostics_.period = period;
      diagnostics_.heat_seconds = heat_seconds;
      diagnostics_.fraction = cycle.fraction;
      diagnostics_.minimum = minimum_;
      diagnostics_.maximum = maximum_;
      diagnostics_.amplitude = cycle.amplitude;
      diagnostics_.midpoint = cycle.midpoint;
      diagnostics_.ssr_on_ms = cycle_ssr_on_ms_;
      diagnostics_.failed_checks = cycles_ <= 2 ? TuneSettling : TuneNeedWindow;
      if (cycles_ >= 5) diagnostics_.failed_checks = calculate(diagnostics_);
      ++diagnostic_sequence_;
      cycle_ssr_on_ms_ = 0;
      if (cycles_ >= 5 && diagnostics_.failed_checks == 0U) { ready_ = true; return; }
      if (cycles_ >= 10) { failed_ = true; return; }
    }
    started_cycle_ = true;
    cycle_start_ = now_ms;
    cycle_ssr_on_ms_ = 0;
    minimum_ = maximum_ = temperature;
  } else if (!heating_ && temperature <= 99.5F) {
    heating_ = true;
    heat_start_ = now_ms;
  }
}

uint32_t RelayAutotune::calculate(TuneCycleDiagnostics& diagnostic) {
  float min_period = recent_[0].period, max_period = min_period;
  float min_amplitude = recent_[0].amplitude, max_amplitude = min_amplitude;
  float min_midpoint = recent_[0].midpoint, max_midpoint = min_midpoint;
  float sum_period = 0.0F, sum_amplitude = 0.0F, sum_ku = 0.0F;
  constexpr float pi = 3.14159265358979323846F;
  uint32_t failures = 0;
  diagnostic.window_count = 3;
  for (uint8_t i = 0; i < 3; ++i) diagnostic.window[i] = recent_[i];
  for (const auto& c : recent_) {
    // Reject tiny/noisy, burst-scale, highly asymmetric or drifting cycles.
    failures |= c.failed_checks;
    min_period = std::min(min_period, c.period);
    max_period = std::max(max_period, c.period);
    min_amplitude = std::min(min_amplitude, c.amplitude);
    max_amplitude = std::max(max_amplitude, c.amplitude);
    min_midpoint = std::min(min_midpoint, c.midpoint);
    max_midpoint = std::max(max_midpoint, c.midpoint);
    sum_period += c.period;
    sum_amplitude += c.amplitude;
    // Fundamental of the 0/25% relay; correct for measured unequal on/off time.
    if (c.amplitude > 0.0F)
      sum_ku += 50.0F * std::sin(pi * c.fraction) / (pi * c.amplitude);
  }
  diagnostic.period_ratio = min_period > 0.0F ? max_period / min_period : INFINITY;
  diagnostic.amplitude_ratio = min_amplitude > 0.0F ? max_amplitude / min_amplitude : INFINITY;
  diagnostic.midpoint_span = max_midpoint - min_midpoint;
  if (max_period > min_period * 1.2F) failures |= TunePeriodSpread;
  if (max_amplitude > min_amplitude * 1.2F) failures |= TuneAmplitudeSpread;
  if (max_midpoint - min_midpoint > 1.0F) failures |= TuneMidpointSpread;
  period_seconds_ = sum_period / 3.0F;
  amplitude_ = sum_amplitude / 3.0F;
  // Tyreus-Luyben, parallel form, output in percent and time in seconds.
  if (failures == 0U) {
    gains_.kp = (sum_ku / 3.0F) / 2.2F;
    gains_.ki = gains_.kp / (2.2F * period_seconds_);
    gains_.kd = gains_.kp * period_seconds_ / 6.3F;
  }
  failures |= candidateFailure(gains_);
  return failures;
}
