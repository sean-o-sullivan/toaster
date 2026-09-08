#pragma once

#include <cstdint>

#include "thermocouple_types.h"
#include "relay_autotune.h"

enum class RecipeId : uint8_t {
  LeadedReflow,
  Sac305Reflow,
  Nylon6Anneal,
  ChamberHold,
  Commission100,
  Commission150,
  Commission200,
  Autotune100,
  ValidateA,
  ValidateB,
  ValidateC,
};

enum class RunMode : uint8_t {
  Reflow,
  Anneal,
  Hold,
  Commissioning,
  Autotune,
  Validation,
};

enum class PhaseKind : uint8_t {
  Ramp,
  Hold,
  Cooldown,
};

enum class EngineState : uint8_t {
  Idle,
  Running,
  Cooling,
  Complete,
  Aborted,
  Fault,
};

enum class FaultCode : uint8_t {
  None,
  ProcessProbe,
  ProcessOverTemperature,
  HighTemperatureProfileNotCommissioned,
  RunTimeout,
  TestStartTooHot,
  TuneUnstable,
  NoTuneCandidate,
};

struct RecipePhase {
  PhaseKind kind;
  float target_celsius;
  float rate_celsius_per_second;
  uint32_t duration_seconds;
};

struct Recipe {
  RecipeId id;
  const char* name;
  RunMode mode;
  const RecipePhase* phases;
  uint8_t phase_count;
  float liquidus_celsius;
  float maximum_process_celsius;
  bool requires_high_temperature_commissioning;
  uint32_t maximum_run_seconds;
  float maximum_output_percent;
};

struct EngineSnapshot {
  EngineState state = EngineState::Idle;
  FaultCode fault = FaultCode::None;
  const Recipe* recipe = nullptr;
  uint8_t phase_index = 0;
  float process_celsius = 0.0F;
  float target_celsius = 0.0F;
  float output_percent = 0.0F;
  bool heater_commanded_on = false;
  uint32_t run_elapsed_seconds = 0;
  uint32_t phase_elapsed_seconds = 0;
  uint32_t liquidus_elapsed_seconds = 0;
  bool probe_healthy = false;
};

enum class StudyResult : uint8_t { Empty, Running, Complete, Failed, Aborted };
struct StudyRecord {
  StudyResult result = StudyResult::Empty;
  uint32_t attempts = 0;
  float start_celsius = 0.0F;
  float peak_celsius = 0.0F;
  uint32_t rise_seconds = 0;
  bool reached_band = false;
  float hold_rmse = 0.0F;
  float hold_output_percent = 0.0F;
  uint32_t elapsed_seconds = 0;
};
struct PidStudy {
  PidGains active;
  PidGains candidate;
  bool candidate_ready = false;
  bool saved = false;
  bool save_failed = false;
  uint8_t cycles = 0;
  float period_seconds = 0.0F;
  float amplitude_celsius = 0.0F;
  StudyRecord points[3];
};

bool isStudyRecipe(RecipeId id);
int studyPoint(RecipeId id);

const Recipe& recipeFor(RecipeId id);
const char* toString(EngineState state);
const char* toString(FaultCode fault);
const char* toString(PhaseKind phase);

class ThermalEngine {
 public:
  bool start(RecipeId recipe_id, uint32_t now_ms, const ThermocoupleReading& process);
  void update(uint32_t now_ms, const ThermocoupleReading& process);
  void abort();
  void acknowledge();

  const EngineSnapshot& snapshot() const { return snapshot_; }
  bool heaterCommand() const { return snapshot_.heater_commanded_on; }
  const PidStudy& study() const { return study_; }
  const PidGains& runGains() const { return run_gains_; }
  bool canSaveStudy() const;
  bool loadPid(const PidGains& gains);
  bool acceptStudy();
  void saveFailed() { study_.save_failed = true; }

 private:
  void fault(FaultCode code);
  void advancePhase(uint32_t now_ms);
  void updateControl(uint32_t now_ms);
  bool checkSafety(uint32_t now_ms, const ThermocoupleReading& process);
  void updateBurst(uint32_t now_ms);
  void updateStudyMetrics(uint32_t now_ms);
  void finishStudy(StudyResult result);

  EngineSnapshot snapshot_;
  uint32_t run_started_ms_ = 0;
  uint32_t phase_started_ms_ = 0;
  uint32_t phase_in_band_ms_ = 0;
  uint32_t liquidus_elapsed_ms_ = 0;
  uint32_t last_control_ms_ = 0;
  uint32_t last_update_ms_ = 0;
  PidControl pid_;
  PidGains run_gains_;
  PidStudy study_;
  RelayAutotune autotune_;
  double hold_error_sum_ = 0.0;
  double hold_output_sum_ = 0.0;
  float hold_seconds_ = 0.0F;
};
