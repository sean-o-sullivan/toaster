#pragma once

#include <algorithm>
#include <cmath>

struct PidGains {
  float kp = 2.4F;
  float ki = 0.035F;
  float kd = 7.0F;
};

inline bool validPidGains(const PidGains& g) {
  return std::isfinite(g.kp) && std::isfinite(g.ki) && std::isfinite(g.kd) &&
         g.kp >= 0.01F && g.kp <= 100.0F && g.ki >= 0.000001F &&
         g.ki <= 10.0F && g.kd >= 0.0F && g.kd <= 10000.0F;
}

class PidControl {
 public:
  void reset(float measured) {
    previous_ = measured;
    integral_output_ = 0.0F;
    slope_ = 0.0F;
  }

  float update(const PidGains& gains, float target, float measured, float dt, float cap) {
    if (!validPidGains(gains) || !std::isfinite(target) || !std::isfinite(measured) ||
        !std::isfinite(dt) || dt <= 0.0F || !std::isfinite(cap) || cap <= 0.0F) {
      return 0.0F;
    }
    // Derivative on measurement avoids a kick when the ramp/phase target changes.
    const float alpha = dt / (2.0F + dt);
    slope_ += alpha * ((measured - previous_) / dt - slope_);
    previous_ = measured;
    const float error = target - measured;
    const float pd = gains.kp * error - gains.kd * slope_;
    const float proposed = integral_output_ + gains.ki * error * dt;
    const float demand = pd + integral_output_;
    // Conditional integration: stop winding into saturation, allow unwinding.
    if ((demand >= 0.0F && demand <= cap) || (demand > cap && error < 0.0F) ||
        (demand < 0.0F && error > 0.0F)) {
      integral_output_ = std::max(0.0F, std::min(proposed, cap));
    }
    return std::max(0.0F, std::min(pd + integral_output_, cap));
  }

 private:
  float previous_ = 0.0F;
  float integral_output_ = 0.0F;
  float slope_ = 0.0F;
};
