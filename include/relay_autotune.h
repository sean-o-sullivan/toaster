#pragma once

#include <cstdint>
#include "pid_control.h"

// Bounded 100 C experiment. The engine owns all heater/sensor safety checks.
class RelayAutotune {
 public:
  void begin(uint32_t now_ms, float temperature);
  void update(uint32_t now_ms, float temperature);
  float demand() const { return ready_ || failed_ ? 0.0F : heating_ ? 25.0F : 0.0F; }
  bool ready() const { return ready_; }
  bool failed() const { return failed_; }
  uint8_t cycles() const { return cycles_; }
  const PidGains& gains() const { return gains_; }
  float periodSeconds() const { return period_seconds_; }
  float amplitudeCelsius() const { return amplitude_; }

 private:
  struct Cycle { float period; float amplitude; float fraction; float midpoint; };
  bool calculate();
  Cycle recent_[3] = {};
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
};
