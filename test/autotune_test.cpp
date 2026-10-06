#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

#include "thermal_engine.h"
#include "pid_settings.h"
#include "tune_report_storage.h"

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

void diagnosticCycle(RelayAutotune& tune, uint32_t& t, uint32_t period_ms,
                     uint32_t heat_ms, float minimum, float maximum) {
  const uint32_t off_ms = period_ms - heat_ms;
  t += off_ms; tune.update(t, 99.5F, false);
  t += heat_ms / 2; tune.update(t, minimum, true);
  t += heat_ms - heat_ms / 2; tune.update(t, maximum, true);
}

uint32_t diagnosticMask(uint32_t period_ms, uint32_t heat_ms, float minimum, float maximum,
                        uint32_t period2_ms = 0, float minimum2 = NAN, float maximum2 = NAN) {
  RelayAutotune tune;
  tune.begin(0, 25);
  uint32_t t = 1000;
  tune.update(t, 100.5F);
  diagnosticCycle(tune, t, period_ms, heat_ms, minimum, maximum);
  diagnosticCycle(tune, t, period_ms, heat_ms, minimum, maximum);
  diagnosticCycle(tune, t, period_ms, heat_ms, minimum, maximum);
  diagnosticCycle(tune, t, period2_ms ? period2_ms : period_ms,
                  period2_ms ? period2_ms / 2 : heat_ms,
                  std::isnan(minimum2) ? minimum : minimum2,
                  std::isnan(maximum2) ? maximum : maximum2);
  diagnosticCycle(tune, t, period_ms, heat_ms, minimum, maximum);
  return tune.diagnostics().failed_checks;
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
    for (int i = 0; i < 5; ++i)
      cycle(tune, t, kind == 0 ? 0.5F : kind == 1 ? (i % 2 ? 6.0F : 3.0F) : 4.0F,
            kind == 2 ? 5000U : 30000U);
    assert(tune.ready() && !tune.failed());
    const uint32_t expected = kind == 0 ? TuneAmplitudeLow :
                              kind == 1 ? TuneAmplitudeSpread : TunePeriodLow;
    assert(tune.diagnostics().failed_checks & expected);
    assert(tuneBlockingFailures(tune.diagnostics().failed_checks) == 0U);
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
  for (int i = 0; i < 5; ++i) {
    asymmetric.update(t + 30000, 104);
    asymmetric.update(t + 50000, 99.5F);
    asymmetric.update(t + 55000, 96);
    asymmetric.update(t + 60000, 100.5F);
    t += 60000;
  }
  assert(asymmetric.ready() && !asymmetric.failed());
  assert(asymmetric.diagnostics().failed_checks & TuneFractionLow);
}

void diagnosticTests() {
  assert((kTuneQualityChecks & TuneAmplitudeLow) != 0U);
  assert((kTuneQualityChecks & TuneMidpointOffset) != 0U);
  assert((kTuneBlockingChecks & TuneInvalidMath) != 0U);
  assert(tuneBlockingFailures(kTuneQualityChecks) == 0U);
  assert(tuneBlockingFailures(TuneNeedWindow | TuneInvalidGains) ==
         (TuneNeedWindow | TuneInvalidGains));
  assert(diagnosticMask(39000, 19500, 96, 104) & TunePeriodLow);
  assert(diagnosticMask(301000, 150500, 96, 104) & TunePeriodHigh);
  assert(diagnosticMask(60000, 30000, 98.1F, 101.9F) & TuneAmplitudeLow);
  assert(diagnosticMask(60000, 30000, 87.9F, 112.1F) & TuneAmplitudeHigh);
  assert(diagnosticMask(100000, 29000, 96, 104) & TuneFractionLow);
  assert(diagnosticMask(100000, 71000, 96, 104) & TuneFractionHigh);
  assert(diagnosticMask(60000, 30000, 101, 109) & TuneMidpointOffset);
  PidGains invalid_gains;
  invalid_gains.kp = NAN;
  assert(RelayAutotune::candidateFailure(invalid_gains) == TuneInvalidGains);
  assert(RelayAutotune::candidateFailure(PidGains{}) == 0U);
  assert(diagnosticMask(40000, 20000, 96, 104, 49000, 96, 104) & TunePeriodSpread);
  assert(diagnosticMask(60000, 30000, 96, 104, 60000, 95, 105) & TuneAmplitudeSpread);
  assert(diagnosticMask(60000, 30000, 96, 104, 60000, 97.1F, 105.1F) & TuneMidpointSpread);
  // Inclusive individual limits remain accepted; spread equality is accepted.
  assert((diagnosticMask(40000, 12000, 98, 102) &
          (TunePeriodLow | TuneAmplitudeLow | TuneFractionLow)) == 0);
  assert((diagnosticMask(300000, 210000, 88, 112) &
          (TunePeriodHigh | TuneAmplitudeHigh | TuneFractionHigh)) == 0);
  assert((diagnosticMask(60000, 30000, 84, 108) & TuneMidpointOffset) == 0);
  assert((diagnosticMask(60000, 30000, 99.5F, 108.5F) & TuneMidpointOffset) == 0);
  assert((diagnosticMask(60000, 30000, 95, 105, 60000, 94, 106) &
          TuneAmplitudeSpread) == 0);
  assert((diagnosticMask(60000, 30000, 96, 104, 72000, 97, 105) &
          (TuneMidpointOffset | TunePeriodSpread | TuneMidpointSpread)) == 0);

  RelayAutotune tune;
  tune.begin(0, 25);
  uint32_t t = 1000;
  tune.update(t, 100.5F);
  diagnosticCycle(tune, t, 60000, 30000, 96, 104);
  assert(tune.diagnostics().failed_checks == TuneSettling);
  assert(tune.diagnostics().ssr_on_ms == 30000U);
  diagnosticCycle(tune, t, 60000, 30000, 96, 104);
  diagnosticCycle(tune, t, 60000, 30000, 96, 104);
  assert(tune.diagnostics().failed_checks == TuneNeedWindow);
}

TuneCycleSample sample(uint8_t cycle, float period, float heat,
                       float minimum, float maximum) {
  TuneCycleSample result;
  result.cycle = cycle;
  result.period = period;
  result.heat_seconds = heat;
  result.fraction = heat / period;
  result.minimum = minimum;
  result.maximum = maximum;
  result.amplitude = (maximum - minimum) * 0.5F;
  result.midpoint = (maximum + minimum) * 0.5F;
  return result;
}

void candidateEstimateTests() {
  TuneCycleDiagnostics first;
  first.window_count = 3;
  first.window[0] = sample(3, 95.9000015F, 28.7999992F, 99.0F, 101.75F);
  first.window[1] = sample(4, 89.8000031F, 28.3999996F, 99.0F, 101.5F);
  first.window[2] = sample(5, 96.0F, 32.7000008F, 98.75F, 101.5F);
  PidGains gains;
  assert(RelayAutotune::estimateCandidate(first, gains));
  assert(std::fabs(gains.kp - 4.5749172F) < 0.0001F);
  assert(std::fabs(gains.ki - 0.02214598F) < 0.000001F);
  assert(std::fabs(gains.kd - 68.18805F) < 0.001F);

  TuneCycleDiagnostics later;
  later.window_count = 3;
  later.window[0] = sample(7, 102.699997F, 32.9000015F, 98.75F, 101.75F);
  later.window[1] = sample(8, 94.0F, 27.7000008F, 99.0F, 101.5F);
  later.window[2] = sample(9, 94.3000031F, 27.3999996F, 99.25F, 101.75F);
  assert(RelayAutotune::estimateCandidate(later, gains));
  assert(std::fabs(gains.kp - 4.4262749F) < 0.0001F);
  assert(std::fabs(gains.ki - 0.02074168F) < 0.000001F);
  assert(std::fabs(gains.kd - 68.15058F) < 0.001F);

  TuneCycleDiagnostics zero = later;
  zero.window[1] = sample(8, 129, 72, 100.0F, 100.0F);
  assert(!RelayAutotune::estimateCandidate(zero, gains));
  TuneCycleDiagnostics nonfinite = later;
  nonfinite.window[2].period = std::numeric_limits<float>::infinity();
  assert(!RelayAutotune::estimateCandidate(nonfinite, gains));
  TuneCycleDiagnostics incomplete = later;
  incomplete.window_count = 2;
  assert(!RelayAutotune::estimateCandidate(incomplete, gains));
}

void reportStorageTests() {
  ThermalEngine e;
  assert(e.start(RecipeId::Autotune100, 0, reading(25, 0)));
  e.update(1800000U, reading(25, 1800000U));
  const auto report = e.study().tune_report;
  assert(report.available && report.terminal && report.outcome == TuneOutcome::Timeout);
  SavedTuneReport saved{};
  saved.report = report;
  saved.checksum = tuneReportChecksum(saved);
  assert(validSavedTuneReport(saved));
  ThermalEngine restored;
  assert(restored.loadTuneReport(saved.report));
  assert(restored.study().tune_report.outcome == TuneOutcome::Timeout);
  ++saved.checksum;
  assert(!validSavedTuneReport(saved));
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
    e.update(start + 1800000U, reading(25, start + 1800000U));
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
  assert(!e.start(RecipeId::Check100, 0, reading(25, 0)));
  assert(e.snapshot().fault == FaultCode::NoTuneCandidate);
  e.acknowledge();
  assert(!e.start(RecipeId::Check150, 0, reading(25, 0)));
  assert(e.snapshot().fault == FaultCode::CheckNotApproved);
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

uint32_t simulateTrackingCheck(ThermalEngine& e, RecipeId id, uint32_t start) {
  const float cap = recipeFor(id).maximum_output_percent;
  assert(e.start(id, start, reading(25.0F, start)));
  for (uint32_t elapsed = 100; elapsed <= 1200000U; elapsed += 100) {
    const float target = e.snapshot().target_celsius;
    const float process = target > 28.0F ? target - 3.0F : 25.0F;
    e.update(start + elapsed, reading(process, start + elapsed));
    assert(e.snapshot().output_percent <= cap);
    if (e.snapshot().state == EngineState::Cooling) {
      e.update(start + elapsed + 100U, reading(60.0F, start + elapsed + 100U));
      assert(e.snapshot().state == EngineState::Complete);
      off(e);
      return start + elapsed + 100U;
    }
    assert(e.snapshot().state == EngineState::Running);
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
  assert(!e.start(RecipeId::Check150, t + 1000, reading(25, t + 1000)));
  assert(e.snapshot().fault == FaultCode::CheckNotApproved);
  e.acknowledge();

  t = simulateTrackingCheck(e, RecipeId::Check100, t + 2000);
  assert(e.runGains().kp == candidate.kp && e.runGains().ki == candidate.ki);
  const auto& initial_result = e.study().checks[0];
  assert(initial_result.result == StudyResult::Complete && initial_result.attempts == 1);
  assert(initial_result.reached_band && initial_result.rise_seconds > 0);
  assert(initial_result.peak_celsius < 120 && initial_result.peak_celsius >= 96);
  assert(std::isfinite(initial_result.hold_rmse) && initial_result.hold_output_percent <= 25);
  assert(!e.canSaveStudy());
  e.acknowledge();
  assert(e.canSaveStudy());
  assert(e.candidateCheckedScope() == kControlCheck100Mask);
  assert(e.acceptStudy());
  assert(e.study().checked_scope_mask == kControlCheck100Mask);
  assert(e.study().checked_setup_revision == 1U);
  assert(e.study().active.kp == candidate.kp);

  // Expanding a plan clears run metrics, not matching saved scope.
  assert(e.configureControlChecks(kAllControlChecksMask, kAllControlChecksMask, 1));
  assert(e.study().candidate_ready);
  assert(e.study().checks[0].result == StudyResult::Empty);
  assert(e.candidateCheckedScope() == kControlCheck100Mask);
  for (auto id : {RecipeId::Check100, RecipeId::Check150, RecipeId::Check200}) {
    t = simulateTrackingCheck(e, id, t + 1000);
    assert(e.runGains().kp == candidate.kp && e.runGains().ki == candidate.ki);
    const auto& result = e.study().checks[controlCheckIndex(id)];
    assert(result.result == StudyResult::Complete && result.attempts == 1);
    assert(result.reached_band && result.rise_seconds > 0);
    const float target = controlCheckTargetCelsius(controlCheckIndex(id));
    assert(result.peak_celsius < target + 20 && result.peak_celsius >= target - 4);
    assert(std::isfinite(result.hold_rmse) &&
           result.hold_output_percent <= recipeFor(id).maximum_output_percent);
    assert(!e.canSaveStudy()); // Results cannot be accepted before acknowledgement.
    e.acknowledge();
  }
  assert(e.canSaveStudy());
  assert(e.candidateCheckedScope() == kAllControlChecksMask);
  // Repeating 150 C increments its attempt and a failure blocks saving.
  assert(e.start(RecipeId::Check150, t + 1000, reading(25, t + 1000)));
  assert(e.study().checks[1].attempts == 2);
  e.abort(); e.acknowledge();
  assert(e.study().checks[1].result == StudyResult::Aborted && !e.canSaveStudy());
  t = simulateTrackingCheck(e, RecipeId::Check150, t + 2000);
  e.acknowledge();
  assert(e.canSaveStudy());
  assert(e.acceptStudy());
  assert(e.study().active.kp == candidate.kp);
  assert(e.study().checked_scope_mask == kAllControlChecksMask);
  assert(e.study().saved);
  assert(!e.start(RecipeId::Sac305Reflow, t, reading(25, t)));
  assert(e.snapshot().fault == FaultCode::HighTemperatureProfileNotCommissioned);
  e.acknowledge();
  assert(e.configureControlChecks(kControlCheck100Mask, kControlCheck100Mask, 2U));
  assert(!e.study().candidate_ready);
  assert(e.study().checks[0].result == StudyResult::Empty);
  assert(e.study().checked_scope_mask == kAllControlChecksMask);
  assert(e.start(RecipeId::Autotune100, t, reading(25, t)));
  assert(!e.study().candidate_ready && e.study().checks[0].result == StudyResult::Empty);
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
  assert(!e.start(RecipeId::Check100, t + 200, reading(25, t + 200)));
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

  SavedPid legacy;
  legacy.checksum = pidChecksum(legacy);
  SavedPidV2 migrated;
  assert(migrateSavedPid(legacy, migrated));
  assert(validSavedPid(migrated));
  assert(migrated.checked_scope_mask == 0U && migrated.setup_revision == 0U);
  migrated.checked_scope_mask = kControlCheck100Mask;
  migrated.checksum = pidChecksum(migrated);
  assert(!validSavedPid(migrated));
  migrated.setup_revision = 42U;
  migrated.checksum = pidChecksum(migrated);
  assert(validSavedPid(migrated));
  ++migrated.setup_revision;
  assert(!validSavedPid(migrated));

  ThermalEngine restored;
  assert(restored.loadPid(legacy.gains));
  assert(restored.study().checked_scope_mask == 0U);
  assert(!restored.loadPid(legacy.gains, kControlCheck100Mask, 0U));
  assert(restored.loadPid(legacy.gains, kControlCheck100Mask, 42U));
  assert(restored.study().checked_scope_mask == kControlCheck100Mask);
  assert(restored.study().checked_setup_revision == 42U);
  assert(!restored.loadPid(legacy.gains, 0x80U, 42U));
}
void retainedScopeTests() {
  SavedPidV2 saved;
  saved.checked_scope_mask = kControlCheck100Mask;
  saved.setup_revision = 1U;
  saved.checksum = pidChecksum(saved);
  assert(validSavedPid(saved));

  ThermalEngine restored;
  assert(restored.configureControlChecks(kControlCheck100Mask,
      kControlCheck100Mask | kControlCheck150Mask, 1U));
  assert(restored.loadPid(saved.gains, saved.checked_scope_mask, saved.setup_revision));
  assert(restored.study().candidate_ready && restored.study().saved);
  assert(restored.study().checks[0].result == StudyResult::Empty);
  assert(restored.candidateCheckedScope() == kControlCheck100Mask);
  assert(restored.canSaveStudy());
  off(restored);

  // Identical gains can extend their scope after reboot without repeating 100 C.
  const uint32_t t = simulateTrackingCheck(restored, RecipeId::Check150, 1000U);
  assert(restored.runGains().kp == saved.gains.kp);
  assert(restored.runGains().ki == saved.gains.ki);
  assert(restored.runGains().kd == saved.gains.kd);
  assert(!restored.study().saved && !restored.canSaveStudy());
  restored.acknowledge();
  const uint8_t extended = kControlCheck100Mask | kControlCheck150Mask;
  assert(restored.candidateCheckedScope() == extended);
  assert(restored.canSaveStudy() && restored.acceptStudy());
  assert(restored.study().checked_scope_mask == extended);
  assert(restored.study().checks[0].result == StudyResult::Empty);
  off(restored);

  saved.gains = restored.study().active;
  saved.checked_scope_mask = restored.study().checked_scope_mask;
  saved.setup_revision = restored.study().checked_setup_revision;
  saved.checksum = pidChecksum(saved);
  assert(validSavedPid(saved));
  ThermalEngine second_boot;
  assert(second_boot.configureControlChecks(kControlCheck100Mask, extended, 1U));
  assert(second_boot.loadPid(saved.gains, saved.checked_scope_mask, saved.setup_revision));
  assert(second_boot.candidateCheckedScope() == extended);
  assert(second_boot.study().checks[1].result == StudyResult::Empty);
  assert(second_boot.canSaveStudy());
  assert(!second_boot.start(RecipeId::Check200, t, reading(25, t)));
  assert(second_boot.snapshot().fault == FaultCode::CheckNotApproved);
  off(second_boot);
  second_boot.acknowledge();

  // The approved 200 C check extends the same persisted 100/150 C gains.
  ThermalEngine higher;
  assert(higher.configureControlChecks(kControlCheck100Mask, kAllControlChecksMask, 1U));
  assert(higher.loadPid(saved.gains, saved.checked_scope_mask, saved.setup_revision));
  assert(higher.candidateCheckedScope() == extended);
  simulateTrackingCheck(higher, RecipeId::Check200, 1000U);
  assert(higher.runGains().kp == saved.gains.kp && higher.runGains().ki == saved.gains.ki &&
      higher.runGains().kd == saved.gains.kd);
  assert(!higher.canSaveStudy());
  higher.acknowledge();
  assert(higher.canSaveStudy() && higher.acceptStudy());
  assert(higher.study().checked_scope_mask == kAllControlChecksMask);
  assert(higher.study().checks[0].result == StudyResult::Empty);
  assert(higher.study().checks[1].result == StudyResult::Empty);
  off(higher);

  // New failed evidence overrides the saved bit until that check succeeds again.
  assert(second_boot.start(RecipeId::Check100, t, reading(25, t)));
  assert(!second_boot.study().saved);
  second_boot.abort(); second_boot.acknowledge();
  assert(second_boot.candidateCheckedScope() == kControlCheck150Mask);
  assert(!second_boot.canSaveStudy());

  PidStudy matching = restored.study();
  for (auto& check : matching.checks) check = {};
  assert(pidCandidateCheckedScope(matching) == extended);
  for (unsigned i = 0; i < 3; ++i) {
    PidStudy changed = matching;
    float& gain = i == 0 ? changed.candidate.kp : i == 1 ? changed.candidate.ki : changed.candidate.kd;
    gain = std::nextafter(gain, std::numeric_limits<float>::infinity());
    assert(pidCandidateCheckedScope(changed) == 0U);
    assert(!pidStudyChecksSatisfied(changed));
  }
  ++matching.setup_revision;
  assert(pidCandidateCheckedScope(matching) == 0U);
  assert(!pidStudyChecksSatisfied(matching));
  ThermalEngine changed_setup;
  assert(changed_setup.configureControlChecks(kControlCheck100Mask, extended, 2U));
  assert(changed_setup.loadPid(saved.gains, saved.checked_scope_mask, 1U));
  assert(!changed_setup.study().candidate_ready && !changed_setup.canSaveStudy());
  off(changed_setup);
  ThermalEngine legacy;
  assert(legacy.loadPid(saved.gains));
  assert(!legacy.study().candidate_ready && !legacy.canSaveStudy());
  off(legacy);

  // Accepting new gains must not make them inherit the old active PID's scope.
  ThermalEngine retuned;
  assert(retuned.configureControlChecks(kControlCheck100Mask, extended, 1U));
  assert(retuned.loadPid(saved.gains, saved.checked_scope_mask, saved.setup_revision));
  uint32_t retune_t = simulate(retuned, RecipeId::Autotune100, 1000U);
  assert(retuned.study().candidate.kp != retuned.study().active.kp);
  retuned.acknowledge();
  assert(retuned.candidateCheckedScope() == 0U);
  retune_t = simulateTrackingCheck(retuned, RecipeId::Check100, retune_t + 1000U);
  retuned.acknowledge();
  assert(retuned.candidateCheckedScope() == kControlCheck100Mask);
  assert(retuned.acceptStudy());
  assert(retuned.study().checked_scope_mask == kControlCheck100Mask);
  (void)retune_t;
}
} // namespace

int main() {
  relayTests(); diagnosticTests(); candidateEstimateTests();
  reportStorageTests(); safetyTests(); pidAndStorageTests();
  studyTests(); retainedScopeTests(); coolingAndCandidateSafety();
  std::puts("Autotune, fixed-probe checks, PID and settings tests passed");
}
