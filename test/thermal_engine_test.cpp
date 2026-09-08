#include <cassert>
#include <cstdio>
#include <initializer_list>
#include <limits>

#include "max31855_decode.h"
#include "thermal_engine.h"

namespace {

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

void assertOff(const ThermalEngine& engine) {
  assert(!engine.heaterCommand());
  assert(engine.snapshot().output_percent == 0.0F);
}

void testDeadline(RecipeId id, uint32_t start_ms) {
  ThermalEngine engine;
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

}  // namespace

int main() {
  for (RecipeId id : {RecipeId::LeadedReflow, RecipeId::Nylon6Anneal, RecipeId::ChamberHold,
                     RecipeId::Commission100, RecipeId::Commission150, RecipeId::Commission200}) {
    testDeadline(id, 1000U);
    testDeadline(id, UINT32_MAX - 10000U);
  }
  for (RecipeId id : {RecipeId::Commission100, RecipeId::Commission150, RecipeId::Commission200}) {
    testCommissioning(id);
  }
  testInvalidDataAndLongHold();
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
