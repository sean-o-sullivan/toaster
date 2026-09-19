#pragma once

#include <cmath>
#include <cstdint>
#include <limits>

struct AnnealProgram {
  float target_celsius = 0.0F;
  uint32_t soak_seconds = 0;
  float ramp_celsius_per_minute = 0.0F;
};

constexpr float kAnnealMinimumTargetCelsius = 60.0F;
constexpr float kAnnealMaximumTargetCelsius = 180.0F;
constexpr uint32_t kAnnealMinimumSoakSeconds = 60U;
constexpr uint32_t kAnnealMaximumSoakSeconds = 6600U;
constexpr float kAnnealMinimumRampCelsiusPerMinute = 1.0F;
constexpr float kAnnealMaximumRampCelsiusPerMinute = 60.0F;
constexpr float kAnnealCooldownTargetCelsius = 60.0F;
constexpr float kAnnealMaximumProcessCelsius = 200.0F;
constexpr uint32_t kAnnealMaximumRunSeconds = 7200U;
constexpr float kAnnealMaximumOutputPercent = 100.0F;
constexpr uint32_t kInvalidAnnealEstimateSeconds =
    std::numeric_limits<uint32_t>::max();

inline bool validAnnealProgram(const AnnealProgram& program) {
  return std::isfinite(program.target_celsius) &&
         std::isfinite(program.ramp_celsius_per_minute) &&
         program.target_celsius >= kAnnealMinimumTargetCelsius &&
         program.target_celsius <= kAnnealMaximumTargetCelsius &&
         program.soak_seconds >= kAnnealMinimumSoakSeconds &&
         program.soak_seconds <= kAnnealMaximumSoakSeconds &&
         program.ramp_celsius_per_minute >= kAnnealMinimumRampCelsiusPerMinute &&
         program.ramp_celsius_per_minute <= kAnnealMaximumRampCelsiusPerMinute;
}

inline uint32_t annealDurationSeconds(float delta_celsius,
                                      float ramp_celsius_per_minute) {
  if (!std::isfinite(delta_celsius) || !std::isfinite(ramp_celsius_per_minute) ||
      delta_celsius < 0.0F || ramp_celsius_per_minute <= 0.0F) {
    return kInvalidAnnealEstimateSeconds;
  }
  const double seconds = std::ceil(
      static_cast<double>(delta_celsius) * 60.0 /
      static_cast<double>(ramp_celsius_per_minute));
  if (!std::isfinite(seconds) ||
      seconds > static_cast<double>(kInvalidAnnealEstimateSeconds - 1U)) {
    return kInvalidAnnealEstimateSeconds;
  }
  return static_cast<uint32_t>(seconds);
}

inline uint32_t annealRampSeconds(const AnnealProgram& program,
                                  float current_celsius) {
  if (!validAnnealProgram(program) || !std::isfinite(current_celsius) ||
      current_celsius > program.target_celsius) {
    return kInvalidAnnealEstimateSeconds;
  }
  return annealDurationSeconds(program.target_celsius - current_celsius,
                               program.ramp_celsius_per_minute);
}

inline uint32_t annealCoolingSeconds(const AnnealProgram& program) {
  if (!validAnnealProgram(program)) {
    return kInvalidAnnealEstimateSeconds;
  }
  return annealDurationSeconds(
      program.target_celsius - kAnnealCooldownTargetCelsius,
      program.ramp_celsius_per_minute);
}

inline uint32_t annealEstimatedTotalSeconds(const AnnealProgram& program,
                                            float current_celsius) {
  const uint32_t ramp_seconds = annealRampSeconds(program, current_celsius);
  const uint32_t cooling_seconds = annealCoolingSeconds(program);
  if (ramp_seconds == kInvalidAnnealEstimateSeconds ||
      cooling_seconds == kInvalidAnnealEstimateSeconds) {
    return kInvalidAnnealEstimateSeconds;
  }
  const uint64_t total = static_cast<uint64_t>(ramp_seconds) +
                         static_cast<uint64_t>(program.soak_seconds) +
                         static_cast<uint64_t>(cooling_seconds);
  return total < kInvalidAnnealEstimateSeconds
             ? static_cast<uint32_t>(total)
             : kInvalidAnnealEstimateSeconds;
}

inline bool annealFitsBudget(const AnnealProgram& program,
                             float current_celsius) {
  const uint32_t estimated_seconds =
      annealEstimatedTotalSeconds(program, current_celsius);
  return estimated_seconds != kInvalidAnnealEstimateSeconds &&
         estimated_seconds < kAnnealMaximumRunSeconds;
}
