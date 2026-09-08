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
    {RecipeId::Autotune100, "Autotune A 100 C", RunMode::Autotune, kCommission100Phases,
     3, 0.0F, 120.0F, false, 1200, 25.0F},
    {RecipeId::ValidateA, "Validate A 100 C", RunMode::Validation, kCommission100Phases,
     3, 0.0F, 120.0F, false, 1200, 25.0F},
    {RecipeId::ValidateB, "Validate B 100 C", RunMode::Validation, kCommission100Phases,
     3, 0.0F, 120.0F, false, 1200, 25.0F},
    {RecipeId::ValidateC, "Validate C 100 C", RunMode::Validation, kCommission100Phases,
     3, 0.0F, 120.0F, false, 1200, 25.0F},
};

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

}  // namespace

const Recipe& recipeFor(RecipeId id) {
  for (const auto& recipe : kRecipes) {
    if (recipe.id == id) {
      return recipe;
    }
  }
  return kRecipes[0];
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
  }
  return "UNKNOWN";
}

bool ThermalEngine::start(RecipeId recipe_id, uint32_t now_ms, const ThermocoupleReading& process) {
  // Start must never reset an active deadline or clear a latched fault.
  if (snapshot_.state != EngineState::Idle) {
    return false;
  }
  const Recipe& recipe = recipeFor(recipe_id);
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
    study_.candidate_ready = false;
    study_.saved = study_.save_failed = false;
    study_.cycles = 0;
    study_.period_seconds = study_.amplitude_celsius = 0.0F;
    for (auto& point : study_.points) point = {};
    autotune_.begin(now_ms, process.celsius);
  }
  const int point = studyPoint(recipe_id);
  if (point >= 0) {
    const uint32_t attempts = study_.points[point].attempts + 1;
    study_.points[point] = {};
    study_.points[point].attempts = attempts;
    study_.points[point].result = StudyResult::Running;
    study_.points[point].start_celsius = process.celsius;
    study_.points[point].peak_celsius = process.celsius;
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
    fault(FaultCode::RunTimeout);
    return;
  }
  updateStudyMetrics(now_ms);
  if (snapshot_.recipe->mode == RunMode::Autotune && snapshot_.state == EngineState::Running) {
    snapshot_.target_celsius = 100.0F;
    autotune_.update(now_ms, process.celsius);
    study_.cycles = autotune_.cycles();
    if (autotune_.failed()) { fault(FaultCode::TuneUnstable); return; }
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
  } else {
    snapshot_.state = EngineState::Cooling;
    snapshot_.target_celsius = 0.0F;
    snapshot_.output_percent = 0.0F;
    snapshot_.heater_commanded_on = false;
    if (process.celsius <= phase.target_celsius) {
      snapshot_.state = EngineState::Complete;
      if (snapshot_.recipe->mode == RunMode::Autotune) study_.candidate_ready = autotune_.ready();
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
  } else if (snapshot_.recipe->phases[snapshot_.phase_index].kind == PhaseKind::Cooldown) {
    snapshot_.state = EngineState::Cooling;
    snapshot_.target_celsius = 0.0F;
    snapshot_.heater_commanded_on = false;
    snapshot_.output_percent = 0.0F;
  }
}

void ThermalEngine::updateControl(uint32_t now_ms) {
  const float dt = std::max(0.02F, static_cast<float>(now_ms - last_control_ms_) / 1000.0F);
  snapshot_.output_percent = pid_.update(run_gains_, snapshot_.target_celsius,
                                         snapshot_.process_celsius, dt,
                                         snapshot_.recipe->maximum_output_percent);
  updateBurst(now_ms);
  last_control_ms_ = now_ms;
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
  return id == RecipeId::Autotune100 || studyPoint(id) >= 0;
}

int studyPoint(RecipeId id) {
  switch (id) {
    case RecipeId::ValidateA: return 0;
    case RecipeId::ValidateB: return 1;
    case RecipeId::ValidateC: return 2;
    default: return -1;
  }
}

bool ThermalEngine::canSaveStudy() const {
  if (snapshot_.state != EngineState::Idle || !study_.candidate_ready ||
      !validPidGains(study_.candidate)) return false;
  for (const auto& point : study_.points)
    if (point.result != StudyResult::Complete) return false;
  return true;
}

bool ThermalEngine::loadPid(const PidGains& gains) {
  if (snapshot_.state != EngineState::Idle || !validPidGains(gains)) return false;
  study_.active = gains;
  return true;
}

bool ThermalEngine::acceptStudy() {
  if (!canSaveStudy()) return false;
  study_.active = study_.candidate;
  study_.saved = true;
  study_.save_failed = false;
  return true;
}

void ThermalEngine::updateStudyMetrics(uint32_t now_ms) {
  const int index = studyPoint(snapshot_.recipe->id);
  if (index < 0) return;
  auto& record = study_.points[index];
  record.peak_celsius = std::max(record.peak_celsius, snapshot_.process_celsius);
  record.elapsed_seconds = (now_ms - run_started_ms_) / 1000U;
  if (!record.reached_band && snapshot_.process_celsius >= 96.0F) {
    record.reached_band = true;
    record.rise_seconds = record.elapsed_seconds;
  }
  if (snapshot_.state == EngineState::Running &&
      snapshot_.recipe->phases[snapshot_.phase_index].kind == PhaseKind::Hold) {
    const float dt = static_cast<float>(now_ms - last_update_ms_) / 1000.0F;
    const float error = snapshot_.process_celsius - 100.0F;
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
  const int index = studyPoint(snapshot_.recipe->id);
  if (index >= 0 && study_.points[index].result == StudyResult::Running)
    study_.points[index].result = result;
}
