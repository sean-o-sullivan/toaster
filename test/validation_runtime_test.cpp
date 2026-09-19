#include <cassert>
#include <cstdio>

#define private public
#include "validation_runtime.h"
#undef private
#include "validation_storage.h"
#include "tune_history.h"

static ThermocoupleReading probe(uint32_t now, float temp = 25.0F) {
  ThermocoupleReading r;
  r.celsius = temp;
  r.sample_ms = now;
  r.fault = ThermocoupleFault::None;
  return r;
}

int main() {
  ThermalEngine engine;
  ValidationRuntime runtime;
  assert(runtime.views()[0].report.status == ValidationStatus::CriteriaMissing);
  assert(!runtime.commission(0, engine));
  assert(engine.start(RecipeId::LeadedReflow, 1000, probe(1000)));
  runtime.start(engine, 1000);
  engine.update(1100, probe(1100, 25.25F));
  runtime.update(engine, 1100);
  engine.snapshot_.state = EngineState::Complete;
  engine.snapshot_.heater_commanded_on = false;
  runtime.update(engine, 1200);
  const auto first = runtime.views()[0].report;
  assert(first.status == ValidationStatus::CriteriaMissing);
  assert(first.metrics.peak_measured);
  assert(!runtime.views()[0].eligible);
  assert(runtime.terminalPending());
  const uint32_t sequence = runtime.sequence();
  runtime.update(engine, 1300);
  assert(runtime.sequence() == sequence);
  runtime.persisted();

  SavedValidationReport stored{};
  stored.report = first;
  stored.checksum = validationChecksum(stored);
  assert(validSavedValidation(stored));
  stored.report.metrics.peak_celsius += 1.0F;
  assert(!validSavedValidation(stored));
  ValidationRuntime rebooted;
  rebooted.loadHistorical(0, first);
  assert(rebooted.views()[0].needs_revalidation);
  assert(rebooted.views()[0].consecutive_passes == 0);
  assert(!rebooted.views()[0].commissioned);

  engine.acknowledge();
  assert(engine.start(RecipeId::LeadedReflow, 2000, probe(2000)));
  runtime.start(engine, 2000);
  // Assessor stops sampling on a gap; runtime must still close the attempt.
  engine.update(3501, probe(3501));
  runtime.update(engine, 3501);
  assert(!runtime.assessor_.running());
  assert(runtime.active_run_);
  engine.abort();
  runtime.update(engine, 3600);
  assert(!runtime.active_run_);
  assert(runtime.views()[0].report.status == ValidationStatus::Incomplete);
  assert(runtime.views()[0].consecutive_passes == 0);
  assert(runtime.terminalPending());

  const auto old_identity = runtime.views()[0].report.identity;
  engine.acknowledge();
  assert(engine.loadPid(engine.study().active, 1, 1));
  assert(runtime.identity(0, engine).gain_revision != old_identity.gain_revision);
  runtime.refreshConfiguration(engine);
  assert(runtime.views()[0].needs_revalidation);

  AnnealProgram program;
  program.target_celsius = 120;
  program.soak_seconds = 600;
  program.ramp_celsius_per_minute = 10;
  assert(engine.startAnneal(program, 4000, probe(4000)));
  runtime.start(engine, 4000);
  const auto custom_identity = runtime.views()[2].report.identity;
  assert(runtime.active_run_ && runtime.latestProfile() == 2);
  assert(custom_identity.recipe_revision != runtime.recipe_revisions_[3]);
  engine.snapshot_.phase_index = 2;
  assert(engine.snapshot().recipe->phases[2].kind == PhaseKind::ControlledCool);
  engine.snapshot_.process_celsius = 100;
  runtime.update(engine, 4100);
  assert(runtime.assessor_.last_stage_ == ValidationStage::Cooling);
  engine.abort();
  runtime.update(engine, 4200);
  assert(runtime.views()[2].report.status == ValidationStatus::Incomplete);
  engine.acknowledge();
  program.soak_seconds = 1200;
  assert(engine.startAnneal(program, 4300, probe(4300)));
  runtime.start(engine, 4300);
  assert(runtime.views()[2].report.identity.recipe_revision != custom_identity.recipe_revision);
  assert(runtime.views()[2].consecutive_passes == 0);
  engine.abort();
  runtime.update(engine, 4400);

  TuneHistory history{};
  history.terminal = true;
  history.outcome = TuneOutcome::Timeout;
  history.checksum = tuneHistoryChecksum(history);
  assert(validTuneHistory(history));
  history.count = 11;
  history.checksum = tuneHistoryChecksum(history);
  assert(!validTuneHistory(history));
  std::puts("Validation runtime, missing criteria, gap lifecycle and storage tests passed");
}
