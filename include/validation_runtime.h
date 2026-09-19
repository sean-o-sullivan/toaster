#pragma once

#include "profile_validation_state.h"
#include "thermal_engine.h"

// Assessment only: no method can command heat or change a recipe.
class ValidationRuntime {
 public:
  ValidationRuntime();
  void start(const ThermalEngine& engine, uint32_t now_ms);
  void update(const ThermalEngine& engine, uint32_t now_ms);
  void refreshConfiguration(const ThermalEngine& engine);
  bool commission(uint8_t profile, const ThermalEngine& engine);
  const ProfileValidationView* views() const { return views_; }
  uint32_t sequence() const { return sequence_; }
  bool terminalPending() const { return terminal_pending_; }
  void persisted() { terminal_pending_ = false; }
  uint8_t latestProfile() const { return latest_profile_; }
  void loadHistorical(uint8_t profile, const ValidationReport& report);

 private:
  RunIdentity identity(uint8_t profile, const ThermalEngine& engine) const;
  ProcessValidationAssessor assessor_;
  ValidationSequence repeats_[4];
  ProfileValidationView views_[4];
  uint32_t run_id_ = 0;
  uint32_t sequence_ = 0;
  uint8_t latest_profile_ = 0;
  bool terminal_pending_ = false;
  bool active_run_ = false;
  bool controller_checked_at_start_ = false;
  uint32_t recipe_revisions_[4] = {};
};
