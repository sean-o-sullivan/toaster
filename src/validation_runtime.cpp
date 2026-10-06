#include "validation_runtime.h"
#include <algorithm>

#include "profile_requirements.h"
#include "pid_settings.h"

namespace {
uint32_t gainsIdentity(const PidStudy& study) {
  SavedPidV2 data{};
  data.gains = study.active;
  data.checked_scope_mask = study.checked_scope_mask;
  data.setup_revision = study.checked_setup_revision;
  data.reserved[0] = study.required_checks_mask;
  return pidChecksum(data);
}
uint32_t recipeIdentity(const Recipe& recipe) {
  // Hash controlled fields explicitly; never hash pointers or struct padding.
  uint32_t hash = 2166136261U;
  const auto add = [&hash](const void* data, size_t size) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) hash = (hash ^ bytes[i]) * 16777619U;
  };
  add(&recipe.id, sizeof(recipe.id));
  add(&recipe.liquidus_celsius, sizeof(recipe.liquidus_celsius));
  add(&recipe.maximum_output_percent, sizeof(recipe.maximum_output_percent));
  add(&recipe.maximum_process_celsius, sizeof(recipe.maximum_process_celsius));
  add(&recipe.maximum_run_seconds, sizeof(recipe.maximum_run_seconds));
  for (uint8_t i = 0; i < recipe.phase_count; ++i) {
    const auto& phase = recipe.phases[i];
    add(&phase.kind, sizeof(phase.kind));
    add(&phase.target_celsius, sizeof(phase.target_celsius));
    add(&phase.rate_celsius_per_second, sizeof(phase.rate_celsius_per_second));
    add(&phase.duration_seconds, sizeof(phase.duration_seconds));
  }
  return hash;
}
}

ValidationRuntime::ValidationRuntime() {
  for (uint8_t i = 0; i < 4; ++i) {
    recipe_revisions_[i] = recipeIdentity(recipeFor(static_cast<RecipeId>(i)));
    views_[i].report.profile = i < 2 ? ValidationProfile::Reflow : ValidationProfile::Anneal;
    views_[i].report.status = validRequirements(requirementsForProfile(i))
        ? ValidationStatus::NotRun : ValidationStatus::CriteriaMissing;
  }
}

RunIdentity ValidationRuntime::identity(uint8_t profile, const ThermalEngine& engine) const {
  RunIdentity result;
  result.setup_revision = engine.study().setup_revision;
  result.gain_revision = gainsIdentity(engine.study());
  result.recipe_revision = recipe_revisions_[profile];
  result.criteria_revision = profileCriteriaRevision(profile);
  result.run_id = run_id_;
  return result;
}

void ValidationRuntime::refreshConfiguration(const ThermalEngine& engine) {
  for (uint8_t i = 0; i < 4; ++i) {
    auto& view = views_[i];
    if (view.report.identity.run_id &&
        !sameValidationConfiguration(view.report.identity, identity(i, engine))) {
      repeats_[i].reset();
      view.consecutive_passes = 0;
      view.eligible = view.commissioned = false;
      view.needs_revalidation = true;
    }
  }
}

void ValidationRuntime::start(const ThermalEngine& engine, uint32_t now_ms) {
  const auto& s = engine.snapshot();
  if (!s.recipe || s.state != EngineState::Running ||
      (static_cast<uint8_t>(s.recipe->id) > 3 && s.recipe->id != RecipeId::CustomAnneal))
    return;
  const uint8_t profile = s.recipe->id == RecipeId::CustomAnneal
      ? 2U : static_cast<uint8_t>(s.recipe->id);
  recipe_revisions_[profile] = recipeIdentity(*s.recipe);
  refreshConfiguration(engine);
  latest_profile_ = profile;
  ++run_id_;
  if (!run_id_) ++run_id_;
  assessor_.begin(requirementsForProfile(latest_profile_), identity(latest_profile_, engine),
                  now_ms, s.process_celsius);
  active_run_ = true;
  controller_checked_at_start_ =
      (engine.study().checked_scope_mask & engine.study().required_checks_mask) ==
          engine.study().required_checks_mask &&
      engine.study().checked_setup_revision == engine.study().setup_revision;
  views_[latest_profile_].report = assessor_.report();
  views_[latest_profile_].eligible = views_[latest_profile_].commissioned = false;
  views_[latest_profile_].needs_revalidation = false;
  terminal_pending_ = false;
  ++sequence_;
}

void ValidationRuntime::update(const ThermalEngine& engine, uint32_t now_ms) {
  if (!active_run_) return;
  const auto& s = engine.snapshot();
  ValidationStage stage = ValidationStage::Ramp;
  if (s.recipe && s.phase_index < s.recipe->phase_count) {
    const auto kind = s.recipe->phases[s.phase_index].kind;
    if (kind == PhaseKind::Hold) stage = ValidationStage::Hold;
    if (kind == PhaseKind::Cooldown || kind == PhaseKind::ControlledCool)
      stage = ValidationStage::Cooling;
  }
  assessor_.sample(now_ms, s.process_celsius, s.target_celsius, stage,
                   s.output_percent, s.probe_healthy);
  auto& view = views_[latest_profile_];
  view.report = assessor_.report();
  if (s.state != EngineState::Running && s.state != EngineState::Cooling) {
    view.report = assessor_.finish(s.state == EngineState::Complete);
    if (!controller_checked_at_start_ && view.report.status == ValidationStatus::Pass)
      view.report.status = ValidationStatus::Incomplete;
    active_run_ = false;
    repeats_[latest_profile_].record(view.report);
    view.consecutive_passes = repeats_[latest_profile_].consecutivePasses();
    view.eligible = repeats_[latest_profile_].eligible(identity(latest_profile_, engine));
    view.eligible = view.eligible &&
                    (engine.study().checked_scope_mask & engine.study().required_checks_mask) ==
                        engine.study().required_checks_mask &&
                    engine.study().checked_setup_revision == engine.study().setup_revision;
    view.commissioned = false;
    terminal_pending_ = true;
    ++sequence_;
  }
  // Trial TAL uses the existing whole-second engine counter, not a PASS criterion.
  if (latest_profile_ == 0U && s.recipe && s.recipe->id == RecipeId::LeadedReflow &&
      !validRequirements(requirementsForProfile(0U)) &&
      view.report.status != ValidationStatus::Incomplete && view.report.metrics.sample_count) {
    view.report.metrics.liquidus_accumulated_ms = s.liquidus_elapsed_seconds * 1000U;
    view.report.liquidus.measured = true;
    view.report.liquidus.value = static_cast<float>(s.liquidus_elapsed_seconds);
    view.report.liquidus.status = ValidationStatus::CriteriaMissing;
  }
}

bool ValidationRuntime::commission(uint8_t profile, const ThermalEngine& engine) {
  if (profile >= 4 || engine.snapshot().state != EngineState::Idle ||
      !validRequirements(requirementsForProfile(profile)) ||
      (engine.study().checked_scope_mask & engine.study().required_checks_mask) !=
          engine.study().required_checks_mask ||
      engine.study().checked_setup_revision != engine.study().setup_revision) return false;
  refreshConfiguration(engine);
  if (!repeats_[profile].commission(identity(profile, engine))) return false;
  views_[profile].commissioned = true;
  views_[profile].eligible = true;
  latest_profile_ = profile;
  terminal_pending_ = true;
  ++sequence_;
  return true;
}

void ValidationRuntime::loadHistorical(uint8_t profile, const ValidationReport& report) {
  if (profile >= 4 || active_run_) return;
  views_[profile].report = report;
  views_[profile].needs_revalidation = true;
  views_[profile].consecutive_passes = 0;
  views_[profile].eligible = views_[profile].commissioned = false;
  repeats_[profile].reset();
  // Reset could have interrupted an unrecorded attempt: restore measurements,
  // never silently resurrect a passing sequence or a commissioned flag.
  run_id_ = std::max(run_id_, report.identity.run_id);
}
