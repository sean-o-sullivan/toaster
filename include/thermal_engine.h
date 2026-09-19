#pragma once

#include <cstdint>

#include "anneal_program.h"
#include "thermocouple_types.h"
#include "relay_autotune.h"

enum class RecipeId : uint8_t {
  LeadedReflow = 0,
  Sac305Reflow = 1,
  Nylon6Anneal = 2,
  ChamberHold = 3,
  Commission100 = 4,
  Commission150 = 5,
  Commission200 = 6,
  Autotune100 = 7,
  ValidateA = 8,  // Retired; retained for historical trace decoding.
  ValidateB = 9,  // Retired; retained for historical trace decoding.
  ValidateC = 10, // Retired; retained for historical trace decoding.
  Check100 = 11,
  Check150 = 12,
  Check200 = 13,
  CustomAnneal = 14,
  Invalid = 255,
};

constexpr uint8_t kControlCheckCount = 3;
constexpr uint8_t kControlCheck100Mask = 1U << 0;
constexpr uint8_t kControlCheck150Mask = 1U << 1;
constexpr uint8_t kControlCheck200Mask = 1U << 2;
constexpr uint8_t kAllControlChecksMask =
    kControlCheck100Mask | kControlCheck150Mask | kControlCheck200Mask;

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
  ControlledCool = 3,
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
  InvalidRecipe,
  CheckNotApproved,
  InvalidAnneal,
};

enum class TuneOutcome : uint8_t { None, Running, Complete, Timeout, Unstable, Aborted, OtherFault };

struct TuneRunReport {
  bool available = false;
  bool terminal = false;
  TuneOutcome outcome = TuneOutcome::None;
  uint8_t cycles = 0;
  uint32_t elapsed_seconds = 0;
  uint32_t sequence = 0;
  TuneCycleDiagnostics latest;
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
  uint32_t candidate_revision = 0;
  uint32_t setup_revision = 0;
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
  TuneRunReport tune_report;
  StudyRecord checks[kControlCheckCount];
  uint8_t required_checks_mask = kControlCheck100Mask;
  uint8_t approved_checks_mask = kControlCheck100Mask;
  uint8_t checked_scope_mask = 0;
  uint32_t setup_revision = 1;
  uint32_t checked_setup_revision = 0;
  uint32_t candidate_revision = 0;
};

bool isStudyRecipe(RecipeId id);
int controlCheckIndex(RecipeId id);
float controlCheckTargetCelsius(uint8_t index);

const Recipe& recipeFor(RecipeId id);
const char* toString(EngineState state);
const char* toString(FaultCode fault);
const char* toString(PhaseKind phase);

class ThermalEngine {
 public:
  ThermalEngine() = default;
  ThermalEngine(const ThermalEngine&) = delete;
  ThermalEngine& operator=(const ThermalEngine&) = delete;

  bool start(RecipeId recipe_id, uint32_t now_ms, const ThermocoupleReading& process);
  bool startAnneal(const AnnealProgram& program, uint32_t now_ms,
                   const ThermocoupleReading& process);
  void update(uint32_t now_ms, const ThermocoupleReading& process);
  void abort();
  void acknowledge();

  const EngineSnapshot& snapshot() const { return snapshot_; }
  const Recipe& configuredAnnealRecipe() const { return custom_recipe_; }
  const AnnealProgram& configuredAnnealProgram() const { return anneal_program_; }
  bool heaterCommand() const { return snapshot_.heater_commanded_on; }
  const PidStudy& study() const { return study_; }
  const PidGains& runGains() const { return run_gains_; }
  bool canSaveStudy() const;
  uint8_t candidateCheckedScope() const;
  bool configureControlChecks(uint8_t required_checks_mask,
                              uint8_t approved_checks_mask,
                              uint32_t setup_revision);
  bool loadPid(const PidGains& gains);
  bool loadPid(const PidGains& gains, uint8_t checked_scope_mask,
               uint32_t setup_revision);
  bool acceptStudy();
  bool loadTuneReport(const TuneRunReport& report);
  void saveFailed() { study_.save_failed = true; }

 private:
  void fault(FaultCode code);
  void advancePhase(uint32_t now_ms);
  void updateControl(uint32_t now_ms);
  bool checkSafety(uint32_t now_ms, const ThermocoupleReading& process);
  void updateBurst(uint32_t now_ms);
  void updateStudyMetrics(uint32_t now_ms);
  void finishStudy(StudyResult result);
  void finishTuneReport(TuneOutcome outcome);

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
  uint32_t last_tune_diagnostic_sequence_ = 0;
  uint32_t tune_report_sequence_ = 0;
  AnnealProgram anneal_program_;
  RecipePhase custom_phases_[4] = {};
  Recipe custom_recipe_ = {
      RecipeId::CustomAnneal, "Custom anneal", RunMode::Anneal,
      custom_phases_, 4, 0.0F, kAnnealMaximumProcessCelsius, false,
      kAnnealMaximumRunSeconds, kAnnealMaximumOutputPercent};
};
