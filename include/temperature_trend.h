#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>

enum class TrendState {
  WarmingUp,
  Stable,
  Rising,
  Falling,
};

class TemperatureTrend {
 public:
  TemperatureTrend() { reset(); }

  void reset() {
    start_ = 0U;
    count_ = 0U;
    have_call_ = false;
    last_call_ms_ = 0U;
    rate_celsius_per_minute_ = 0.0F;
    state_ = TrendState::WarmingUp;
  }

  void update(uint32_t now_ms, float celsius, bool healthy) {
    if (!healthy || !std::isfinite(celsius)) {
      reset();
      return;
    }

    if (have_call_ && elapsed(now_ms, last_call_ms_) > kMaximumCallGapMs) {
      reset();
    }
    have_call_ = true;
    last_call_ms_ = now_ms;

    if (count_ != 0U && elapsed(now_ms, newest().time_ms) < kMinimumSampleSpacingMs) {
      return;
    }

    append(now_ms, celsius);
    discardExpired(now_ms);
    calculateRateAndState();
  }

  bool ready() const {
    return count_ >= 2U && elapsed(newest().time_ms, oldest().time_ms) >= kReadyHistoryMs;
  }

  float rateCelsiusPerMinute() const { return rate_celsius_per_minute_; }

  TrendState state() const { return state_; }

 private:
  struct Sample {
    uint32_t time_ms;
    float celsius;
  };

  static constexpr std::size_t kCapacity = 31U;
  static constexpr uint32_t kMinimumSampleSpacingMs = 1000U;
  static constexpr uint32_t kMaximumCallGapMs = 1500U;
  static constexpr uint32_t kReadyHistoryMs = 10000U;
  static constexpr uint32_t kWindowMs = 30000U;
  static constexpr float kEnterRate = 0.5F;
  static constexpr float kExitRate = 0.3F;
  static constexpr float kThresholdTolerance = 0.0001F;

  static uint32_t elapsed(uint32_t later, uint32_t earlier) { return later - earlier; }

  const Sample& oldest() const { return samples_[start_]; }

  const Sample& newest() const {
    return samples_[(start_ + count_ - 1U) % kCapacity];
  }

  void append(uint32_t now_ms, float celsius) {
    if (count_ == kCapacity) {
      start_ = (start_ + 1U) % kCapacity;
      --count_;
    }
    samples_[(start_ + count_) % kCapacity] = {now_ms, celsius};
    ++count_;
  }

  void discardExpired(uint32_t now_ms) {
    while (count_ != 0U && elapsed(now_ms, oldest().time_ms) > kWindowMs) {
      start_ = (start_ + 1U) % kCapacity;
      --count_;
    }
  }

  void calculateRateAndState() {
    if (!ready()) {
      rate_celsius_per_minute_ = 0.0F;
      state_ = TrendState::WarmingUp;
      return;
    }

    const uint32_t origin_ms = oldest().time_ms;
    double sum_time_s = 0.0;
    double sum_temperature = 0.0;
    for (std::size_t i = 0U; i < count_; ++i) {
      const Sample& sample = samples_[(start_ + i) % kCapacity];
      sum_time_s += static_cast<double>(elapsed(sample.time_ms, origin_ms)) / 1000.0;
      sum_temperature += static_cast<double>(sample.celsius);
    }

    const double mean_time_s = sum_time_s / static_cast<double>(count_);
    const double mean_temperature = sum_temperature / static_cast<double>(count_);
    double covariance = 0.0;
    double time_variance = 0.0;
    for (std::size_t i = 0U; i < count_; ++i) {
      const Sample& sample = samples_[(start_ + i) % kCapacity];
      const double centered_time =
          static_cast<double>(elapsed(sample.time_ms, origin_ms)) / 1000.0 - mean_time_s;
      covariance += centered_time * (static_cast<double>(sample.celsius) - mean_temperature);
      time_variance += centered_time * centered_time;
    }
    rate_celsius_per_minute_ =
        static_cast<float>((covariance / time_variance) * 60.0);

    switch (state_) {
      case TrendState::WarmingUp:
      case TrendState::Stable:
        if (rate_celsius_per_minute_ >= kEnterRate - kThresholdTolerance) {
          state_ = TrendState::Rising;
        } else if (rate_celsius_per_minute_ <= -kEnterRate + kThresholdTolerance) {
          state_ = TrendState::Falling;
        } else {
          state_ = TrendState::Stable;
        }
        break;
      case TrendState::Rising:
        if (rate_celsius_per_minute_ <= -kEnterRate + kThresholdTolerance) {
          state_ = TrendState::Falling;
        } else if (rate_celsius_per_minute_ <= kExitRate + kThresholdTolerance) {
          state_ = TrendState::Stable;
        }
        break;
      case TrendState::Falling:
        if (rate_celsius_per_minute_ >= kEnterRate - kThresholdTolerance) {
          state_ = TrendState::Rising;
        } else if (rate_celsius_per_minute_ >= -kExitRate - kThresholdTolerance) {
          state_ = TrendState::Stable;
        }
        break;
    }
  }

  Sample samples_[kCapacity]{};
  std::size_t start_ = 0U;
  std::size_t count_ = 0U;
  bool have_call_ = false;
  uint32_t last_call_ms_ = 0U;
  float rate_celsius_per_minute_ = 0.0F;
  TrendState state_ = TrendState::WarmingUp;
};
