#include "relay_autotune.h"

#include <algorithm>
#include <cmath>

namespace {
constexpr float kPi = 3.14159265358979323846F;

bool estimateWindow(const TuneCycleSample* window, PidGains& gains,
                    float& period_seconds, float& amplitude_celsius) {
  float sum_period = 0.0F;
  float sum_amplitude = 0.0F;
  float sum_ku = 0.0F;
  for (uint8_t i = 0; i < 3; ++i) {
    const TuneCycleSample& c = window[i];
    if (!std::isfinite(c.period) || !std::isfinite(c.heat_seconds) ||
        !std::isfinite(c.fraction) || !std::isfinite(c.minimum) ||
        !std::isfinite(c.maximum) || !std::isfinite(c.amplitude) ||
        !std::isfinite(c.midpoint) || c.period <= 0.0F ||
        c.heat_seconds <= 0.0F || c.heat_seconds >= c.period ||
        c.maximum <= c.minimum) {
      return false;
    }
    const float fraction = c.heat_seconds / c.period;
    const float amplitude = (c.maximum - c.minimum) * 0.5F;
    const float ku = 50.0F * std::sin(kPi * fraction) / (kPi * amplitude);
    if (!std::isfinite(fraction) || fraction <= 0.0F || fraction >= 1.0F ||
        !std::isfinite(amplitude) || amplitude <= 0.0F ||
        !std::isfinite(ku) || ku <= 0.0F) {
      return false;
    }
    sum_period += c.period;
    sum_amplitude += amplitude;
    sum_ku += ku;
  }
  period_seconds = sum_period / 3.0F;
  amplitude_celsius = sum_amplitude / 3.0F;
  gains.kp = (sum_ku / 3.0F) / 2.2F;
  gains.ki = gains.kp / (2.2F * period_seconds);
  gains.kd = gains.kp * period_seconds / 6.3F;
  return std::isfinite(period_seconds) && period_seconds > 0.0F &&
         std::isfinite(amplitude_celsius) && amplitude_celsius > 0.0F &&
         std::isfinite(gains.kp) && std::isfinite(gains.ki) && std::isfinite(gains.kd);
}
}  // namespace

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
      if (cycles_ >= 5 && tuneBlockingFailures(diagnostics_.failed_checks) == 0U) {
        ready_ = true;
        return;
      }
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
  }
  diagnostic.period_ratio = min_period > 0.0F ? max_period / min_period : INFINITY;
  diagnostic.amplitude_ratio = min_amplitude > 0.0F ? max_amplitude / min_amplitude : INFINITY;
  diagnostic.midpoint_span = max_midpoint - min_midpoint;
  if (max_period > min_period * 1.2F) failures |= TunePeriodSpread;
  if (max_amplitude > min_amplitude * 1.2F) failures |= TuneAmplitudeSpread;
  if (max_midpoint - min_midpoint > 1.0F) failures |= TuneMidpointSpread;
  PidGains candidate;
  // Tyreus-Luyben, parallel form. Quality heuristics warn; valid math blocks.
  if (!estimateWindow(recent_, candidate, period_seconds_, amplitude_)) {
    failures |= TuneInvalidMath;
  } else {
    failures |= candidateFailure(candidate);
    if (candidateFailure(candidate) == 0U) gains_ = candidate;
  }
  return failures;
}

bool RelayAutotune::estimateCandidate(const TuneCycleDiagnostics& diagnostic, PidGains& gains) {
  if (diagnostic.window_count != 3U) return false;
  PidGains candidate;
  float period_seconds = 0.0F;
  float amplitude_celsius = 0.0F;
  if (!estimateWindow(diagnostic.window, candidate, period_seconds, amplitude_celsius) ||
      candidateFailure(candidate) != 0U) {
    return false;
  }
  gains = candidate;
  return true;
}
