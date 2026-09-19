#include <Arduino.h>
#include <driver/gpio.h>
#include <Preferences.h>
#include <algorithm>

#include "board_pins.h"
#include "max31855.h"
#include "thermal_engine.h"
#include "ui.h"
#include "pid_settings.h"
#include "tune_report_storage.h"
#include "tune_history.h"
#include "validation_runtime.h"
#include "validation_storage.h"
#include "display_settings.h"
#include "display_telemetry.h"

namespace {

constexpr uint32_t kSensorPeriodMs = 100;
constexpr uint32_t kControlPeriodMs = 100;
constexpr uint32_t kUiPeriodMs = 100;

Max31855 g_thermocouple;
ThermalEngine g_engine;
OvenUi g_ui;
ValidationRuntime g_validation;
TuneHistory g_tune_history;
uint32_t g_tune_started_ms = 0;
uint32_t g_last_history_log_ms = 0;
uint8_t g_history_log_index = 0;
bool g_history_tracking = false;
uint32_t g_last_assessment_log_ms = 0;
uint8_t g_assessment_log_index = 0;
uint8_t g_validation_persist_status = 0;
uint32_t g_last_recipe_log_ms = 0;
uint8_t g_recipe_log_phase = 0;
ThermocoupleReading g_process_reading;
bool g_sensor_bus_ready = false;
uint32_t g_last_sensor_ms = 0;
uint32_t g_last_control_ms = 0;
uint32_t g_last_ui_ms = 0;
uint32_t g_last_cpu_temperature_ms = 0;
float g_cpu_temperature = NAN;
uint32_t g_last_log_ms = 0;
uint32_t g_last_tune_log_ms = 0;
uint32_t g_seen_tune_sequence = 0;
uint32_t g_tune_log_losses = 0;
uint32_t g_last_failed_tune_sequence = 0;
uint8_t g_tune_persist_status = 0;  // 0 unknown, 1 verified, 2 failed.
uint8_t g_pending_tune_window = 3;
uint8_t g_pending_tune_window_count = 0;
uint32_t g_pending_tune_sequence = 0;
bool g_tune_persist_attempted = true;

void loadSettings() {
  Preferences storage;
  if (!storage.begin("toaster-pid", true)) return;
  SavedPidV2 current{};
  if (storage.getBytesLength("gains") == sizeof(current) &&
      storage.getBytes("gains", &current, sizeof(current)) == sizeof(current) &&
      validSavedPid(current)) {
    g_engine.loadPid(current.gains, current.checked_scope_mask, current.setup_revision);
  } else {
    SavedPid saved;
    if (storage.getBytesLength("gains") == sizeof(saved) &&
      storage.getBytes("gains", &saved, sizeof(saved)) == sizeof(saved) && validSavedPid(saved)) {
      g_engine.loadPid(saved.gains);  // Legacy gains have no verified temperature scope.
    }
  }
  storage.end();
}

void loadValidationReports() {
  Preferences storage;
  if (!storage.begin("toaster-assess", true)) return;
  for (uint8_t i = 0; i < 4; ++i) {
    char key[8];
    snprintf(key, sizeof(key), "p%u", i);
    SavedValidationReport saved{};
    if (storage.getBytesLength(key) == sizeof(saved) &&
        storage.getBytes(key, &saved, sizeof(saved)) == sizeof(saved) &&
        validSavedValidation(saved) && saved.profile == i)
      g_validation.loadHistorical(i, saved.report);
  }
  storage.end();
}

void persistValidationReport() {
  const auto state = g_engine.snapshot().state;
  if (!g_validation.terminalPending() || g_engine.heaterCommand() ||
      state == EngineState::Running || state == EngineState::Cooling) return;
  g_validation.persisted();  // One bounded attempt; never stall control for retries.
  SavedValidationReport saved{};
  saved.profile = g_validation.latestProfile();
  saved.report = g_validation.views()[saved.profile].report;
  saved.checksum = validationChecksum(saved);
  Preferences storage;
  if (!storage.begin("toaster-assess", false)) { g_validation_persist_status = 2; return; }
  char key[8];
  snprintf(key, sizeof(key), "p%u", saved.profile);
  SavedValidationReport check{};
  const bool verified = storage.putBytes(key, &saved, sizeof(saved)) == sizeof(saved) &&
      storage.getBytes(key, &check, sizeof(check)) == sizeof(check) &&
      validSavedValidation(check) && check.checksum == saved.checksum;
  storage.end();
  g_validation_persist_status = verified ? 1 : 2;
}

void loadTuneReport() {
  Preferences storage;
  if (!storage.begin("toaster-diag", true)) return;
  SavedTuneReport saved{};
  if (storage.getBytesLength("report") == sizeof(saved) &&
      storage.getBytes("report", &saved, sizeof(saved)) == sizeof(saved) &&
      validSavedTuneReport(saved)) {
    g_engine.loadTuneReport(saved.report);
    g_seen_tune_sequence = saved.report.sequence;
  }
  TuneHistory history{};
  if (storage.getBytesLength("history") == sizeof(history) &&
      storage.getBytes("history", &history, sizeof(history)) == sizeof(history) &&
      validTuneHistory(history)) g_tune_history = history;
  storage.end();
}

void updateTuneHistory(uint32_t now_ms) {
  if (!g_history_tracking) return;
  const auto& s = g_engine.snapshot();
  const auto& report = g_engine.study().tune_report;
  g_tune_history.elapsed_ms = now_ms - g_tune_started_ms;
  if (g_tune_history.first_crossing_ms == UINT32_MAX && s.probe_healthy &&
      s.process_celsius >= 100.5F)
    g_tune_history.first_crossing_ms = g_tune_history.elapsed_ms;
  const auto& d = report.latest;
  if (d.cycle > 0 && d.cycle <= 10) {
    g_tune_history.cycles[d.cycle - 1] = d;
    g_tune_history.count = std::max(g_tune_history.count, d.cycle);
  }
  if (s.state == EngineState::Cooling && g_tune_history.accepted_ms == UINT32_MAX)
    g_tune_history.accepted_ms = g_tune_history.elapsed_ms;
  if (report.terminal) {
    g_tune_history.terminal = true;
    g_tune_history.outcome = report.outcome;
    if (report.outcome == TuneOutcome::Complete)
      g_tune_history.cooldown_end_ms = g_tune_history.elapsed_ms;
    g_tune_history.checksum = tuneHistoryChecksum(g_tune_history);
    g_history_tracking = false;
  }
}

void persistTerminalTuneReport() {
  const auto& report = g_engine.study().tune_report;
  if (!report.available || !report.terminal || g_tune_persist_attempted ||
      g_engine.heaterCommand() || g_engine.snapshot().state == EngineState::Running ||
      g_engine.snapshot().state == EngineState::Cooling) return;
  g_tune_persist_attempted = true;  // One bounded attempt per run.
  SavedTuneReport saved{};
  saved.report = report;
  saved.checksum = tuneReportChecksum(saved);
  Preferences storage;
  if (!storage.begin("toaster-diag", false)) { g_tune_persist_status = 2; return; }
  const bool written = storage.putBytes("report", &saved, sizeof(saved)) == sizeof(saved);
  SavedTuneReport check{};
  const bool verified = written &&
      storage.getBytes("report", &check, sizeof(check)) == sizeof(check) &&
      validSavedTuneReport(check) && check.checksum == saved.checksum;
  bool history_verified = false;
  if (g_tune_history.terminal) {
    TuneHistory history_check{};
    history_verified = storage.putBytes("history", &g_tune_history, sizeof(g_tune_history)) ==
        sizeof(g_tune_history) && storage.getBytes("history", &history_check, sizeof(history_check)) ==
        sizeof(history_check) && validTuneHistory(history_check) &&
        history_check.checksum == g_tune_history.checksum;
  }
  storage.end();
  g_tune_persist_status = verified && history_verified ? 1 : 2;
}

void logTuneReport(uint32_t now_ms) {
  const auto& report = g_engine.study().tune_report;
  if (!report.available) return;
  const bool changed = report.sequence != g_seen_tune_sequence;
  if (changed && g_pending_tune_sequence != 0U &&
      g_pending_tune_sequence != report.sequence) {
    if (g_pending_tune_window < g_pending_tune_window_count)
      g_tune_log_losses += g_pending_tune_window_count - g_pending_tune_window;
    g_pending_tune_sequence = 0;
  }
  if (changed && g_last_failed_tune_sequence != 0U &&
      g_last_failed_tune_sequence != report.sequence) {
    ++g_tune_log_losses;
    g_last_failed_tune_sequence = 0;
  }
  if (!changed && g_pending_tune_window >= report.latest.window_count &&
      now_ms - g_last_tune_log_ms < 10000U) return;
  char line[256];
  const auto& d = report.latest;
  int length = 0;
  if (changed || g_pending_tune_window >= d.window_count) {
    length = snprintf(line, sizeof(line),
        "TUNE,%lu,%u,%u,%u,%lu,%u,%.2f,%.2f,%.4f,%.2f,%.2f,%.2f,%.2f,%lu,%.4f,%.4f,%.2f,%lu,%lu,%u,%u\n",
        static_cast<unsigned long>(report.sequence), report.terminal,
        static_cast<unsigned>(report.outcome), report.cycles,
        static_cast<unsigned long>(report.elapsed_seconds), d.cycle, d.period,
        d.heat_seconds, d.fraction, d.minimum, d.maximum, d.amplitude, d.midpoint,
        static_cast<unsigned long>(d.ssr_on_ms), d.period_ratio, d.amplitude_ratio,
        d.midpoint_span, static_cast<unsigned long>(d.failed_checks),
        static_cast<unsigned long>(g_tune_log_losses), g_tune_persist_status, d.window_count);
  } else {
    const auto& w = d.window[g_pending_tune_window];
    length = snprintf(line, sizeof(line),
        "TUNEW,%lu,%u,%u,%.2f,%.2f,%.4f,%.2f,%.2f,%.2f,%.2f,%lu,%lu\n",
        static_cast<unsigned long>(report.sequence), g_pending_tune_window, w.cycle,
        w.period, w.heat_seconds, w.fraction, w.minimum, w.maximum, w.amplitude,
        w.midpoint, static_cast<unsigned long>(w.ssr_on_ms),
        static_cast<unsigned long>(w.failed_checks));
  }
  if (length > 0 && length < static_cast<int>(sizeof(line)) && Serial &&
      Serial.availableForWrite() >= length) {
    Serial.write(reinterpret_cast<const uint8_t*>(line), length);
    if (changed || g_pending_tune_window >= d.window_count) {
      g_seen_tune_sequence = report.sequence;
      if (g_last_failed_tune_sequence == report.sequence) g_last_failed_tune_sequence = 0;
      g_pending_tune_sequence = report.sequence;
      g_pending_tune_window = 0;
      g_pending_tune_window_count = d.window_count;
      g_last_tune_log_ms = now_ms;
    } else {
      ++g_pending_tune_window;
    }
  } else {
    if (changed) g_last_failed_tune_sequence = report.sequence;
  }
}

void saveStudy() {
  // Never touch flash or change gains while a run is active.
  if (!g_engine.canSaveStudy()) return;
  SavedPidV2 saved{};
  saved.gains = g_engine.study().candidate;
  saved.checked_scope_mask = g_engine.candidateCheckedScope();
  saved.setup_revision = g_engine.study().setup_revision;
  saved.checksum = pidChecksum(saved);
  Preferences storage;
  if (!storage.begin("toaster-pid", false)) { g_engine.saveFailed(); return; }
  const bool written = storage.putBytes("gains", &saved, sizeof(saved)) == sizeof(saved);
  SavedPidV2 check{};
  const bool verified = written && storage.getBytes("gains", &check, sizeof(check)) == sizeof(check) &&
      validSavedPid(check) && check.checksum == saved.checksum;
  storage.end();
  if (verified) {
    g_engine.acceptStudy();
    g_validation.refreshConfiguration(g_engine);
  }
  else g_engine.saveFailed();
}

void logTrace(uint32_t now_ms) {
  if (now_ms - g_last_log_ms < 1000U) return;
  g_last_log_ms = now_ms;
  const auto& s = g_engine.snapshot();
  const auto& study = g_engine.study();
  const auto& g = g_engine.runGains();
  const int check_index = s.recipe ? controlCheckIndex(s.recipe->id) : -1;
  const StudyRecord r = check_index >= 0 ? study.checks[check_index] : StudyRecord{};
  // TRACE's historical point field is retired. CHECK carries temperature identity.
  const int point = -1;
  char line[320];
  const int length = snprintf(line, sizeof(line),
      "TRACE,%lu,%lu,%d,%u,%u,%.2f,%.2f,%.2f,%u,%.4f,%.6f,%.3f,%d,%lu,%.2f,%lu,%.3f,%.2f,%u,%u,%u,%.2f,%.4f,%.6f,%.3f,%.2f,%.2f,%u\n",
      static_cast<unsigned long>(now_ms), static_cast<unsigned long>(s.run_elapsed_seconds),
      s.recipe ? static_cast<int>(s.recipe->id) : -1, static_cast<unsigned>(s.state), s.phase_index,
      s.process_celsius, s.target_celsius, s.output_percent, s.heater_commanded_on,
      g.kp, g.ki, g.kd, point, static_cast<unsigned long>(r.attempts), r.peak_celsius,
      static_cast<unsigned long>(r.rise_seconds), r.hold_rmse, r.hold_output_percent,
      static_cast<unsigned>(s.fault), study.cycles, s.probe_healthy, r.start_celsius,
      study.candidate.kp, study.candidate.ki, study.candidate.kd,
      study.period_seconds, study.amplitude_celsius, study.candidate_ready);
  // Drop a sample instead of blocking heater control on a disconnected/slow host.
  if (length > 0 && length < static_cast<int>(sizeof(line)) && Serial &&
      Serial.availableForWrite() >= length) Serial.write(reinterpret_cast<const uint8_t*>(line), length);
}

bool logBounded(const char* line, int length, size_t capacity) {
  if (length <= 0 || static_cast<size_t>(length) >= capacity || !Serial ||
      Serial.availableForWrite() < length) return false;
  Serial.write(reinterpret_cast<const uint8_t*>(line), length);
  return true;
}

void logHistory(uint32_t now_ms) {
  if (!g_tune_history.count && !g_tune_history.terminal && !g_history_tracking) return;
  if (now_ms - g_last_history_log_ms < 1000U) return;
  char line[256];
  int length;
  if (g_history_log_index == 0) {
    length = snprintf(line, sizeof(line), "TUNESTAGE,1,%u,%u,%lu,%lu,%lu,%lu,%lu,%u\n",
        g_tune_history.terminal, static_cast<unsigned>(g_tune_history.outcome),
        static_cast<unsigned long>(g_tune_history.elapsed_ms),
        static_cast<unsigned long>(g_tune_history.first_crossing_ms),
        static_cast<unsigned long>(g_tune_history.accepted_ms),
        static_cast<unsigned long>(g_tune_history.cooldown_end_ms),
        static_cast<unsigned long>(g_tune_history.deadline_ms), g_tune_history.count);
  } else {
    const auto& d = g_tune_history.cycles[g_history_log_index - 1];
    length = snprintf(line, sizeof(line),
        "TUNEH,1,%u,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%lu,%lu,%.9g,%.9g,%.9g\n",
        d.cycle, d.period, d.heat_seconds, d.fraction, d.minimum, d.maximum,
        d.amplitude, d.midpoint, static_cast<unsigned long>(d.ssr_on_ms),
        static_cast<unsigned long>(d.failed_checks), d.period_ratio,
        d.amplitude_ratio, d.midpoint_span);
  }
  if (logBounded(line, length, sizeof(line))) {
    g_history_log_index = (g_history_log_index + 1U) % (g_tune_history.count + 1U);
    g_last_history_log_ms = now_ms;
  }
}

void logAssessment(uint32_t now_ms) {
  if (now_ms - g_last_assessment_log_ms < 250U) return;
  char line[256];
  int length;
  const uint8_t slot = g_assessment_log_index;
  const auto& study = g_engine.study();
  if (slot < 3) {
    const auto& c = study.checks[slot];
    length = snprintf(line, sizeof(line),
        "CHECK,1,%u,%u,%u,%lu,%lu,%lu,%.9g,%.9g,%lu,%.9g,%.9g,%u,%u,%u\n",
        slot, 100 + slot * 50, static_cast<unsigned>(c.result),
        static_cast<unsigned long>(c.attempts), static_cast<unsigned long>(c.setup_revision),
        static_cast<unsigned long>(c.candidate_revision), c.start_celsius, c.peak_celsius,
        static_cast<unsigned long>(c.rise_seconds), c.hold_rmse, c.hold_output_percent,
        study.required_checks_mask, study.approved_checks_mask, study.checked_scope_mask);
  } else {
    const uint8_t profile = (slot - 3U) / 3U;
    const uint8_t record = (slot - 3U) % 3U;
    const auto& v = g_validation.views()[profile];
    const auto& r = v.report;
    const auto& m = r.metrics;
    if (record == 0) {
      length = snprintf(line, sizeof(line),
          "VALIDATION,1,%u,%u,%u,%u,%u,%u,%u,%u,%lu,%lu,%lu,%lu,%lu,%u\n",
          profile, static_cast<unsigned>(r.status), r.run_completed, r.evidence_complete,
          v.consecutive_passes, v.eligible, v.commissioned, v.needs_revalidation,
          static_cast<unsigned long>(r.identity.setup_revision),
          static_cast<unsigned long>(r.identity.gain_revision),
          static_cast<unsigned long>(r.identity.recipe_revision),
          static_cast<unsigned long>(r.identity.criteria_revision),
          static_cast<unsigned long>(r.identity.run_id), g_validation_persist_status);
    } else if (record == 1) {
      length = snprintf(line, sizeof(line),
          "METRIC,1,%u,PROFILE,%u,%.9g,%u,%.9g,%u,%.9g,%lu,%lu,%lu,%lu,%lu,%lu\n",
          profile, m.peak_measured, m.peak_celsius, m.heating_slope_measured,
          m.heating_slope_celsius_per_second, m.cooling_slope_measured,
          m.cooling_slope_celsius_per_second,
          static_cast<unsigned long>(m.soak_accumulated_ms),
          static_cast<unsigned long>(m.soak_longest_continuous_ms),
          static_cast<unsigned long>(m.liquidus_accumulated_ms),
          static_cast<unsigned long>(m.liquidus_longest_continuous_ms),
          static_cast<unsigned long>(m.sample_count), static_cast<unsigned long>(m.maximum_gap_ms));
    } else {
      length = snprintf(line, sizeof(line),
          "METRIC,1,%u,HOLD,%u,%.9g,%.9g,%.9g,%.9g,%.9g,%lu,%lu,%u,%lu,%u,%lu\n",
          profile, m.hold_error_measured, m.hold_mean_error_celsius, m.hold_rms_error_celsius,
          m.hold_max_error_celsius, m.hold_drift_celsius_per_second, m.hold_mean_demand_percent,
          static_cast<unsigned long>(m.hold_elapsed_ms),
          static_cast<unsigned long>(m.hold_longest_in_band_ms), m.warmup_measured,
          static_cast<unsigned long>(m.warmup_ms), m.settling_measured,
          static_cast<unsigned long>(m.settling_ms));
    }
  }
  if (logBounded(line, length, sizeof(line))) {
    g_assessment_log_index = (slot + 1U) % 15U;
    g_last_assessment_log_ms = now_ms;
  }
}

void logRecipe(uint32_t now_ms) {
  const auto* recipe = g_engine.snapshot().recipe;
  if (!recipe || !recipe->phase_count || now_ms - g_last_recipe_log_ms < 1000U) return;
  g_recipe_log_phase %= recipe->phase_count;
  const auto& phase = recipe->phases[g_recipe_log_phase];
  char line[200];
  const int length = snprintf(line, sizeof(line),
      "RUNMETA,2,%u,%u,%u,%.9g,%.9g,%lu,%lu,%.9g,%.9g\n",
      static_cast<unsigned>(recipe->id), g_recipe_log_phase, static_cast<unsigned>(phase.kind),
      phase.target_celsius, phase.rate_celsius_per_second,
      static_cast<unsigned long>(phase.duration_seconds),
      static_cast<unsigned long>(recipe->maximum_run_seconds), recipe->maximum_process_celsius,
      recipe->maximum_output_percent);
  if (logBounded(line, length, sizeof(line))) {
    ++g_recipe_log_phase;
    g_last_recipe_log_ms = now_ms;
  }
}

void setHeaterGate(bool enabled) {
  // Sole firmware path to the SSR input. Hardware reset-off must be bench-verified.
  gpio_set_level(board::kSsrGate, enabled ? 1 : 0);
}

void processUiCommand(uint32_t now_ms) {
  switch (g_ui.consumeCommand()) {
    case UiCommand::StartSelected:
      if (g_engine.start(g_ui.selectedRecipe(), now_ms, g_process_reading)) {
        g_validation.start(g_engine, now_ms);
        if (static_cast<uint8_t>(g_ui.selectedRecipe()) < 4U)
          g_validation_persist_status = 0;
        if (g_ui.selectedRecipe() == RecipeId::Autotune100) {
          g_tune_history = {};
          g_tune_started_ms = now_ms;
          g_history_tracking = true;
          g_history_log_index = 0;
          g_tune_persist_attempted = false;
          g_tune_persist_status = 0;
        }
      }
      break;
    case UiCommand::Stop:
      g_engine.abort();
      setHeaterGate(false);
      break;
    case UiCommand::StartCustomAnneal:
      if (g_engine.startAnneal(g_ui.annealProgram(), now_ms, g_process_reading)) {
        g_validation.start(g_engine, now_ms);
        g_validation_persist_status = 0;
      }
      break;
    case UiCommand::Acknowledge:
      g_engine.acknowledge();
      break;
    case UiCommand::SaveStudy:
      setHeaterGate(false);
      saveStudy();
      break;
    case UiCommand::CommissionProfile:
      g_validation.commission(g_ui.selectedRecipe() == RecipeId::CustomAnneal
          ? 2U : static_cast<uint8_t>(g_ui.selectedRecipe()), g_engine);
      break;
    default:
      break;
  }
}

}  // namespace

void setup() {
  // Gate low before display, logging, I2C, or any potentially slow startup work.
  gpio_set_level(board::kSsrGate, 0);
  gpio_set_direction(board::kSsrGate, GPIO_MODE_OUTPUT);
  setHeaterGate(false);

  Serial.begin(115200);
  delay(100);
  Serial.println("Toaster thermal controller booting");
  g_engine.configureControlChecks(1U, 1U, 1U);  // Fixed central PCB, initial 100 C scope.
  loadSettings();
  loadTuneReport();
  loadValidationReports();

  g_sensor_bus_ready = g_thermocouple.begin();
  if (!g_sensor_bus_ready) {
    Serial.println("MAX31855 SPI3 initialization failed; heater remains locked off");
  }
  Preferences display_storage;
  if (!g_ui.begin(loadDisplayBrightness(display_storage))) {
    Serial.println("FT6336 touch initialization failed; display remains read-only");
  }
}

void loop() {
  const uint32_t now_ms = millis();

  if (g_sensor_bus_ready && now_ms - g_last_sensor_ms >= kSensorPeriodMs) {
    g_process_reading = g_thermocouple.readProcess();
    g_last_sensor_ms = now_ms;
  }

  processUiCommand(now_ms);

  if (now_ms - g_last_control_ms >= kControlPeriodMs) {
    g_engine.update(now_ms, g_process_reading);
    setHeaterGate(g_engine.heaterCommand());
    g_validation.update(g_engine, now_ms);
    updateTuneHistory(now_ms);
    const auto& tune_report = g_engine.study().tune_report;
    if (tune_report.available && !tune_report.terminal &&
        tune_report.outcome == TuneOutcome::Running) {
      g_tune_persist_attempted = false;
      g_tune_persist_status = 0;
    }
    persistTerminalTuneReport();
    persistValidationReport();
    g_last_control_ms = now_ms;
  }

  if (now_ms - g_last_ui_ms >= kUiPeriodMs) {
    // Diagnostic only. No CPU-sensor setup/read work during active heat control.
    if (g_engine.snapshot().state == EngineState::Idle &&
        !g_engine.heaterCommand() && now_ms - g_last_cpu_temperature_ms >= 1000U) {
      g_cpu_temperature = temperatureRead();
      g_last_cpu_temperature_ms = millis();
    }
    // The loop timestamp predates sensor reads and may be earlier than sample_ms.
    const uint32_t telemetry_now_ms = millis();
    const bool cpu_fresh = telemetry_now_ms - g_last_cpu_temperature_ms <= 2000U;
    g_ui.setElectronicsTemperatures(maxBoardTemperature(g_process_reading, telemetry_now_ms),
                                    cpu_fresh ? g_cpu_temperature : NAN);
    g_ui.setProfileValidation(g_validation.views());
    g_ui.update(g_engine.snapshot(), g_engine.study());
    g_last_ui_ms = now_ms;
  }
  g_ui.tick(now_ms);
  // Flash writes only while idle and output-off, after a released gesture.
  if (g_engine.snapshot().state == EngineState::Idle && !g_engine.heaterCommand() &&
      g_ui.consumeBrightnessChange()) {
    Preferences display_storage;
    if (!saveDisplayBrightness(display_storage, g_ui.brightnessPercent()))
      Serial.println("Display brightness save failed; current level remains until reboot");
  }
  logTrace(now_ms);
  logTuneReport(now_ms);
  logHistory(now_ms);
  logAssessment(now_ms);
  logRecipe(now_ms);
  delay(1);
}
