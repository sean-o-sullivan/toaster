#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

#include "thermal_engine.h"
#include "pid_settings.h"

namespace {
ThermocoupleReading reading(float celsius, uint32_t now) {
  ThermocoupleReading r;
  r.celsius = celsius;
  r.sample_ms = now;
  r.fault = ThermocoupleFault::None;
  return r;
}

void off(const ThermalEngine& engine) {
  assert(!engine.heaterCommand());
  assert(engine.snapshot().output_percent == 0.0F);
}

void cycle(RelayAutotune& tune, uint32_t& t, float amplitude, uint32_t half_ms = 30000) {
  t += half_ms / 2; tune.update(t, 100.0F + amplitude);
  t += half_ms / 2; tune.update(t, 99.5F);
  t += half_ms / 2; tune.update(t, 100.0F - amplitude);
  t += half_ms / 2; tune.update(t, 100.5F);
}

void relayTests() {
  for (uint32_t start : {0U, UINT32_MAX - 10000U}) {
    RelayAutotune tune;
    tune.begin(start, 25.0F);
    assert(tune.demand() == 25.0F);
    uint32_t t = start + 1000;
    tune.update(t, 100.5F);
    assert(tune.demand() == 0.0F);
    for (int i = 0; i < 5; ++i) cycle(tune, t, 4.0F);
    assert(tune.ready() && !tune.failed());
    assert(tune.demand() == 0.0F);
    assert(std::fabs(tune.periodSeconds() - 60.0F) < 0.01F);
    assert(std::fabs(tune.amplitudeCelsius() - 4.0F) < 0.01F);
    const float expected_kp = (50.0F / (3.14159265F * 4.0F)) / 2.2F;
    assert(std::fabs(tune.gains().kp - expected_kp) < 0.001F);
    assert(std::fabs(tune.gains().ki - expected_kp / 132.0F) < 0.0001F);
    assert(std::fabs(tune.gains().kd - expected_kp * 60.0F / 6.3F) < 0.001F);
  }
  for (int kind = 0; kind < 3; ++kind) {
    RelayAutotune tune;
    tune.begin(0, 25);
    uint32_t t = 1000;
    tune.update(t, 100.5F);
    for (int i = 0; i < 10; ++i)
      cycle(tune, t, kind == 0 ? 0.5F : kind == 1 ? (i % 2 ? 6.0F : 3.0F) : 4.0F,
            kind == 2 ? 5000U : 30000U);
    assert(tune.failed() && !tune.ready());
    assert(tune.demand() == 0.0F);
  }
  RelayAutotune invalid;
  invalid.begin(0, 25);
  invalid.update(100, std::numeric_limits<float>::quiet_NaN());
  assert(invalid.failed() && invalid.demand() == 0);
  RelayAutotune asymmetric;
  asymmetric.begin(0, 25);
  uint32_t t = 1000;
  asymmetric.update(t, 100.5F);
  for (int i = 0; i < 10; ++i) {
    asymmetric.update(t + 30000, 104);
    asymmetric.update(t + 50000, 99.5F);
    asymmetric.update(t + 55000, 96);
    asymmetric.update(t + 60000, 100.5F);
    t += 60000;
  }
  assert(asymmetric.failed());
}

void safetyTests() {
  for (uint32_t start : {0U, UINT32_MAX - 10000U}) {
    ThermalEngine e;
    assert(!e.start(RecipeId::Autotune100, start, reading(60.25F, start)));
    assert(e.snapshot().fault == FaultCode::TestStartTooHot);
    e.acknowledge();
    assert(e.start(RecipeId::Autotune100, start, reading(25, start)));
    assert(!e.start(RecipeId::Autotune100, start + 100, reading(25, start + 100)));
    e.update(start + 100, reading(25, start + 100));
    assert(e.snapshot().output_percent == 25);
    assert(!e.loadPid(PidGains{}));
    assert(!e.acceptStudy());
    e.update(start + 1200000U, reading(25, start + 1200000U));
    assert(e.snapshot().fault == FaultCode::RunTimeout);
    off(e);
  }
  for (int kind = 0; kind < 4; ++kind) {
    ThermalEngine e;
    assert(e.start(RecipeId::Autotune100, 0, reading(25, 0)));
    auto r = reading(25, 100);
    if (kind == 0) r.fault = ThermocoupleFault::OpenCircuit;
    if (kind == 1) r.celsius = 120.25F;
    if (kind == 2) r.sample_ms = 0;
    e.update(kind == 2 ? 1001 : 100, r);
    if (kind == 3) e.abort();
    else assert(e.snapshot().state == EngineState::Fault);
    off(e);
    assert(!e.study().candidate_ready && !e.canSaveStudy());
  }
  ThermalEngine e;
  assert(!e.start(RecipeId::ValidateB, 0, reading(25, 0)));
  assert(e.snapshot().fault == FaultCode::NoTuneCandidate);
}

// Synthetic FOPDT plant: not a claim about the physical Cecotec oven.
struct Plant {
  float temperature = 25.0F;
  std::vector<float> delay = std::vector<float>(150, 0.0F);
  size_t next = 0;
  void step(bool heater, float gain = 6.0F) {
    const float input = delay[next];
    delay[next] = heater ? 100.0F : 0.0F;
    next = (next + 1) % delay.size();
    temperature += 0.1F * (25.0F + gain * input - temperature) / 120.0F;
  }
};

uint32_t simulate(ThermalEngine& e, RecipeId id, uint32_t start, float gain = 6.0F) {
  Plant p;
  assert(e.start(id, start, reading(p.temperature, start)));
  for (uint32_t elapsed = 100; elapsed <= 1200000U; elapsed += 100) {
    p.step(e.heaterCommand(), gain);
    e.update(start + elapsed, reading(p.temperature, start + elapsed));
    assert(e.snapshot().output_percent <= 25.0F);
    if (e.snapshot().state == EngineState::Complete) { off(e); return start + elapsed; }
    if (e.snapshot().state == EngineState::Fault) {
      std::fprintf(stderr, "simulation id=%u fault=%s t=%lu temp=%.2f cycles=%u\n",
                   static_cast<unsigned>(id), toString(e.snapshot().fault),
                   static_cast<unsigned long>(elapsed), p.temperature, e.study().cycles);
      assert(false);
    }
  }
  assert(false);
  return 0;
}

void studyTests() {
  ThermalEngine e;
  const PidGains original = e.study().active;
  uint32_t t = simulate(e, RecipeId::Autotune100, 1000);
  assert(e.study().candidate_ready);
  assert(e.study().active.kp == original.kp);
  const PidGains candidate = e.study().candidate;
  std::printf("Synthetic tune: P %.3f I %.5f D %.3f, period %.1fs amplitude %.2fC\n",
               candidate.kp, candidate.ki, candidate.kd, e.study().period_seconds, e.study().amplitude_celsius);
  assert(!e.acceptStudy());
  e.acknowledge();
  assert(!e.canSaveStudy());
  for (auto id : {RecipeId::ValidateA, RecipeId::ValidateB, RecipeId::ValidateC}) {
    t = simulate(e, id, t + 1000, id == RecipeId::ValidateB ? 5.8F : 6.0F);
    assert(e.runGains().kp == candidate.kp && e.runGains().ki == candidate.ki);
    const auto& result = e.study().points[studyPoint(id)];
    assert(result.result == StudyResult::Complete && result.attempts == 1);
    assert(result.reached_band && result.rise_seconds > 0);
    assert(result.peak_celsius < 120 && result.peak_celsius >= 96);
    assert(std::isfinite(result.hold_rmse) && result.hold_output_percent <= 25);
    assert(!e.canSaveStudy()); // Results cannot be accepted before acknowledgement.
    e.acknowledge();
  }
  assert(e.canSaveStudy());
  // Repeating B increments its attempt and a failure blocks saving.
  assert(e.start(RecipeId::ValidateB, t + 1000, reading(25, t + 1000)));
  assert(e.study().points[1].attempts == 2);
  e.abort(); e.acknowledge();
  assert(e.study().points[1].result == StudyResult::Aborted && !e.canSaveStudy());
  t = simulate(e, RecipeId::ValidateB, t + 2000);
  e.acknowledge();
  assert(e.canSaveStudy());
  assert(e.acceptStudy());
  assert(e.study().active.kp == candidate.kp);
  assert(e.study().saved);
  assert(!e.start(RecipeId::Sac305Reflow, t, reading(25, t)));
  assert(e.snapshot().fault == FaultCode::HighTemperatureProfileNotCommissioned);
  e.acknowledge();
  assert(e.start(RecipeId::Autotune100, t, reading(25, t)));
  assert(!e.study().candidate_ready && e.study().points[0].result == StudyResult::Empty);
  e.abort(); e.acknowledge();
  assert(e.study().active.kp == candidate.kp);
}

void coolingAndCandidateSafety() {
  ThermalEngine e;
  assert(e.start(RecipeId::Autotune100, 0, reading(25, 0)));
  uint32_t t = 1000;
  e.update(t, reading(100.5F, t));
  for (int i = 0; i < 5; ++i) {
    t += 15000; e.update(t, reading(104, t));
    t += 15000; e.update(t, reading(99.5F, t));
    t += 15000; e.update(t, reading(96, t));
    t += 15000; e.update(t, reading(100.5F, t));
  }
  assert(e.snapshot().state == EngineState::Cooling);
  assert(!e.study().candidate_ready);
  off(e);
  auto bad = reading(80, t + 100);
  bad.fault = ThermocoupleFault::OpenCircuit;
  e.update(t + 100, bad);
  assert(e.snapshot().fault == FaultCode::ProcessProbe);
  assert(!e.study().candidate_ready);
  e.acknowledge();
  assert(!e.start(RecipeId::ValidateA, t + 200, reading(25, t + 200)));
}

void pidAndStorageTests() {
  PidControl pid;
  PidGains gains;
  pid.reset(100);
  float output = 0;
  for (int i = 0; i < 20000; ++i) output = pid.update(gains, 101, 100, 0.1F, 25);
  assert(output > 20); // Integral can supply holding power beyond the old 3.5% ceiling.
  pid.reset(100);
  PidGains p_only; p_only.kp = 1; p_only.ki = 0.000001F; p_only.kd = 100;
  assert(pid.update(p_only, 101, 100, 0.1F, 25) < 1.01F); // No derivative setpoint kick.
  pid.reset(25);
  for (int i = 0; i < 10000; ++i) assert(pid.update(gains, 100, 25, 0.1F, 25) == 25);
  assert(pid.update(gains, 25, 25, 0.1F, 25) < 0.1F); // Saturation did not wind up.
  SavedPid saved;
  saved.checksum = pidChecksum(saved);
  assert(validSavedPid(saved));
  saved.gains.kp += 1;
  assert(!validSavedPid(saved));
  saved.checksum = pidChecksum(saved);
  assert(validSavedPid(saved));
  saved.version = 2; saved.checksum = pidChecksum(saved);
  assert(!validSavedPid(saved));
  saved.version = 1; saved.gains.ki = std::numeric_limits<float>::infinity();
  saved.checksum = pidChecksum(saved);
  assert(!validSavedPid(saved));
}
} // namespace

int main() {
  relayTests(); safetyTests(); pidAndStorageTests(); studyTests(); coolingAndCandidateSafety();
  std::puts("Autotune, three-point study, PID and settings tests passed");
}
