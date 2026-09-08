#include <Arduino.h>
#include <driver/gpio.h>
#include <Preferences.h>

#include "board_pins.h"
#include "max31855.h"
#include "thermal_engine.h"
#include "ui.h"
#include "pid_settings.h"

namespace {

constexpr uint32_t kSensorPeriodMs = 100;
constexpr uint32_t kControlPeriodMs = 100;
constexpr uint32_t kUiPeriodMs = 100;

Max31855 g_thermocouple;
ThermalEngine g_engine;
OvenUi g_ui;
ThermocoupleReading g_process_reading;
bool g_sensor_bus_ready = false;
uint32_t g_last_sensor_ms = 0;
uint32_t g_last_control_ms = 0;
uint32_t g_last_ui_ms = 0;
uint32_t g_last_log_ms = 0;

void loadSettings() {
  Preferences storage;
  if (!storage.begin("toaster-pid", true)) return;
  SavedPid saved;
  if (storage.getBytesLength("gains") == sizeof(saved) &&
      storage.getBytes("gains", &saved, sizeof(saved)) == sizeof(saved) && validSavedPid(saved)) {
    g_engine.loadPid(saved.gains);
  }
  storage.end();
}

void saveStudy() {
  // Never touch flash or change gains while a run is active.
  if (!g_engine.canSaveStudy()) return;
  SavedPid saved;
  saved.gains = g_engine.study().candidate;
  saved.checksum = pidChecksum(saved);
  Preferences storage;
  if (!storage.begin("toaster-pid", false)) { g_engine.saveFailed(); return; }
  const bool written = storage.putBytes("gains", &saved, sizeof(saved)) == sizeof(saved);
  SavedPid check;
  const bool verified = written && storage.getBytes("gains", &check, sizeof(check)) == sizeof(check) &&
      validSavedPid(check) && check.checksum == saved.checksum;
  storage.end();
  if (verified) g_engine.acceptStudy();
  else g_engine.saveFailed();
}

void logTrace(uint32_t now_ms) {
  if (now_ms - g_last_log_ms < 1000U) return;
  g_last_log_ms = now_ms;
  const auto& s = g_engine.snapshot();
  const auto& study = g_engine.study();
  const auto& g = g_engine.runGains();
  const int point = s.recipe ? studyPoint(s.recipe->id) : -1;
  const StudyRecord r = point >= 0 ? study.points[point] : StudyRecord{};
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

void setHeaterGate(bool enabled) {
  // Sole firmware path to the SSR input. Hardware reset-off must be bench-verified.
  gpio_set_level(board::kSsrGate, enabled ? 1 : 0);
}

void processUiCommand(uint32_t now_ms) {
  switch (g_ui.consumeCommand()) {
    case UiCommand::StartSelected:
      g_engine.start(g_ui.selectedRecipe(), now_ms, g_process_reading);
      break;
    case UiCommand::Stop:
      g_engine.abort();
      setHeaterGate(false);
      break;
    case UiCommand::Acknowledge:
      g_engine.acknowledge();
      break;
    case UiCommand::SaveStudy:
      setHeaterGate(false);
      saveStudy();
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
  loadSettings();

  g_sensor_bus_ready = g_thermocouple.begin();
  if (!g_sensor_bus_ready) {
    Serial.println("MAX31855 SPI3 initialization failed; heater remains locked off");
  }
  if (!g_ui.begin()) {
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
    g_last_control_ms = now_ms;
  }

  if (now_ms - g_last_ui_ms >= kUiPeriodMs) {
    g_ui.update(g_engine.snapshot(), g_engine.study());
    g_last_ui_ms = now_ms;
  }
  g_ui.tick(now_ms);
  logTrace(now_ms);
  delay(1);
}
