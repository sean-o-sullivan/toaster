#pragma once

#include <cstddef>
#include <cstdint>

enum class ValidationProfile : uint8_t { Reflow, Anneal };
enum class ValidationStage : uint8_t { Ramp, Hold, Cooling };
enum class ValidationStatus : uint8_t {
  NotRun,
  Running,
  Pass,
  Fail,
  Incomplete,
  CriteriaMissing,
};
enum class DurationMode : uint8_t { Continuous, Accumulated };

struct RunIdentity {
  uint32_t setup_revision = 0;
  uint32_t gain_revision = 0;
  uint32_t recipe_revision = 0;
  uint32_t criteria_revision = 0;
  uint32_t run_id = 0;
};

struct CriterionRequirement {
  bool required = false;
  bool not_applicable = false;
  float minimum = 0.0F;
  float maximum = 0.0F;
};

struct SlopeRequirement {
  CriterionRequirement criterion;
  float window_start_celsius = 0.0F;
  float window_end_celsius = 0.0F;
};

struct BandDurationRequirement {
  CriterionRequirement criterion;
  float band_minimum_celsius = 0.0F;
  float band_maximum_celsius = 0.0F;
  DurationMode mode = DurationMode::Continuous;
};

struct ThresholdDurationRequirement {
  CriterionRequirement criterion;
  float threshold_celsius = 0.0F;
  DurationMode mode = DurationMode::Accumulated;
};

struct AnnealRequirement {
  bool required = false;
  bool not_applicable = false;
  float target_celsius = 0.0F;
  float band_celsius = 0.0F;
  float minimum_hold_seconds = 0.0F;
  float maximum_warmup_seconds = 0.0F;
  float maximum_settling_seconds = 0.0F;
  float settling_continuous_seconds = 0.0F;
  float maximum_overshoot_celsius = 0.0F;
  float maximum_mean_error_celsius = 0.0F;
  float maximum_rms_error_celsius = 0.0F;
  float maximum_error_celsius = 0.0F;
  float maximum_abs_drift_celsius_per_second = 0.0F;
};

// A zero-initialized Requirements object has no configured specification and
// can never pass. IDs/revisions are caller-owned stable provenance, not labels.
struct Requirements {
  static const size_t kIdSize = 24;
  static const size_t kRevisionSize = 16;
  static const size_t kSourceSize = 80;

  bool valid = false;
  ValidationProfile profile = ValidationProfile::Reflow;
  char criteria_id[kIdSize] = {};
  char criteria_revision[kRevisionSize] = {};
  char source_reference[kSourceSize] = {};

  SlopeRequirement heating_slope;
  BandDurationRequirement soak;
  CriterionRequirement peak_celsius;
  ThresholdDurationRequirement liquidus;
  SlopeRequirement cooling_slope;
  AnnealRequirement anneal;
};

struct CriterionResult {
  ValidationStatus status = ValidationStatus::NotRun;
  bool not_applicable = false;
  bool measured = false;
  float value = 0.0F;
};

struct ValidationMetrics {
  bool heating_slope_measured = false;
  float heating_slope_celsius_per_second = 0.0F;
  uint32_t soak_accumulated_ms = 0;
  uint32_t soak_longest_continuous_ms = 0;
  bool peak_measured = false;
  float peak_celsius = 0.0F;
  uint32_t liquidus_accumulated_ms = 0;
  uint32_t liquidus_longest_continuous_ms = 0;
  bool cooling_slope_measured = false;
  float cooling_slope_celsius_per_second = 0.0F;

  uint32_t hold_elapsed_ms = 0;
  uint32_t hold_in_band_accumulated_ms = 0;
  uint32_t hold_longest_in_band_ms = 0;
  bool warmup_measured = false;
  uint32_t warmup_ms = 0;
  bool settling_measured = false;
  uint32_t settling_ms = 0;
  bool hold_error_measured = false;
  float hold_mean_error_celsius = 0.0F;
  float hold_rms_error_celsius = 0.0F;
  float hold_max_error_celsius = 0.0F;
  float hold_drift_celsius_per_second = 0.0F;
  float hold_peak_celsius = 0.0F;
  float hold_max_target_mismatch_celsius = 0.0F;
  float hold_mean_demand_percent = 0.0F;
  float hold_max_demand_percent = 0.0F;

  uint32_t sample_count = 0;
  uint32_t maximum_gap_ms = 0;
};

struct ValidationReport {
  RunIdentity identity;
  ValidationProfile profile = ValidationProfile::Reflow;
  ValidationStatus status = ValidationStatus::NotRun;
  bool run_completed = false;
  bool evidence_complete = false;
  ValidationMetrics metrics;
  CriterionResult heating_slope;
  CriterionResult soak;
  CriterionResult peak;
  CriterionResult liquidus;
  CriterionResult cooling_slope;
  CriterionResult anneal;
};

class ProcessValidationAssessor {
 public:
  // begin() includes the first measurement. Invalid identity, requirements or
  // temperature is retained as a non-passing report rather than defaulting.
  bool begin(const Requirements& requirements, const RunIdentity& identity,
             uint32_t now_ms, float temperature_celsius);
  bool sample(uint32_t now_ms, float temperature_celsius, float target_celsius,
              ValidationStage stage, float demand_percent, bool valid);
  ValidationReport finish(bool completed);
  bool running() const { return running_; }
  const ValidationReport& report() const { return report_; }

 private:
  void reset();
  void invalidateEvidence();
  void accumulateBand(uint32_t delta_ms, float previous, float current,
                      float low, float high, uint32_t& accumulated,
                      uint32_t& uninterrupted, uint32_t& longest);
  void accumulateAbove(uint32_t delta_ms, float previous, float current,
                       float threshold, uint32_t& accumulated,
                       uint32_t& uninterrupted, uint32_t& longest);
  void assess();

  Requirements requirements_;
  ValidationReport report_;
  bool running_ = false;
  bool finalized_ = false;
  bool evidence_complete_ = false;
  uint32_t last_ms_ = 0;
  uint64_t elapsed_ms_ = 0;
  float last_temperature_ = 0.0F;
  float last_target_ = 0.0F;
  float last_demand_ = 0.0F;
  ValidationStage last_stage_ = ValidationStage::Ramp;

  bool heat_start_seen_ = false;
  bool heat_end_seen_ = false;
  double heat_start_ms_ = 0.0;
  double heat_end_ms_ = 0.0;
  bool cool_start_seen_ = false;
  bool cool_end_seen_ = false;
  double cool_start_ms_ = 0.0;
  double cool_end_ms_ = 0.0;

  uint32_t soak_current_ms_ = 0;
  uint32_t liquidus_current_ms_ = 0;
  uint32_t hold_current_in_band_ms_ = 0;
  bool hold_seen_ = false;
  bool warmup_seen_ = false;
  uint32_t settling_current_ms_ = 0;

  double hold_error_time_sum_ = 0.0;
  double hold_error_squared_time_sum_ = 0.0;
  double hold_demand_time_sum_ = 0.0;
  double hold_time_seconds_ = 0.0;
  double regression_n_ = 0.0;
  double regression_t_sum_ = 0.0;
  double regression_error_sum_ = 0.0;
  double regression_tt_sum_ = 0.0;
  double regression_te_sum_ = 0.0;
};

class ValidationSequence {
 public:
  static const uint8_t kRequiredPasses = 3;

  struct State {
    static const uint32_t kMagic = 0x50565331U;  // "PVS1"
    static const uint16_t kVersion = 1U;

    uint32_t magic = kMagic;
    uint16_t version = kVersion;
    RunIdentity identity;
    uint32_t pass_run_ids[kRequiredPasses] = {};
    uint8_t passes = 0;
    bool commissioned = false;
    uint32_t checksum = 0;
  };

  void reset();
  bool record(const ValidationReport& report);
  bool eligible(const RunIdentity& identity) const;
  bool commission(const RunIdentity& identity);
  State snapshot() const;
  bool restore(const State& state, const RunIdentity& expected_identity);
  static uint32_t stateChecksum(const State& state);
  uint8_t consecutivePasses() const { return passes_; }
  bool commissioned() const { return commissioned_; }

 private:
  RunIdentity identity_;
  bool identity_set_ = false;
  uint32_t last_run_id_ = 0;
  uint32_t pass_run_ids_[kRequiredPasses] = {};
  uint8_t passes_ = 0;
  bool commissioned_ = false;
};

bool validRequirements(const Requirements& requirements);
bool sameValidationConfiguration(const RunIdentity& a, const RunIdentity& b);
const char* toString(ValidationStatus status);
