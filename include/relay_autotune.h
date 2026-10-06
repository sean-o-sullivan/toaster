#pragma once

#include <cstdint>
#include "pid_control.h"

enum TuneCheck : uint32_t {
  TuneSettling = 1U << 0,
  TuneNeedWindow = 1U << 1,
  TunePeriodLow = 1U << 2,
  TunePeriodHigh = 1U << 3,
  TuneAmplitudeLow = 1U << 4,
  TuneAmplitudeHigh = 1U << 5,
  TuneFractionLow = 1U << 6,
  TuneFractionHigh = 1U << 7,
  TuneMidpointOffset = 1U << 8,
  TunePeriodSpread = 1U << 9,
  TuneAmplitudeSpread = 1U << 10,
  TuneMidpointSpread = 1U << 11,
  TuneInvalidGains = 1U << 12,
  TuneInvalidMath = 1U << 13,
};

// Heuristic quality checks remain visible but do not block a usable estimate.
constexpr uint32_t kTuneQualityChecks =
    TunePeriodLow | TunePeriodHigh | TuneAmplitudeLow | TuneAmplitudeHigh |
    TuneFractionLow | TuneFractionHigh | TuneMidpointOffset | TunePeriodSpread |
    TuneAmplitudeSpread | TuneMidpointSpread;
constexpr uint32_t kTuneBlockingChecks =
    TuneSettling | TuneNeedWindow | TuneInvalidGains | TuneInvalidMath;

inline uint32_t tuneBlockingFailures(uint32_t checks) {
  return checks & kTuneBlockingChecks;
}

struct TuneCycleSample {
  uint8_t cycle = 0;
  float period = 0.0F;
  float heat_seconds = 0.0F;
  float fraction = 0.0F;
  float minimum = 0.0F;
  float maximum = 0.0F;
  float amplitude = 0.0F;
  float midpoint = 0.0F;
  uint32_t ssr_on_ms = 0;
  uint32_t failed_checks = 0;
};

struct TuneCycleDiagnostics {
  uint8_t cycle = 0;
  float period = 0.0F;
  float heat_seconds = 0.0F;
  float fraction = 0.0F;
  float minimum = 0.0F;
  float maximum = 0.0F;
  float amplitude = 0.0F;
  float midpoint = 0.0F;
  uint32_t ssr_on_ms = 0;
  uint32_t failed_checks = TuneNeedWindow;
  TuneCycleSample window[3];
  uint8_t window_count = 0;
  float period_ratio = 0.0F;
  float amplitude_ratio = 0.0F;
  float midpoint_span = 0.0F;
};

// Bounded 100 C experiment. The engine owns all heater/sensor safety checks.
class RelayAutotune {
 public:
  void begin(uint32_t now_ms, float temperature);
  void update(uint32_t now_ms, float temperature, bool heater_on = false);
  float demand() const { return ready_ || failed_ ? 0.0F : heating_ ? 25.0F : 0.0F; }
  bool ready() const { return ready_; }
  bool failed() const { return failed_; }
  uint8_t cycles() const { return cycles_; }
  const PidGains& gains() const { return gains_; }
  float periodSeconds() const { return period_seconds_; }
  float amplitudeCelsius() const { return amplitude_; }
  const TuneCycleDiagnostics& diagnostics() const { return diagnostics_; }
  uint32_t diagnosticSequence() const { return diagnostic_sequence_; }
  static uint32_t candidateFailure(const PidGains& gains) {
    return validPidGains(gains) ? 0U : TuneInvalidGains;
  }
  static bool estimateCandidate(const TuneCycleDiagnostics& diagnostic, PidGains& gains);

 private:
  uint32_t calculate(TuneCycleDiagnostics& diagnostic);
  TuneCycleSample recent_[3] = {};
  PidGains gains_;
  bool heating_ = true;
  bool started_cycle_ = false;
  bool ready_ = false;
  bool failed_ = false;
  uint8_t cycles_ = 0;
  uint32_t cycle_start_ = 0;
  uint32_t heat_start_ = 0;
  float minimum_ = 0.0F;
  float maximum_ = 0.0F;
  float period_seconds_ = 0.0F;
  float amplitude_ = 0.0F;
  uint32_t last_update_ms_ = 0;
  uint32_t cycle_ssr_on_ms_ = 0;
  uint32_t diagnostic_sequence_ = 0;
  TuneCycleDiagnostics diagnostics_;
};
