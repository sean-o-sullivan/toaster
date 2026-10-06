#include <cassert>
#include <cstdio>
#include <initializer_list>
#include <limits>

#include "max31855_decode.h"
#include "thermal_engine.h"

namespace {

static_assert(static_cast<uint8_t>(RecipeId::ValidateA) == 8, "legacy recipe ID changed");
static_assert(static_cast<uint8_t>(RecipeId::ValidateB) == 9, "legacy recipe ID changed");
static_assert(static_cast<uint8_t>(RecipeId::ValidateC) == 10, "legacy recipe ID changed");
static_assert(static_cast<uint8_t>(RecipeId::Check100) == 11, "check recipe ID changed");
static_assert(static_cast<uint8_t>(RecipeId::Check150) == 12, "check recipe ID changed");
static_assert(static_cast<uint8_t>(RecipeId::Check200) == 13, "check recipe ID changed");
static_assert(static_cast<uint8_t>(RecipeId::CustomAnneal) == 14, "custom recipe ID changed");
static_assert(static_cast<uint8_t>(PhaseKind::ControlledCool) == 3, "phase ID changed");

ThermocoupleReading healthy(float celsius, uint32_t sample_ms) {
  ThermocoupleReading reading;
  reading.celsius = celsius;
  reading.fault = ThermocoupleFault::None;
  reading.sample_ms = sample_ms;
  return reading;
}

ThermocoupleReading failed(ThermocoupleFault fault) {
  ThermocoupleReading reading;
  reading.fault = fault;
  return reading;
}

AnnealProgram anneal(float target_celsius, uint32_t soak_seconds,
                     float ramp_celsius_per_minute) {
  AnnealProgram program;
  program.target_celsius = target_celsius;
  program.soak_seconds = soak_seconds;
  program.ramp_celsius_per_minute = ramp_celsius_per_minute;
  return program;
}

void assertOff(const ThermalEngine& engine) {
  assert(!engine.heaterCommand());
  assert(engine.snapshot().output_percent == 0.0F);
}

void testDeadline(RecipeId id, uint32_t start_ms) {
  ThermalEngine engine;
  assert(engine.configureControlChecks(kControlCheck100Mask, kAllControlChecksMask, 1));
  assert(engine.start(id, start_ms, healthy(25.0F, start_ms)));
  const uint32_t limit_ms = recipeFor(id).maximum_run_seconds * 1000U;
  const uint32_t before = start_ms + limit_ms - 1U;
  engine.update(before, healthy(25.0F, before));
  assert(engine.snapshot().state == EngineState::Running);
  // A repeated Start cannot extend the original deadline.
  assert(!engine.start(id, before, healthy(25.0F, before)));
  const uint32_t deadline = start_ms + limit_ms;
  engine.update(deadline, healthy(25.0F, deadline));
  assert(engine.snapshot().fault == FaultCode::RunTimeout);
  assertOff(engine);
  assert(!engine.start(id, deadline, healthy(25.0F, deadline)));
  engine.update(deadline + 1U, healthy(25.0F, deadline + 1U));
  assert(engine.snapshot().state == EngineState::Fault);
  assertOff(engine);
  engine.acknowledge();
  assert(engine.start(id, deadline, healthy(25.0F, deadline)));
}

void testCommissioning(RecipeId id) {
  const auto& recipe = recipeFor(id);
  assert(recipe.maximum_run_seconds == 1200U);
  assert(recipe.maximum_output_percent == 25.0F);
  assert(recipe.phases[0].rate_celsius_per_second == 0.5F);
  assert(recipe.phases[1].duration_seconds == 300U);
  ThermalEngine engine;
  assert(engine.configureControlChecks(kControlCheck100Mask, kAllControlChecksMask, 1));
  assert(!engine.start(id, 0, healthy(60.25F, 0)));
  assert(engine.snapshot().fault == FaultCode::TestStartTooHot);
  engine.acknowledge();
  assert(engine.start(id, 0, healthy(60.0F, 0)));
  engine.abort();
  assertOff(engine);
  engine.acknowledge();
  assert(engine.start(id, 0, healthy(25.0F, 0)));
  engine.update(20000, healthy(25.0F, 20000));
  assert(engine.snapshot().output_percent == 25.0F);
  assert(engine.heaterCommand());
  engine.abort();
  engine.acknowledge();
  assert(engine.start(id, 0, healthy(25.0F, 0)));
  uint32_t now = 0;
  bool saw_heat = false;
  for (now = 100; now < 1200000U; now += 100) {
    // Synthetic 3 C tracking lag tests state logic, not real oven dynamics.
    const float target = engine.snapshot().target_celsius;
    const float process = target > 28.0F ? target - 3.0F : 25.0F;
    engine.update(now, healthy(process, now));
    assert(engine.snapshot().output_percent >= 0.0F);
    assert(engine.snapshot().output_percent <= 25.0F);
    saw_heat = saw_heat || engine.heaterCommand();
    if (engine.snapshot().state == EngineState::Cooling) {
      break;
    }
    assert(engine.snapshot().state == EngineState::Running);
  }
  assert(saw_heat);
  assert(engine.snapshot().state == EngineState::Cooling);
  assertOff(engine);
  engine.update(now + 100U, healthy(60.0F, now + 100U));
  assert(engine.snapshot().state == EngineState::Complete);
  assertOff(engine);
  // Completion never advances to another temperature or releases SAC305.
  assert(!engine.start(RecipeId::Sac305Reflow, now + 100U,
                       healthy(25.0F, now + 100U)));
  engine.acknowledge();
  assert(!engine.start(RecipeId::Sac305Reflow, now + 100U,
                       healthy(25.0F, now + 100U)));
  assert(engine.snapshot().fault == FaultCode::HighTemperatureProfileNotCommissioned);

  engine.acknowledge();
  assert(engine.start(id, 0, healthy(25.0F, 0)));
  engine.update(100, healthy(recipe.maximum_process_celsius + 0.25F, 100));
  assert(engine.snapshot().fault == FaultCode::ProcessOverTemperature);
  assertOff(engine);
}

void testInvalidDataAndLongHold() {
  ThermalEngine engine;
  for (uint32_t raw : {0U, UINT32_MAX, 0x00640208U}) {
    const auto bad = decodeMax31855(raw, 0);
    assert(!engine.start(RecipeId::ChamberHold, 0, bad));
    assertOff(engine);
    engine.acknowledge();
    assert(engine.start(RecipeId::ChamberHold, 0, healthy(25.0F, 0)));
    engine.update(100, decodeMax31855(raw, 100));
    assert(engine.snapshot().fault == FaultCode::ProcessProbe);
    assertOff(engine);
    engine.acknowledge();
  }
  assert(!engine.start(RecipeId::ChamberHold, 0,
                       healthy(std::numeric_limits<float>::quiet_NaN(), 0)));
  engine.acknowledge();
  assert(!engine.start(RecipeId::ChamberHold, 0, healthy(std::numeric_limits<float>::infinity(), 0)));
  engine.acknowledge();
  assert(!engine.start(RecipeId::ChamberHold, 0, healthy(141.0F, 0)));
  assertOff(engine);
  engine.acknowledge();

  // Repeated excursions reset the continuous hold timer, not the total deadline.
  assert(engine.start(RecipeId::ChamberHold, 0, healthy(120.0F, 0)));
  engine.update(100, healthy(120.0F, 100));
  assert(engine.snapshot().phase_index == 1U);
  for (uint32_t t = 1000; t < 7200000U; t += 1000) {
    engine.update(t, healthy((t / 1000U) % 2U ? 120.0F : 125.0F, t));
    assert(engine.snapshot().state == EngineState::Running);
  }
  engine.update(7200000U, healthy(120.0F, 7200000U));
  assert(engine.snapshot().fault == FaultCode::RunTimeout);
  assertOff(engine);
}

void testRecipeAndApprovalGuards() {
  ThermalEngine engine;
  assert(recipeFor(RecipeId::ValidateA).id == RecipeId::Invalid);
  assert(recipeFor(static_cast<RecipeId>(99)).id == RecipeId::Invalid);
  for (RecipeId id : {RecipeId::ValidateA, RecipeId::ValidateB, RecipeId::ValidateC,
                      static_cast<RecipeId>(99)}) {
    assert(!engine.start(id, 0, healthy(25.0F, 0)));
    assert(engine.snapshot().fault == FaultCode::InvalidRecipe);
    assertOff(engine);
    engine.acknowledge();
  }

  for (RecipeId id : {RecipeId::Commission150, RecipeId::Commission200,
                      RecipeId::Check150, RecipeId::Check200}) {
    assert(!engine.start(id, 0, healthy(25.0F, 0)));
    assert(engine.snapshot().fault == FaultCode::CheckNotApproved);
    assertOff(engine);
    engine.acknowledge();
  }
  assert(!engine.configureControlChecks(kControlCheck100Mask, kControlCheck100Mask, 0));
  assert(!engine.configureControlChecks(0, kControlCheck100Mask, 1));
  assert(!engine.configureControlChecks(kControlCheck150Mask, kControlCheck100Mask, 1));
  assert(!engine.configureControlChecks(0x80U, 0x80U, 1));
  assert(engine.configureControlChecks(kControlCheck100Mask,
                                       kControlCheck100Mask | kControlCheck150Mask, 7));
  assert(engine.configureControlChecks(kControlCheck100Mask,
                                       kControlCheck100Mask | kControlCheck150Mask, 7));
  assert(engine.start(RecipeId::Commission150, 0, healthy(25.0F, 0)));
  assert(!engine.configureControlChecks(kControlCheck100Mask, kAllControlChecksMask, 7));
  engine.abort();
  engine.acknowledge();
  assert(!engine.start(RecipeId::Check150, 0, healthy(25.0F, 0)));
  assert(engine.snapshot().fault == FaultCode::NoTuneCandidate);
  assertOff(engine);
  engine.acknowledge();
  for (RecipeId id : {RecipeId::Commission200, RecipeId::Check200}) {
    assert(!engine.start(id, 0, healthy(25.0F, 0)));
    assert(engine.snapshot().fault == FaultCode::CheckNotApproved);
    assertOff(engine);
    engine.acknowledge();
  }
}

void testControlCheckOutputCaps() {
  const auto& trial = recipeFor(RecipeId::LeadedReflow);
  assert(trial.maximum_output_percent == 50.0F);
  assert(trial.maximum_process_celsius == 220.0F);
  assert(trial.maximum_run_seconds == 1800U);
  assert(recipeHeatingDeadlineSeconds(trial) == 1800U);
  assert(trial.liquidus_celsius == 183.0F);
  assert(trial.phase_count == 5U);
  assert(trial.phases[0].target_celsius == 150.0F);
  assert(trial.phases[0].rate_celsius_per_second == 1.0F);
  assert(trial.phases[1].duration_seconds == 60U);
  assert(trial.phases[2].target_celsius == 205.0F);
  assert(trial.phases[2].rate_celsius_per_second == 1.2F);
  assert(trial.phases[3].duration_seconds == 20U);
  assert(trial.phases[4].kind == PhaseKind::Cooldown);
  assert(trial.phases[4].target_celsius == 60.0F);
  for (RecipeId id : {RecipeId::Autotune100, RecipeId::Commission100,
                     RecipeId::Commission150, RecipeId::Commission200}) {
    assert(recipeFor(id).maximum_output_percent == 25.0F);
  }
  PidGains gains;
  gains.kp = 4.4263F;
  gains.ki = 0.020742F;
  gains.kd = 68.151F;
  for (RecipeId id : {RecipeId::Check100, RecipeId::Check150, RecipeId::Check200,
                      RecipeId::LeadedReflow}) {
    const float cap = (id == RecipeId::Check200 || id == RecipeId::LeadedReflow) ? 50.0F : 25.0F;
    assert(recipeFor(id).maximum_output_percent == cap);
    for (unsigned ending = 0; ending < 4; ++ending) {
      ThermalEngine engine;
      assert(engine.configureControlChecks(kControlCheck100Mask, kAllControlChecksMask, 1U));
      assert(engine.loadPid(gains, kControlCheck100Mask | kControlCheck150Mask, 1U));
      assert(engine.start(id, 0U, healthy(25.0F, 0U)));
      engine.update(200000U, healthy(25.0F, 200000U));
      assert(engine.snapshot().output_percent == cap);
      assert(engine.heaterCommand());
      assert(engine.runGains().kp == gains.kp);
      assert(engine.runGains().ki == gains.ki);
      assert(engine.runGains().kd == gains.kd);
      assert(engine.study().checked_scope_mask ==
          (kControlCheck100Mask | kControlCheck150Mask));
      const uint32_t pulse_end_ms = 200000U + static_cast<uint32_t>(cap * 50.0F);
      engine.update(pulse_end_ms - 1U, healthy(25.0F, pulse_end_ms - 1U));
      assert(engine.snapshot().output_percent == cap && engine.heaterCommand());
      engine.update(pulse_end_ms, healthy(25.0F, pulse_end_ms));
      assert(engine.snapshot().output_percent == cap && !engine.heaterCommand());
      engine.update(205000U, healthy(25.0F, 205000U));
      assert(engine.snapshot().output_percent == cap && engine.heaterCommand());
      if (ending == 0) {
        engine.abort();
        assert(engine.snapshot().state == EngineState::Aborted);
      } else if (ending == 1) {
        auto bad = healthy(25.0F, 205100U);
        bad.fault = ThermocoupleFault::OpenCircuit;
        engine.update(205100U, bad);
        assert(engine.snapshot().fault == FaultCode::ProcessProbe);
      } else if (ending == 2) {
        engine.update(206001U, healthy(25.0F, 205000U));
        assert(engine.snapshot().fault == FaultCode::ProcessProbe);
      } else {
        engine.update(205100U, healthy(recipeFor(id).maximum_process_celsius + 0.25F, 205100U));
        assert(engine.snapshot().fault == FaultCode::ProcessOverTemperature);
      }
      assertOff(engine);
    }
  }
}

void startCheckCooling(ThermalEngine& engine, RecipeId id, uint32_t start_ms) {
  const float target = controlCheckTargetCelsius(controlCheckIndex(id));
  assert(engine.configureControlChecks(kControlCheck100Mask, kAllControlChecksMask, 1U));
  assert(engine.loadPid(PidGains{}, kControlCheck100Mask, 1U));
  assert(engine.start(id, start_ms, healthy(25.0F, start_ms)));
  engine.update(start_ms + 400000U, healthy(target, start_ms + 400000U));
  assert(engine.snapshot().phase_index == 1U);
  engine.update(start_ms + 400100U, healthy(target, start_ms + 400100U));
  engine.update(start_ms + 700100U, healthy(target, start_ms + 700100U));
  assert(engine.snapshot().state == EngineState::Cooling);
  assertOff(engine);
}

void testCheckCooldownAllowance(RecipeId id, uint32_t start_ms) {
  const Recipe& recipe = recipeFor(id);
  const int index = controlCheckIndex(id);
  const float target = controlCheckTargetCelsius(index);
  const uint32_t total_ms = recipe.maximum_run_seconds * 1000U;
  assert(recipe.maximum_run_seconds == (id == RecipeId::Check150 ? 1500U : 1800U));
  assert(recipeHeatingDeadlineSeconds(recipe) == 1200U);
  assert(recipe.maximum_process_celsius == target + 20.0F);
  assert(recipe.maximum_output_percent == (id == RecipeId::Check200 ? 50.0F : 25.0F));
  for (RecipeId id : {RecipeId::Check100,
                     RecipeId::Commission100, RecipeId::Commission150, RecipeId::Commission200}) {
    assert(recipeFor(id).maximum_run_seconds == 1200U);
    assert(recipeHeatingDeadlineSeconds(recipeFor(id)) == 1200U);
  }

  // Heating cannot consume the extra cooling-only allowance.
  for (bool stalled_hold : {false, true}) {
    ThermalEngine engine;
    assert(engine.configureControlChecks(kControlCheck100Mask, kAllControlChecksMask, 1U));
    assert(engine.loadPid(PidGains{}, kControlCheck100Mask, 1U));
    assert(engine.start(id, start_ms, healthy(25.0F, start_ms)));
    if (stalled_hold) {
      engine.update(start_ms + 400000U, healthy(target, start_ms + 400000U));
      assert(engine.snapshot().phase_index == 1U);
    }
    const float temperature = stalled_hold ? target - 5.0F : 25.0F;
    engine.update(start_ms + 1199999U, healthy(temperature, start_ms + 1199999U));
    assert(engine.snapshot().state == EngineState::Running);
    assert(!engine.start(id, start_ms + 1199999U,
        healthy(temperature, start_ms + 1199999U)));
    engine.update(start_ms + 1200000U, healthy(temperature, start_ms + 1200000U));
    assert(engine.snapshot().fault == FaultCode::RunTimeout);
    assert(engine.study().checks[index].result == StudyResult::Failed);
    assertOff(engine);
  }

  for (unsigned ending = 0; ending < 6; ++ending) {
    ThermalEngine engine;
    startCheckCooling(engine, id, start_ms);
    engine.update(start_ms + 1200000U, healthy(66.75F, start_ms + 1200000U));
    assert(engine.snapshot().state == EngineState::Cooling);
    assert(engine.study().checks[index].result == StudyResult::Running);
    assertOff(engine);
    if (ending == 0) {
      const uint32_t completed_ms = id == RecipeId::Check150 ? 1286000U : 1650000U;
      engine.update(start_ms + completed_ms, healthy(60.0F, start_ms + completed_ms));
      assert(engine.snapshot().state == EngineState::Complete);
      assert(engine.study().checks[index].result == StudyResult::Complete);
    } else if (ending == 1) {
      engine.update(start_ms + total_ms - 1U, healthy(60.25F, start_ms + total_ms - 1U));
      assert(engine.snapshot().state == EngineState::Cooling);
      assertOff(engine);
      assert(!engine.start(id, start_ms + total_ms - 1U,
          healthy(60.25F, start_ms + total_ms - 1U)));
      engine.update(start_ms + total_ms, healthy(60.0F, start_ms + total_ms));
      assert(engine.snapshot().fault == FaultCode::RunTimeout);
      assert(engine.study().checks[index].result == StudyResult::Failed);
      engine.update(start_ms + total_ms + 1U, healthy(50.0F, start_ms + total_ms + 1U));
      assert(engine.snapshot().state == EngineState::Fault);
    } else if (ending == 2) {
      auto bad = healthy(65.0F, start_ms + 1200100U);
      bad.fault = ThermocoupleFault::OpenCircuit;
      engine.update(start_ms + 1200100U, bad);
      assert(engine.snapshot().fault == FaultCode::ProcessProbe);
    } else if (ending == 3) {
      engine.update(start_ms + 1200100U, healthy(recipe.maximum_process_celsius + 0.25F, start_ms + 1200100U));
      assert(engine.snapshot().fault == FaultCode::ProcessOverTemperature);
    } else if (ending == 4) {
      engine.abort();
      assert(engine.snapshot().state == EngineState::Aborted);
      assert(engine.study().checks[index].result == StudyResult::Aborted);
    } else {
      engine.update(start_ms + 1201001U, healthy(65.0F, start_ms + 1200000U));
      assert(engine.snapshot().fault == FaultCode::ProcessProbe);
    }
    assertOff(engine);
  }
}

void testAnnealProgramValidation() {
  const AnnealProgram default_program;
  assert(!validAnnealProgram(default_program));

  const AnnealProgram nominal = anneal(120.0F, 60U, 60.0F);
  assert(validAnnealProgram(nominal));
  assert(annealRampSeconds(nominal, 60.0F) == 60U);
  assert(annealCoolingSeconds(nominal) == 60U);
  assert(annealEstimatedTotalSeconds(nominal, 60.0F) == 180U);
  assert(annealFitsBudget(nominal, 60.0F));
  assert(annealRampSeconds(nominal, 121.0F) == kInvalidAnnealEstimateSeconds);

  assert(validAnnealProgram(anneal(60.0F, 60U, 1.0F)));
  assert(validAnnealProgram(anneal(180.0F, 6600U, 60.0F)));
  assert(!validAnnealProgram(anneal(59.9F, 60U, 1.0F)));
  assert(!validAnnealProgram(anneal(180.1F, 60U, 1.0F)));
  assert(!validAnnealProgram(anneal(120.0F, 59U, 1.0F)));
  assert(!validAnnealProgram(anneal(120.0F, 6601U, 1.0F)));
  assert(!validAnnealProgram(anneal(120.0F, 60U, 0.9F)));
  assert(!validAnnealProgram(anneal(120.0F, 60U, 60.1F)));
  assert(!validAnnealProgram(
      anneal(std::numeric_limits<float>::quiet_NaN(), 60U, 1.0F)));
  assert(!validAnnealProgram(
      anneal(120.0F, 60U, std::numeric_limits<float>::infinity())));

  const AnnealProgram exact_deadline = anneal(60.0F, 6600U, 1.0F);
  assert(annealEstimatedTotalSeconds(exact_deadline, 50.0F) == 7200U);
  assert(!annealFitsBudget(exact_deadline, 50.0F));
  assert(!annealFitsBudget(anneal(180.0F, 6600U, 1.0F), 25.0F));
}

void testAnnealStartGuardsAndFrozenRecipe() {
  ThermalEngine engine;
  assert(recipeFor(RecipeId::CustomAnneal).id == RecipeId::Invalid);
  assert(!engine.start(RecipeId::CustomAnneal, 0, healthy(25.0F, 0)));
  assert(engine.snapshot().fault == FaultCode::InvalidAnneal);
  assertOff(engine);
  engine.acknowledge();

  const AnnealProgram invalid;
  assert(!engine.startAnneal(invalid, 0,
                             failed(ThermocoupleFault::OpenCircuit)));
  assert(engine.snapshot().fault == FaultCode::ProcessProbe);
  engine.acknowledge();
  assert(!engine.startAnneal(invalid, 0, healthy(201.0F, 0)));
  assert(engine.snapshot().fault == FaultCode::ProcessOverTemperature);
  engine.acknowledge();
  assert(!engine.startAnneal(invalid, 0, healthy(25.0F, 0)));
  assert(engine.snapshot().fault == FaultCode::InvalidAnneal);
  engine.acknowledge();

  assert(!engine.startAnneal(anneal(120.0F, 60U, 60.0F), 0,
                             healthy(120.1F, 0)));
  assert(engine.snapshot().fault == FaultCode::InvalidAnneal);
  engine.acknowledge();
  assert(!engine.startAnneal(anneal(60.0F, 6600U, 1.0F), 0,
                             healthy(50.0F, 0)));
  assert(engine.snapshot().fault == FaultCode::InvalidAnneal);
  engine.acknowledge();

  AnnealProgram program = anneal(120.0F, 60U, 30.0F);
  assert(engine.startAnneal(program, 1000U, healthy(25.0F, 1000U)));
  assert(engine.snapshot().recipe == &engine.configuredAnnealRecipe());
  assert(engine.snapshot().recipe->id == RecipeId::CustomAnneal);
  assert(engine.snapshot().recipe->maximum_process_celsius == 200.0F);
  assert(engine.snapshot().recipe->maximum_run_seconds == 7200U);
  assert(engine.snapshot().recipe->maximum_output_percent == 100.0F);
  assert(engine.snapshot().recipe->phase_count == 4U);
  assert(engine.snapshot().recipe->phases[0].kind == PhaseKind::Ramp);
  assert(engine.snapshot().recipe->phases[0].target_celsius == 120.0F);
  assert(engine.snapshot().recipe->phases[0].rate_celsius_per_second == 0.5F);
  assert(engine.snapshot().recipe->phases[1].kind == PhaseKind::Hold);
  assert(engine.snapshot().recipe->phases[1].duration_seconds == 60U);
  assert(engine.snapshot().recipe->phases[2].kind == PhaseKind::ControlledCool);
  assert(engine.snapshot().recipe->phases[2].target_celsius == 60.0F);
  assert(engine.snapshot().recipe->phases[2].rate_celsius_per_second == 0.5F);
  assert(engine.snapshot().recipe->phases[3].kind == PhaseKind::Cooldown);

  program = anneal(180.0F, 600U, 60.0F);
  assert(!engine.startAnneal(program, 1100U, healthy(25.0F, 1100U)));
  assert(engine.configuredAnnealProgram().target_celsius == 120.0F);
  assert(engine.snapshot().recipe->phases[0].target_celsius == 120.0F);
  engine.abort();
  assert(engine.snapshot().state == EngineState::Aborted);
  assertOff(engine);
}

void testAnnealFaultsAndTimeout() {
  ThermalEngine engine;
  const AnnealProgram program = anneal(120.0F, 60U, 60.0F);
  assert(engine.startAnneal(program, 0, healthy(25.0F, 0)));
  engine.update(100U, failed(ThermocoupleFault::OpenCircuit));
  assert(engine.snapshot().fault == FaultCode::ProcessProbe);
  assertOff(engine);
  engine.acknowledge();

  assert(engine.startAnneal(program, 0, healthy(25.0F, 0)));
  engine.update(100U, healthy(200.1F, 100U));
  assert(engine.snapshot().fault == FaultCode::ProcessOverTemperature);
  assertOff(engine);
  engine.acknowledge();

  const AnnealProgram long_program = anneal(180.0F, 6600U, 60.0F);
  assert(engine.startAnneal(long_program, 0, healthy(60.0F, 0)));
  engine.update(7200000U, healthy(60.0F, 7200000U));
  assert(engine.snapshot().fault == FaultCode::RunTimeout);
  assertOff(engine);
}

void testAnnealControlledCooling() {
  ThermalEngine engine;
  const AnnealProgram program = anneal(120.0F, 60U, 60.0F);
  assert(engine.startAnneal(program, 0, healthy(60.0F, 0)));

  engine.update(60000U, healthy(120.0F, 60000U));
  assert(engine.snapshot().phase_index == 1U);
  engine.update(60001U, healthy(120.0F, 60001U));
  engine.update(120001U, healthy(120.0F, 120001U));
  assert(engine.snapshot().phase_index == 2U);
  assert(engine.snapshot().state == EngineState::Running);
  assertOff(engine);

  // Heating cannot masquerade as active cooling above the descending setpoint.
  engine.update(130001U, healthy(111.0F, 130001U));
  assert(engine.snapshot().target_celsius == 110.0F);
  assertOff(engine);

  // Above-target observations still update PID history. A later small crossing
  // cannot create a stale-measurement derivative spike.
  engine.update(140001U, healthy(101.0F, 140001U));
  assert(engine.snapshot().target_celsius == 100.0F);
  assertOff(engine);
  engine.update(150001U, healthy(91.0F, 150001U));
  assertOff(engine);
  engine.update(160001U, healthy(81.0F, 160001U));
  assertOff(engine);
  engine.update(161001U, healthy(78.5F, 161001U));
  assert(engine.snapshot().output_percent > 0.0F);
  assert(engine.snapshot().output_percent < 25.0F);

  // The heater may brake excessive passive cooling from below the setpoint.
  engine.update(165000U, healthy(65.0F, 165000U));
  assert(engine.heaterCommand());

  // Crossing into ordinary Cooling gates output off in the same update.
  engine.update(180001U, healthy(63.0F, 180001U));
  assert(engine.snapshot().phase_index == 3U);
  assert(engine.snapshot().state == EngineState::Cooling);
  assertOff(engine);
  engine.update(181001U, healthy(61.0F, 181001U));
  assert(engine.snapshot().state == EngineState::Cooling);
  assertOff(engine);
  engine.update(182001U, healthy(60.0F, 182001U));
  assert(engine.snapshot().state == EngineState::Complete);
  assertOff(engine);
}

}  // namespace

int main() {
  assert(recipeFor(RecipeId::Autotune100).maximum_run_seconds == 1800U);
  testDeadline(RecipeId::Autotune100, 1000U);
  testDeadline(RecipeId::Autotune100, UINT32_MAX - 10000U);
  {
    ThermalEngine recovery;
    assert(!recovery.recoverTuneCandidate());
    TuneRunReport report;
    report.available = report.terminal = true;
    report.outcome = TuneOutcome::Timeout;
    report.cycles = 9;
    report.latest.window_count = 3;
    for (unsigned i = 0; i < 3; ++i) {
      auto& c = report.latest.window[i];
      c.cycle = 7 + i;
      c.period = 94.0F;
      c.heat_seconds = 27.4F;
      c.fraction = c.heat_seconds / c.period;
      c.minimum = 99.25F;
      c.maximum = 101.75F;
      c.amplitude = 1.25F;
      c.midpoint = 100.5F;
      c.failed_checks = TuneAmplitudeLow | TuneFractionLow;
    }
    assert(recovery.loadTuneReport(report));
    const PidGains active = recovery.study().active;
    assert(recovery.recoverTuneCandidate());
    assert(recovery.study().candidate_ready);
    assert(!recovery.canSaveStudy());
    assert(recovery.study().active.kp == active.kp);
    assert(recovery.study().tune_report.outcome == TuneOutcome::Timeout);
    assertOff(recovery);
    assert(!recovery.recoverTuneCandidate());
    assert(recovery.start(RecipeId::Check100, 0, healthy(25, 0)));
    assert(recovery.runGains().kp == recovery.study().candidate.kp);
    assert(!recovery.recoverTuneCandidate());
    ThermalEngine rejected;
    report.outcome = TuneOutcome::OtherFault;
    assert(rejected.loadTuneReport(report));
    assert(!rejected.recoverTuneCandidate());
    report.outcome = TuneOutcome::Timeout;
    report.latest.window[0].maximum = report.latest.window[0].minimum;
    assert(rejected.loadTuneReport(report));
    assert(!rejected.recoverTuneCandidate());
  }
  for (RecipeId id : {RecipeId::LeadedReflow, RecipeId::Nylon6Anneal, RecipeId::ChamberHold,
                     RecipeId::Commission100, RecipeId::Commission150, RecipeId::Commission200}) {
    testDeadline(id, 1000U);
    testDeadline(id, UINT32_MAX - 10000U);
  }
  for (RecipeId id : {RecipeId::Commission100, RecipeId::Commission150, RecipeId::Commission200}) {
    testCommissioning(id);
  }
  testInvalidDataAndLongHold();
  testRecipeAndApprovalGuards();
  testControlCheckOutputCaps();
  for (RecipeId id : {RecipeId::Check150, RecipeId::Check200}) {
    testCheckCooldownAllowance(id, 1000U);
    testCheckCooldownAllowance(id, UINT32_MAX - 10000U);
  }
  testAnnealProgramValidation();
  testAnnealStartGuardsAndFrozenRecipe();
  testAnnealFaultsAndTimeout();
  testAnnealControlledCooling();
  ThermalEngine engine;

  // A sample taken just after the loop timestamp is valid; this occurs at millisecond rollover.
  assert(engine.start(RecipeId::ChamberHold, 10, healthy(25.0F, 11)));
  engine.abort();
  engine.acknowledge();

  assert(!engine.start(RecipeId::LeadedReflow, 0, failed(ThermocoupleFault::OpenCircuit)));
  assert(engine.snapshot().state == EngineState::Fault);
  assert(engine.snapshot().fault == FaultCode::ProcessProbe);
  assert(!engine.heaterCommand());

  engine.acknowledge();
  assert(engine.snapshot().state == EngineState::Idle);
  assert(engine.start(RecipeId::LeadedReflow, 100, healthy(25.0F, 100)));
  engine.update(200, failed(ThermocoupleFault::OpenCircuit));
  assert(engine.snapshot().state == EngineState::Fault);
  assert(engine.snapshot().fault == FaultCode::ProcessProbe);
  assert(!engine.heaterCommand());

  engine.acknowledge();
  assert(engine.start(RecipeId::LeadedReflow, 300, healthy(25.0F, 300)));
  engine.update(400, healthy(231.0F, 400));
  assert(engine.snapshot().state == EngineState::Fault);
  assert(engine.snapshot().fault == FaultCode::ProcessOverTemperature);
  assert(!engine.heaterCommand());

  engine.acknowledge();
  assert(engine.start(RecipeId::LeadedReflow, 450, healthy(25.0F, 450)));
  engine.update(550, healthy(221.0F, 550));
  assert(engine.snapshot().state == EngineState::Fault);
  assert(engine.snapshot().fault == FaultCode::ProcessOverTemperature);
  assert(!engine.heaterCommand());

  engine.acknowledge();
  assert(!engine.start(RecipeId::Sac305Reflow, 500, healthy(25.0F, 500)));
  assert(engine.snapshot().fault == FaultCode::HighTemperatureProfileNotCommissioned);
  assert(!engine.heaterCommand());

  engine.acknowledge();
  assert(engine.start(RecipeId::ChamberHold, 600, healthy(25.0F, 600)));
  engine.abort();
  assert(engine.snapshot().state == EngineState::Aborted);
  assert(!engine.heaterCommand());

  // Entering the final cooldown phase must turn the heater off in the same update.
  engine.acknowledge();
  assert(engine.start(RecipeId::LeadedReflow, 1000, healthy(150.0F, 1000)));
  engine.update(1100, healthy(150.0F, 1100));
  assert(engine.snapshot().phase_index == 1);
  engine.update(1200, healthy(150.0F, 1200));
  engine.update(61300, healthy(150.0F, 61300));
  assert(engine.snapshot().phase_index == 2);
  engine.update(107300, healthy(205.0F, 107300));
  assert(engine.snapshot().phase_index == 3);
  engine.update(107400, healthy(205.0F, 107400));
  engine.update(127500, healthy(205.0F, 127500));
  assert(engine.snapshot().state == EngineState::Cooling);
  assert(engine.snapshot().phase_index == 4);
  assert(!engine.heaterCommand());

  // Time-above-liquidus accumulates only while the process reading is above 183 C.
  assert(engine.snapshot().liquidus_elapsed_seconds >= 66U);
  const uint32_t liquidus_seconds = engine.snapshot().liquidus_elapsed_seconds;
  engine.update(128500, healthy(170.0F, 128500));
  assert(engine.snapshot().liquidus_elapsed_seconds == liquidus_seconds);

  // Cooling also has a deadline; timeout cannot re-enable heat.
  engine.update(1800999U, healthy(80.0F, 1800999U));
  assert(engine.snapshot().state == EngineState::Cooling);
  assertOff(engine);
  engine.update(1801000U, healthy(80.0F, 1801000U));
  assert(engine.snapshot().fault == FaultCode::RunTimeout);
  assertOff(engine);

  // A valid-but-stale sample is a probe fault, not permission to keep heating.
  engine.abort();
  engine.acknowledge();
  assert(engine.start(RecipeId::ChamberHold, 200000, healthy(25.0F, 200000)));
  engine.update(201001, healthy(25.0F, 200000));
  assert(engine.snapshot().state == EngineState::Fault);
  assert(engine.snapshot().fault == FaultCode::ProcessProbe);
  assert(!engine.heaterCommand());
  std::puts("Thermal engine tests passed");
}
