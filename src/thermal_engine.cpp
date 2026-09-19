#include "thermal_engine.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr RecipePhase kLeadedPhases[] = {
    {PhaseKind::Ramp, 150.0F, 1.0F, 0},
    {PhaseKind::Hold, 150.0F, 0.0F, 60},
    {PhaseKind::Ramp, 205.0F, 1.2F, 0},
    {PhaseKind::Hold, 205.0F, 0.0F, 20},
    {PhaseKind::Cooldown, 60.0F, 0.0F, 0},
};

constexpr RecipePhase kSac305Phases[] = {
    {PhaseKind::Ramp, 160.0F, 1.2F, 0},
    {PhaseKind::Hold, 160.0F, 0.0F, 45},
    {PhaseKind::Ramp, 235.0F, 1.4F, 0},
    {PhaseKind::Hold, 235.0F, 0.0F, 20},
    {PhaseKind::Cooldown, 60.0F, 0.0F, 0},
};

constexpr RecipePhase kNylon6Phases[] = {
    {PhaseKind::Ramp, 180.0F, 1.0F, 0},
    {PhaseKind::Hold, 180.0F, 0.0F, 3600},
    {PhaseKind::Cooldown, 60.0F, 0.0F, 0},
};

constexpr RecipePhase kChamberHoldPhases[] = {
    {PhaseKind::Ramp, 120.0F, 1.0F, 0},
    {PhaseKind::Hold, 120.0F, 0.0F, 3600},
    {PhaseKind::Cooldown, 60.0F, 0.0F, 0},
};

constexpr RecipePhase kCommission100Phases[] = {
    {PhaseKind::Ramp, 100.0F, 0.5F, 0},
    {PhaseKind::Hold, 100.0F, 0.0F, 300},
    {PhaseKind::Cooldown, 60.0F, 0.0F, 0},
};
constexpr RecipePhase kCommission150Phases[] = {
    {PhaseKind::Ramp, 150.0F, 0.5F, 0},
    {PhaseKind::Hold, 150.0F, 0.0F, 300},
    {PhaseKind::Cooldown, 60.0F, 0.0F, 0},
};
constexpr RecipePhase kCommission200Phases[] = {
    {PhaseKind::Ramp, 200.0F, 0.5F, 0},
    {PhaseKind::Hold, 200.0F, 0.0F, 300},
    {PhaseKind::Cooldown, 60.0F, 0.0F, 0},
};

constexpr Recipe kRecipes[] = {
    {RecipeId::LeadedReflow, "SnPb reflow", RunMode::Reflow, kLeadedPhases,
     static_cast<uint8_t>(sizeof(kLeadedPhases) / sizeof(kLeadedPhases[0])), 183.0F, 220.0F, false, 1800, 100.0F},
    {RecipeId::Sac305Reflow, "SAC305 reflow", RunMode::Reflow, kSac305Phases,
     static_cast<uint8_t>(sizeof(kSac305Phases) / sizeof(kSac305Phases[0])), 217.0F, 245.0F, true, 1800, 100.0F},
    {RecipeId::Nylon6Anneal, "Nylon-6 anneal", RunMode::Anneal, kNylon6Phases,
     static_cast<uint8_t>(sizeof(kNylon6Phases) / sizeof(kNylon6Phases[0])), 0.0F, 200.0F, false, 7200, 100.0F},
    {RecipeId::ChamberHold, "Chamber hold", RunMode::Hold, kChamberHoldPhases,
     static_cast<uint8_t>(sizeof(kChamberHoldPhases) / sizeof(kChamberHoldPhases[0])), 0.0F, 140.0F, false, 7200, 100.0F},
    {RecipeId::Commission100, "Commission 100 C", RunMode::Commissioning, kCommission100Phases,
     3, 0.0F, 120.0F, false, 1200, 25.0F},
    {RecipeId::Commission150, "Commission 150 C", RunMode::Commissioning, kCommission150Phases,
     3, 0.0F, 170.0F, false, 1200, 25.0F},
    {RecipeId::Commission200, "Commission 200 C", RunMode::Commissioning, kCommission200Phases,
     3, 0.0F, 220.0F, false, 1200, 25.0F},
    {RecipeId::Autotune100, "Autotune 100 C", RunMode::Autotune, kCommission100Phases,
     3, 0.0F, 120.0F, false, 1200, 25.0F},
    {RecipeId::Check100, "Check control 100 C", RunMode::Validation, kCommission100Phases,
     3, 0.0F, 120.0F, false, 1200, 25.0F},
    {RecipeId::Check150, "Check control 150 C", RunMode::Validation, kCommission150Phases,
     3, 0.0F, 170.0F, false, 1200, 25.0F},
    {RecipeId::Check200, "Check control 200 C", RunMode::Validation, kCommission200Phases,
     3, 0.0F, 220.0F, false, 1200, 25.0F},
};

constexpr Recipe kInvalidRecipe = {
    RecipeId::Invalid, "Invalid recipe", RunMode::Hold, nullptr, 0,
    0.0F, 0.0F, false, 0, 0.0F};

constexpr float kTargetToleranceC = 4.0F;
constexpr uint32_t kBurstWindowMs = 5000;
constexpr uint32_t kMinimumBurstOnMs = 250;
constexpr uint32_t kMaximumSensorAgeMs = 1000;
constexpr uint32_t kMaximumSensorLeadMs = 10;

template <typename T>
T clampValue(T value, T minimum, T maximum) {
  return std::max(minimum, std::min(value, maximum));
}

bool readingIsHealthy(const ThermocoupleReading& reading, uint32_t now_ms) {
  // Sensor reads can finish a millisecond after the loop timestamp was captured.
  // Both unsigned differences remain correct across the millis() wrap.
  const uint32_t age_ms = now_ms - reading.sample_ms;
  const uint32_t lead_ms = reading.sample_ms - now_ms;
  return reading.valid() &&
         (age_ms <= kMaximumSensorAgeMs || lead_ms <= kMaximumSensorLeadMs);
}

uint8_t approvalMaskForRecipe(RecipeId id) {
  switch (id) {
    case RecipeId::Check100: return kControlCheck100Mask;
    case RecipeId::Check150:
    case RecipeId::Commission150: return kControlCheck150Mask;
    case RecipeId::Check200:
    case RecipeId::Commission200: return kControlCheck200Mask;
    default: return 0;
  }
}

}  // namespace

const Recipe& recipeFor(RecipeId id) {
  for (const auto& recipe : kRecipes) {
    if (recipe.id == id) {
      return recipe;
    }
  }
  return kInvalidRecipe;
}

const char* toString(EngineState state) {
  switch (state) {
    case EngineState::Idle: return "IDLE";
    case EngineState::Running: return "RUNNING";
    case EngineState::Cooling: return "COOLING";
    case EngineState::Complete: return "COMPLETE";
    case EngineState::Aborted: return "ABORTED";
    case EngineState::Fault: return "FAULT";
  }
  return "UNKNOWN";
}

const char* toString(FaultCode fault) {
  switch (fault) {
    case FaultCode::TuneUnstable: return "TUNE UNSTABLE";
    case FaultCode::NoTuneCandidate: return "NO TUNE CANDIDATE";
    case FaultCode::InvalidRecipe: return "INVALID RECIPE";
    case FaultCode::CheckNotApproved: return "CHECK NOT APPROVED";
    case FaultCode::InvalidAnneal: return "INVALID ANNEAL";
    case FaultCode::None: return "NONE";
    case FaultCode::ProcessProbe: return "PROCESS PROBE";
    case FaultCode::ProcessOverTemperature: return "PROCESS OVERTEMP";
    case FaultCode::HighTemperatureProfileNotCommissioned: return "SAC305 NOT COMMISSIONED";
    case FaultCode::RunTimeout: return "RUN TIMEOUT";
    case FaultCode::TestStartTooHot: return "TEST START TOO HOT";
  }
  return "UNKNOWN";
}

const char* toString(PhaseKind phase) {
  switch (phase) {
    case PhaseKind::Ramp: return "RAMP";
    case PhaseKind::Hold: return "HOLD";
    case PhaseKind::Cooldown: return "COOL";
    case PhaseKind::ControlledCool: return "CONTROLLED COOL";
  }
  return "UNKNOWN";
}

bool ThermalEngine::start(RecipeId recipe_id, uint32_t now_ms, const ThermocoupleReading& process) {
  // Start must never reset an active deadline or clear a latched fault.
  if (snapshot_.state != EngineState::Idle) {
    return false;
  }
  if (recipe_id == RecipeId::CustomAnneal) {
    fault(FaultCode::InvalidAnneal);
    return false;
  }
  const Recipe& recipe = recipeFor(recipe_id);
  if (recipe.id == RecipeId::Invalid) {
    fault(FaultCode::InvalidRecipe);
    return false;
  }
  const uint8_t approval_mask = approvalMaskForRecipe(recipe_id);
  if (approval_mask != 0U && (study_.approved_checks_mask & approval_mask) == 0U) {
    fault(FaultCode::CheckNotApproved);
    return false;
  }
  if (!readingIsHealthy(process, now_ms)) {
    fault(FaultCode::ProcessProbe);
    return false;
  }
  if (recipe.requires_high_temperature_commissioning) {
    fault(FaultCode::HighTemperatureProfileNotCommissioned);
    return false;
  }
  if (process.celsius > recipe.maximum_process_celsius) {
    fault(FaultCode::ProcessOverTemperature);
    return false;
  }
  if ((recipe.mode == RunMode::Commissioning || isStudyRecipe(recipe_id)) &&
      process.celsius > 60.0F) {
    fault(FaultCode::TestStartTooHot);
    return false;
  }

  if (recipe.mode == RunMode::Validation && !study_.candidate_ready) {
    fault(FaultCode::NoTuneCandidate);
    return false;
  }
  if (recipe.mode == RunMode::Autotune) {
    ++study_.candidate_revision;
    study_.candidate_ready = false;
    study_.saved = study_.save_failed = false;
    study_.cycles = 0;
    study_.period_seconds = study_.amplitude_celsius = 0.0F;
    for (auto& check : study_.checks) check = {};
    autotune_.begin(now_ms, process.celsius);
    study_.tune_report = {};
    study_.tune_report.available = true;
    study_.tune_report.outcome = TuneOutcome::Running;
    study_.tune_report.sequence = ++tune_report_sequence_;
    last_tune_diagnostic_sequence_ = 0;
  }
  const int check_index = controlCheckIndex(recipe_id);
  if (check_index >= 0) {
    const uint32_t attempts = study_.checks[check_index].attempts + 1;
    study_.checks[check_index] = {};
    study_.checks[check_index].attempts = attempts;
    study_.checks[check_index].result = StudyResult::Running;
    study_.checks[check_index].start_celsius = process.celsius;
    study_.checks[check_index].peak_celsius = process.celsius;
    study_.checks[check_index].candidate_revision = study_.candidate_revision;
    study_.checks[check_index].setup_revision = study_.setup_revision;
  }
  hold_error_sum_ = hold_output_sum_ = 0.0;
  hold_seconds_ = 0.0F;
  run_gains_ = recipe.mode == RunMode::Validation ? study_.candidate : study_.active;
  snapshot_ = {};
  snapshot_.state = EngineState::Running;
  snapshot_.recipe = &recipe;
  snapshot_.process_celsius = process.celsius;
  snapshot_.target_celsius = process.celsius;
  snapshot_.probe_healthy = true;
  run_started_ms_ = now_ms;
  phase_started_ms_ = now_ms;
  last_control_ms_ = now_ms;
  last_update_ms_ = now_ms;
  phase_in_band_ms_ = 0;
  liquidus_elapsed_ms_ = 0;
  pid_.reset(process.celsius);
  return true;
}

bool ThermalEngine::startAnneal(const AnnealProgram& program, uint32_t now_ms,
                                const ThermocoupleReading& process) {
  // Never let a second Start mutate an active run's frozen recipe.
  if (snapshot_.state != EngineState::Idle) {
    return false;
  }
  // Sensor and hard process limits outrank configurable program validation.
  if (!readingIsHealthy(process, now_ms)) {
    fault(FaultCode::ProcessProbe);
    return false;
  }
  if (process.celsius > kAnnealMaximumProcessCelsius) {
    fault(FaultCode::ProcessOverTemperature);
    return false;
  }
  if (!validAnnealProgram(program) || process.celsius > program.target_celsius ||
      !annealFitsBudget(program, process.celsius)) {
    fault(FaultCode::InvalidAnneal);
    return false;
  }

  anneal_program_ = program;
  const float rate_celsius_per_second =
      program.ramp_celsius_per_minute / 60.0F;
  custom_phases_[0] = {PhaseKind::Ramp, program.target_celsius,
                       rate_celsius_per_second, 0U};
  custom_phases_[1] = {PhaseKind::Hold, program.target_celsius, 0.0F,
                       program.soak_seconds};
  custom_phases_[2] = {PhaseKind::ControlledCool, kAnnealCooldownTargetCelsius,
                       rate_celsius_per_second, 0U};
  custom_phases_[3] = {PhaseKind::Cooldown, kAnnealCooldownTargetCelsius, 0.0F, 0U};

  hold_error_sum_ = hold_output_sum_ = 0.0;
  hold_seconds_ = 0.0F;
  run_gains_ = study_.active;
  snapshot_ = {};
  snapshot_.state = EngineState::Running;
  snapshot_.recipe = &custom_recipe_;
  snapshot_.process_celsius = process.celsius;
  snapshot_.target_celsius = process.celsius;
  snapshot_.probe_healthy = true;
  run_started_ms_ = now_ms;
  phase_started_ms_ = now_ms;
  last_control_ms_ = now_ms;
  last_update_ms_ = now_ms;
  phase_in_band_ms_ = 0;
  liquidus_elapsed_ms_ = 0;
  pid_.reset(process.celsius);
  return true;
}

void ThermalEngine::update(uint32_t now_ms, const ThermocoupleReading& process) {
  snapshot_.process_celsius = process.celsius;
  snapshot_.probe_healthy =
      readingIsHealthy(process, now_ms);

  if (snapshot_.state != EngineState::Running && snapshot_.state != EngineState::Cooling) {
    snapshot_.heater_commanded_on = false;
    snapshot_.output_percent = 0.0F;
    return;
  }
  if (!checkSafety(now_ms, process)) {
    return;
  }

  snapshot_.run_elapsed_seconds = (now_ms - run_started_ms_) / 1000U;
  snapshot_.phase_elapsed_seconds = (now_ms - phase_started_ms_) / 1000U;
  if (now_ms - run_started_ms_ >= snapshot_.recipe->maximum_run_seconds * 1000UL) {
    if (snapshot_.recipe->mode == RunMode::Autotune) finishTuneReport(TuneOutcome::Timeout);
    fault(FaultCode::RunTimeout);
    return;
  }
  updateStudyMetrics(now_ms);
  if (snapshot_.recipe->mode == RunMode::Autotune && snapshot_.state == EngineState::Running) {
    snapshot_.target_celsius = 100.0F;
    autotune_.update(now_ms, process.celsius, snapshot_.heater_commanded_on);
    study_.cycles = autotune_.cycles();
    if (autotune_.diagnosticSequence() != last_tune_diagnostic_sequence_) {
      last_tune_diagnostic_sequence_ = autotune_.diagnosticSequence();
      study_.tune_report.available = true;
      study_.tune_report.terminal = false;
      study_.tune_report.outcome = TuneOutcome::Running;
      study_.tune_report.cycles = autotune_.cycles();
      study_.tune_report.elapsed_seconds = snapshot_.run_elapsed_seconds;
      study_.tune_report.latest = autotune_.diagnostics();
      study_.tune_report.sequence = ++tune_report_sequence_;
    }
    if (autotune_.failed()) { finishTuneReport(TuneOutcome::Unstable); fault(FaultCode::TuneUnstable); return; }
    if (autotune_.ready()) {
      study_.candidate = autotune_.gains();
      study_.period_seconds = autotune_.periodSeconds();
      study_.amplitude_celsius = autotune_.amplitudeCelsius();
      snapshot_.phase_index = 2;
      snapshot_.state = EngineState::Cooling;
      snapshot_.target_celsius = 0.0F;
      snapshot_.output_percent = 0.0F;
      snapshot_.heater_commanded_on = false;
    } else {
      snapshot_.output_percent = autotune_.demand();
      updateBurst(now_ms);
    }
    last_update_ms_ = now_ms;
    return;
  }
  const RecipePhase& phase = snapshot_.recipe->phases[snapshot_.phase_index];

  if (snapshot_.recipe->liquidus_celsius > 0.0F && process.celsius >= snapshot_.recipe->liquidus_celsius) {
    liquidus_elapsed_ms_ += now_ms - last_update_ms_;
  }
  snapshot_.liquidus_elapsed_seconds = liquidus_elapsed_ms_ / 1000U;

  if (phase.kind == PhaseKind::Ramp) {
    const float elapsed_seconds = static_cast<float>(now_ms - last_update_ms_) / 1000.0F;
    snapshot_.target_celsius = std::min(phase.target_celsius,
                                        snapshot_.target_celsius + phase.rate_celsius_per_second * elapsed_seconds);
    if (snapshot_.target_celsius >= phase.target_celsius &&
        process.celsius >= phase.target_celsius - kTargetToleranceC) {
      advancePhase(now_ms);
    }
  } else if (phase.kind == PhaseKind::Hold) {
    snapshot_.target_celsius = phase.target_celsius;
    if (std::fabs(process.celsius - phase.target_celsius) <= kTargetToleranceC) {
      if (phase_in_band_ms_ == 0U) {
        phase_in_band_ms_ = now_ms;
      }
      if ((now_ms - phase_in_band_ms_) >= phase.duration_seconds * 1000UL) {
        advancePhase(now_ms);
      }
    } else {
      phase_in_band_ms_ = 0;
    }
  } else if (phase.kind == PhaseKind::ControlledCool) {
    const float elapsed_seconds =
        static_cast<float>(now_ms - last_update_ms_) / 1000.0F;
    snapshot_.target_celsius =
        std::max(phase.target_celsius,
                 snapshot_.target_celsius -
                     phase.rate_celsius_per_second * elapsed_seconds);
    if (snapshot_.target_celsius <= phase.target_celsius &&
        process.celsius <= phase.target_celsius + kTargetToleranceC) {
      advancePhase(now_ms);
    }
  } else {
    snapshot_.state = EngineState::Cooling;
    snapshot_.target_celsius = 0.0F;
    snapshot_.output_percent = 0.0F;
    snapshot_.heater_commanded_on = false;
    if (process.celsius <= phase.target_celsius) {
      snapshot_.state = EngineState::Complete;
      if (snapshot_.recipe->mode == RunMode::Autotune) {
        study_.candidate_ready = autotune_.ready();
        finishTuneReport(TuneOutcome::Complete);
      }
      finishStudy(StudyResult::Complete);
    }
  }

  if (snapshot_.state == EngineState::Running) {
    updateControl(now_ms);
  }
  last_update_ms_ = now_ms;
}

void ThermalEngine::abort() {
  if (snapshot_.state == EngineState::Running || snapshot_.state == EngineState::Cooling) {
    if (snapshot_.recipe && snapshot_.recipe->mode == RunMode::Autotune)
      finishTuneReport(TuneOutcome::Aborted);
    finishStudy(StudyResult::Aborted);
    snapshot_.state = EngineState::Aborted;
  }
  snapshot_.heater_commanded_on = false;
  snapshot_.output_percent = 0.0F;
}

void ThermalEngine::acknowledge() {
  if (snapshot_.state == EngineState::Fault || snapshot_.state == EngineState::Complete ||
      snapshot_.state == EngineState::Aborted) {
    snapshot_ = {};
  }
}

void ThermalEngine::fault(FaultCode code) {
  if (snapshot_.recipe && snapshot_.recipe->mode == RunMode::Autotune &&
      !study_.tune_report.terminal)
    finishTuneReport(code == FaultCode::RunTimeout ? TuneOutcome::Timeout :
                     code == FaultCode::TuneUnstable ? TuneOutcome::Unstable : TuneOutcome::OtherFault);
  finishStudy(StudyResult::Failed);
  if (snapshot_.recipe && snapshot_.recipe->mode == RunMode::Autotune) study_.candidate_ready = false;
  snapshot_.state = EngineState::Fault;
  snapshot_.fault = code;
  snapshot_.heater_commanded_on = false;
  snapshot_.output_percent = 0.0F;
}

void ThermalEngine::advancePhase(uint32_t now_ms) {
  ++snapshot_.phase_index;
  phase_started_ms_ = now_ms;
  phase_in_band_ms_ = 0;
  // Preserve integral heating bias across ramp/hold transitions.
  if (snapshot_.phase_index >= snapshot_.recipe->phase_count) {
    snapshot_.state = EngineState::Complete;
    snapshot_.heater_commanded_on = false;
    snapshot_.output_percent = 0.0F;
  } else if (snapshot_.recipe->phases[snapshot_.phase_index].kind ==
             PhaseKind::ControlledCool) {
    pid_.reset(snapshot_.process_celsius);
    last_control_ms_ = now_ms;
  } else if (snapshot_.recipe->phases[snapshot_.phase_index].kind == PhaseKind::Cooldown) {
    snapshot_.state = EngineState::Cooling;
    snapshot_.target_celsius = 0.0F;
    snapshot_.heater_commanded_on = false;
    snapshot_.output_percent = 0.0F;
  }
}

void ThermalEngine::updateControl(uint32_t now_ms) {
  const RecipePhase& phase = snapshot_.recipe->phases[snapshot_.phase_index];
  const float dt = std::max(0.02F, static_cast<float>(now_ms - last_control_ms_) / 1000.0F);
  snapshot_.output_percent = pid_.update(run_gains_, snapshot_.target_celsius,
                                         snapshot_.process_celsius, dt,
                                         snapshot_.recipe->maximum_output_percent);
  last_control_ms_ = now_ms;
  if (phase.kind == PhaseKind::ControlledCool &&
      snapshot_.process_celsius >= snapshot_.target_celsius) {
    snapshot_.output_percent = 0.0F;
    snapshot_.heater_commanded_on = false;
  } else {
    updateBurst(now_ms);
  }
}

void ThermalEngine::updateBurst(uint32_t now_ms) {
  const uint32_t burst_position_ms = (now_ms - run_started_ms_) % kBurstWindowMs;
  const uint32_t requested_on_ms = static_cast<uint32_t>(snapshot_.output_percent * 50.0F);
  const uint32_t on_ms = requested_on_ms >= kMinimumBurstOnMs ? requested_on_ms : 0U;
  snapshot_.heater_commanded_on = on_ms > 0U && burst_position_ms < on_ms;
}

bool ThermalEngine::checkSafety(uint32_t now_ms, const ThermocoupleReading& process) {
  if (!readingIsHealthy(process, now_ms)) {
    fault(FaultCode::ProcessProbe);
    return false;
  }
  if (process.celsius > snapshot_.recipe->maximum_process_celsius) {
    fault(FaultCode::ProcessOverTemperature);
    return false;
  }
  return true;
}

bool isStudyRecipe(RecipeId id) {
  return id == RecipeId::Autotune100 || controlCheckIndex(id) >= 0;
}

int controlCheckIndex(RecipeId id) {
  switch (id) {
    case RecipeId::Check100: return 0;
    case RecipeId::Check150: return 1;
    case RecipeId::Check200: return 2;
    default: return -1;
  }
}

float controlCheckTargetCelsius(uint8_t index) {
  constexpr float kTargets[kControlCheckCount] = {100.0F, 150.0F, 200.0F};
  return index < kControlCheckCount ? kTargets[index] : 0.0F;
}

bool ThermalEngine::canSaveStudy() const {
  if (snapshot_.state != EngineState::Idle || !study_.candidate_ready ||
      !validPidGains(study_.candidate)) return false;
  for (uint8_t i = 0; i < kControlCheckCount; ++i) {
    const uint8_t mask = static_cast<uint8_t>(1U << i);
    if ((study_.required_checks_mask & mask) == 0U) continue;
    const auto& check = study_.checks[i];
    if (check.result != StudyResult::Complete ||
        check.candidate_revision != study_.candidate_revision ||
        check.setup_revision != study_.setup_revision) return false;
  }
  return true;
}

uint8_t ThermalEngine::candidateCheckedScope() const {
  uint8_t checked_scope_mask = 0U;
  for (uint8_t i = 0; i < kControlCheckCount; ++i) {
    const auto& check = study_.checks[i];
    if (check.result == StudyResult::Complete &&
        check.candidate_revision == study_.candidate_revision &&
        check.setup_revision == study_.setup_revision) {
      checked_scope_mask |= static_cast<uint8_t>(1U << i);
    }
  }
  return checked_scope_mask;
}

bool ThermalEngine::configureControlChecks(uint8_t required_checks_mask,
                                           uint8_t approved_checks_mask,
                                           uint32_t setup_revision) {
  if (snapshot_.state != EngineState::Idle || required_checks_mask == 0U ||
      approved_checks_mask == 0U || setup_revision == 0U ||
      (required_checks_mask & ~kAllControlChecksMask) != 0U ||
      (approved_checks_mask & ~kAllControlChecksMask) != 0U ||
      (required_checks_mask & approved_checks_mask) != required_checks_mask) return false;
  if (study_.required_checks_mask == required_checks_mask &&
      study_.approved_checks_mask == approved_checks_mask &&
      study_.setup_revision == setup_revision) return true;

  const bool setup_changed = study_.setup_revision != setup_revision;
  study_.required_checks_mask = required_checks_mask;
  study_.approved_checks_mask = approved_checks_mask;
  study_.setup_revision = setup_revision;
  for (auto& check : study_.checks) check = {};
  study_.saved = false;
  study_.save_failed = false;
  if (setup_changed) study_.candidate_ready = false;
  return true;
}

bool ThermalEngine::loadPid(const PidGains& gains) {
  return loadPid(gains, 0U, 0U);
}

bool ThermalEngine::loadPid(const PidGains& gains, uint8_t checked_scope_mask,
                            uint32_t setup_revision) {
  if (snapshot_.state != EngineState::Idle || !validPidGains(gains)) return false;
  if ((checked_scope_mask & ~kAllControlChecksMask) != 0U ||
      (checked_scope_mask != 0U && setup_revision == 0U)) return false;
  study_.active = gains;
  study_.checked_scope_mask = checked_scope_mask;
  study_.checked_setup_revision = checked_scope_mask == 0U ? 0U : setup_revision;
  return true;
}

bool ThermalEngine::acceptStudy() {
  if (!canSaveStudy()) return false;
  study_.active = study_.candidate;
  study_.checked_scope_mask = candidateCheckedScope();
  study_.checked_setup_revision = study_.setup_revision;
  study_.saved = true;
  study_.save_failed = false;
  return true;
}

bool ThermalEngine::loadTuneReport(const TuneRunReport& report) {
  if (snapshot_.state != EngineState::Idle || !report.available || !report.terminal) return false;
  study_.tune_report = report;
  tune_report_sequence_ = report.sequence;
  return true;
}

void ThermalEngine::finishTuneReport(TuneOutcome outcome) {
  study_.tune_report.available = true;
  study_.tune_report.terminal = true;
  study_.tune_report.outcome = outcome;
  study_.tune_report.cycles = autotune_.cycles();
  study_.tune_report.elapsed_seconds = snapshot_.run_elapsed_seconds;
  study_.tune_report.sequence = ++tune_report_sequence_;
}

void ThermalEngine::updateStudyMetrics(uint32_t now_ms) {
  const int index = controlCheckIndex(snapshot_.recipe->id);
  if (index < 0) return;
  auto& record = study_.checks[index];
  const float target_celsius = controlCheckTargetCelsius(static_cast<uint8_t>(index));
  record.peak_celsius = std::max(record.peak_celsius, snapshot_.process_celsius);
  record.elapsed_seconds = (now_ms - run_started_ms_) / 1000U;
  if (!record.reached_band && snapshot_.process_celsius >= target_celsius - kTargetToleranceC) {
    record.reached_band = true;
    record.rise_seconds = record.elapsed_seconds;
  }
  if (snapshot_.state == EngineState::Running &&
      snapshot_.recipe->phases[snapshot_.phase_index].kind == PhaseKind::Hold) {
    const float dt = static_cast<float>(now_ms - last_update_ms_) / 1000.0F;
    const float error = snapshot_.process_celsius - target_celsius;
    hold_seconds_ += dt;
    hold_error_sum_ += error * error * dt;
    hold_output_sum_ += snapshot_.output_percent * dt;
    if (hold_seconds_ > 0.0F) {
      record.hold_rmse = std::sqrt(hold_error_sum_ / hold_seconds_);
      record.hold_output_percent = hold_output_sum_ / hold_seconds_;
    }
  }
}

void ThermalEngine::finishStudy(StudyResult result) {
  if (!snapshot_.recipe) return;
  const int index = controlCheckIndex(snapshot_.recipe->id);
  if (index >= 0 && study_.checks[index].result == StudyResult::Running)
    study_.checks[index].result = result;
}
