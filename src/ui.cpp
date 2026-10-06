#include "ui.h"
#include "ui_theme.h"
#include <Arduino.h>
#include <TFT_eSPI.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include "board_pins.h"
#include "touch_ft6336.h"

static_assert(LVGL_VERSION_MAJOR == 8 && LVGL_VERSION_MINOR == 4, "SIGNAL-3 targets LVGL 8.4.x");
static_assert(sizeof(lv_color_t) == 2, "Display flush requires 16-bit LVGL colour");

namespace {
using namespace thermal_ui;
constexpr uint16_t kScreenWidth = Width;
constexpr uint16_t kScreenHeight = Height;
constexpr uint32_t kMenuSleepMs = 30000U;
constexpr uint8_t kBacklightPwmChannel = 7;
constexpr uint32_t kBacklightPwmFrequencyHz = 5000U;
constexpr uint8_t kBacklightPwmResolutionBits = 8;
constexpr uint32_t kBrightnessHoldMs = 300U;
constexpr int kBrightnessTravelPixels = 120;
#ifdef TOASTER_BOOT_ANIMATION
constexpr uint16_t kBootWidth = 240;
constexpr uint16_t kBootHeight = 135;
constexpr uint16_t kBootFrameCount = 25;
constexpr uint32_t kBootFramePeriodMs = 80;
constexpr uint32_t kBootDurationMs = kBootFrameCount * kBootFramePeriodMs;
constexpr uint32_t kBootFrameBytes =
    static_cast<uint32_t>(kBootWidth) * kBootHeight * sizeof(uint16_t);
constexpr uint16_t kBootDisplayWidth = kScreenHeight;
constexpr uint16_t kBootDisplayHeight = kScreenWidth;
constexpr uint16_t kBootCropWidth = kBootHeight * kBootDisplayWidth / kBootDisplayHeight;
constexpr uint16_t kBootCropX = (kBootWidth - kBootCropWidth) / 2;
// Boot-only 8px overpaint: a ragged front, then the actual menu pixels.
int g_boot_reveal = -1;

extern const uint8_t kBootFramesStart[]
    asm("_binary_assets_boot_history_hot_240x135_12_5fps_rgb565_start");
extern const uint8_t kBootFramesEnd[]
    asm("_binary_assets_boot_history_hot_240x135_12_5fps_rgb565_end");
#endif

TFT_eSPI g_tft;
Ft6336Touch g_touch;
OvenUi* g_ui = nullptr;
lv_disp_draw_buf_t g_draw_buffer;
lv_color_t g_pixel_buffer[kScreenWidth * 32];

lv_color_t color(uint32_t hex) { return lv_color_hex(hex); }

lv_obj_t* box(lv_obj_t* parent, int x, int y, int w, int h, uint32_t fill,
              uint32_t stroke = 0) {
  auto* o = lv_obj_create(parent);
  lv_obj_remove_style_all(o);
  lv_obj_set_pos(o, x, y);
  lv_obj_set_size(o, w, h);
  lv_obj_set_style_bg_color(o, color(fill), 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
  if (stroke) {
    lv_obj_set_style_border_width(o, 1, 0);
    lv_obj_set_style_border_color(o, color(stroke), 0);
  }
  lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
  return o;
}

// A deterministic one-pixel weight pass over LVGL's already bundled font.
// No external font, font conversion, texture, or extra label object is needed.
void boldPass(lv_event_t* event) {
  auto* o = lv_event_get_target(event);
  lv_draw_label_dsc_t d;
  lv_draw_label_dsc_init(&d);
  lv_obj_init_draw_label_dsc(o, LV_PART_MAIN, &d);
  lv_area_t area;
  lv_obj_get_coords(o, &area);
  ++area.x1; ++area.x2;
  lv_draw_label(lv_event_get_draw_ctx(event), &d, &area, lv_label_get_text(o), nullptr);
}

lv_obj_t* label(lv_obj_t* parent, const char* text, int x, int y,
                const lv_font_t* font = &lv_font_montserrat_14, uint32_t ink = Text,
                int wrap_width = 0) {
  auto* o = lv_label_create(parent);
  lv_obj_remove_style_all(o);
  lv_obj_set_style_text_font(o, font, 0);
  lv_obj_set_style_text_color(o, color(ink), 0);
  lv_obj_set_style_text_line_space(o, 3, 0);
  lv_label_set_text(o, text);
  if (font == &lv_font_montserrat_20 || font == &lv_font_montserrat_48)
    lv_obj_add_event_cb(o, boldPass, LV_EVENT_DRAW_MAIN_END, nullptr);
  if (wrap_width) {
    lv_obj_set_width(o, wrap_width);
    lv_label_set_long_mode(o, LV_LABEL_LONG_WRAP);
  }
  lv_obj_set_pos(o, x, y);
  lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
  return o;
}

void setText(lv_obj_t* o, const char* text) {
  if (o && std::strcmp(lv_label_get_text(o), text) != 0) lv_label_set_text(o, text);
}

void sigil(lv_obj_t* parent, uint16_t mask, int x, int y, int module, uint32_t ink) {
  // Identical integer geometry on the LCD, in the host render and SVG export.
  for (unsigned r = 0; r < 3; ++r)
    for (unsigned c = 0; c < 3; ++c)
      if (occupied(mask, r, c)) box(parent, x + c * module, y + r * module,
                                  module, module, ink);
}

uint32_t modeColor(RecipeId id) {
  if (id == RecipeId::CustomAnneal) return Anneal;
  switch (recipeFor(id).mode) {
    case RunMode::Reflow: return Reflow;
    case RunMode::Anneal: return Anneal;
    case RunMode::Hold: return Chamber;
    default: return Test;
  }
}
uint16_t modeMask(RecipeId id) {
  if (id == RecipeId::CustomAnneal) return AnnealMask;
  switch (recipeFor(id).mode) {
    case RunMode::Reflow: return ReflowMask;
    case RunMode::Anneal: return AnnealMask;
    case RunMode::Hold: return ChamberMask;
    default: return TestMask;
  }
}
const char* modeName(RecipeId id) {
  if (id == RecipeId::CustomAnneal) return "ANNEAL";
  switch (recipeFor(id).mode) {
    case RunMode::Reflow: return "REFLOW";
    case RunMode::Anneal: return "ANNEAL";
    case RunMode::Hold: return "CHAMBER";
    case RunMode::Autotune: return "PID TUNE";
    case RunMode::Validation: return "PID CHECK";
    default: return "HEATER TEST";
  }
}
const char* recipeName(RecipeId id) {
  switch (id) {
    case RecipeId::LeadedReflow: return "SMD291AXT5 / trial";
    case RecipeId::Sac305Reflow: return "SAC305 solder profile";
    case RecipeId::Nylon6Anneal: return "Nylon-6 fixed profile";
    case RecipeId::CustomAnneal: return "Custom anneal program";
    case RecipeId::ChamberHold: return "Fixed chamber profile";
    case RecipeId::Commission100: return "100°C / 5 min hold";
    case RecipeId::Commission150: return "150°C / 5 min hold";
    case RecipeId::Commission200: return "200°C / 5 min hold";
    case RecipeId::Autotune100: return "Tune controller / 100°C";
    case RecipeId::Check100: return "Candidate check / 100°C";
    case RecipeId::Check150: return "Candidate check / 150°C";
    case RecipeId::Check200: return "Candidate check / 200°C";
    case RecipeId::ValidateA:
    case RecipeId::ValidateB:
    case RecipeId::ValidateC: return "Retired spatial recipe";
    case RecipeId::Invalid: return "Invalid recipe";
  }
  return "PROCESS";
}
void header(lv_obj_t* root, const char* title, const char* subtitle,
            uint32_t accent, uint16_t mask) {
  box(root, 8, 8, 40, 40, accent);
  if (mask) sigil(root, mask, 16, 16, 8, Ink);
  else { // A safety exclamation is deliberately not a mode identity sigil.
    box(root, 25, 16, 6, 15, Ink);
    box(root, 25, 35, 6, 5, Ink);
  }
  auto* slab = box(root, 50, 8, 182, 40, accent);
  label(slab, title, 8, 3, std::strlen(title) > 11 ? &lv_font_montserrat_16 :
        &lv_font_montserrat_20, Ink);
  label(slab, subtitle, 8, 25, &lv_font_montserrat_12, Ink);
}

lv_obj_t* button(lv_obj_t* root, const char* text, int x, int y, int w, int h,
                 uint32_t fill, UiCommand command, bool hold = false) {
  auto* b = lv_btn_create(root);
  lv_obj_remove_style_all(b);
  lv_obj_set_pos(b, x, y);
  lv_obj_set_size(b, w, h);
  lv_obj_set_style_bg_color(b, color(fill), 0);
  lv_obj_set_style_bg_color(b, lv_color_mix(color(fill), color(Text), 200), LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
  if (fill == Surface) {
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_color(b, color(Border), 0);
  }
  lv_obj_set_style_outline_width(b, 0, 0);
  lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_PRESS_LOCK);
  auto* text_obj = label(b, text, 0, 0, &lv_font_montserrat_16,
                         fill == Surface ? Text : Ink);
  lv_obj_add_event_cb(text_obj, boldPass, LV_EVENT_DRAW_MAIN_END, nullptr);
  lv_obj_center(text_obj); // Keep first child a label for existing host regression tests.
  if (hold) {
    auto* progress = box(b, 0, h - 3, w, 3, Ink);
    lv_obj_add_flag(progress, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(b, OvenUi::eventHandler, LV_EVENT_ALL,
                       reinterpret_cast<void*>(static_cast<uintptr_t>(command)));
  } else {
    lv_obj_add_event_cb(b, OvenUi::eventHandler,
                       command == UiCommand::Stop ? LV_EVENT_PRESSED : LV_EVENT_CLICKED,
                       reinterpret_cast<void*>(static_cast<uintptr_t>(command)));
  }
  return b;
}

void modeButton(lv_obj_t* root, const char* text, int y, uint32_t accent,
                 uint16_t mask, UiCommand command) {
  auto* b = button(root, text, 8, y, 224, 40, accent, command);
  auto* t = lv_obj_get_child(b, 0);
  lv_obj_align(t, LV_ALIGN_TOP_LEFT, 48, 12);
  sigil(b, mask, 8, 8, 8, Ink);
  box(b, 39, 6, 1, 28, Ink);
  label(b, ">", 207, 10, &lv_font_montserrat_20, Ink);
}

float peakTarget(const Recipe& r) {
  float peak = 0;
  for (uint8_t i = 0; i < r.phase_count; ++i) peak = std::max(peak, r.phases[i].target_celsius);
  return peak;
}
uint32_t holdSeconds(const Recipe& r) {
  uint32_t seconds = 0;
  for (uint8_t i = 0; i < r.phase_count; ++i)
    if (r.phases[i].kind == PhaseKind::Hold) seconds += r.phases[i].duration_seconds;
  return seconds;
}
uint8_t approvalBit(RecipeId id) {
  switch (id) {
    case RecipeId::Commission100:
    case RecipeId::Check100: return 1U << 0;
    case RecipeId::Commission150:
    case RecipeId::Check150: return 1U << 1;
    case RecipeId::Commission200:
    case RecipeId::Check200: return 1U << 2;
    default: return 0;
  }
}
int profileIndex(RecipeId id) {
  if (id == RecipeId::CustomAnneal) return 2;
  const uint8_t raw = static_cast<uint8_t>(id);
  return raw < 4U ? raw : -1;
}
const char* controllerScope(const PidStudy& study) {
  if (study.checked_scope_mask == 0U) return "LEGACY / UNVERIFIED";
  if (study.checked_scope_mask == kControlCheck100Mask) return "CHECKED 100°C ONLY";
  if (study.checked_scope_mask == (kControlCheck100Mask | kControlCheck150Mask)) return "CHECKED 100/150°C";
  if (study.checked_scope_mask == kAllControlChecksMask) return "CHECKED 100-200°C";
  return "CHECKED / LIMITED SCOPE";
}
const char* controllerScopeShort(uint8_t scope) {
  if (scope == 0U) return "UNVERIFIED";
  if (scope == kControlCheck100Mask) return "100°C ONLY";
  if (scope == (kControlCheck100Mask | kControlCheck150Mask)) return "100+150°C";
  if (scope == kAllControlChecksMask) return "100-200°C";
  return "LIMITED";
}
const char* controllerScopeShort(const PidStudy& study) {
  return controllerScopeShort(study.checked_scope_mask);
}
bool experimentalLeadedProfile(RecipeId id, const ProfileValidationView& view) {
  return id == RecipeId::LeadedReflow && view.report.status == ValidationStatus::CriteriaMissing;
}
const char* profileStatus(const ProfileValidationView& view, RecipeId id) {
  if (experimentalLeadedProfile(id, view)) return "EXPERIMENTAL";
  if (view.needs_revalidation) return "NEEDS REVALIDATION";
  if (view.commissioned) return "COMMISSIONED / FIXED SETUP";
  return toString(view.report.status);
}
const char* profileStatusShort(const ProfileValidationView& view, RecipeId id) {
  if (experimentalLeadedProfile(id, view)) return "EXPERIMENTAL";
  if (view.needs_revalidation) return "REVALIDATE";
  if (view.commissioned) return "COMMISSIONED";
  if (view.report.status == ValidationStatus::CriteriaMissing) return "SPECS MISSING";
  return toString(view.report.status);
}
bool profileViewChanged(const ProfileValidationView& a, const ProfileValidationView& b) {
  const auto& am = a.report.metrics;
  const auto& bm = b.report.metrics;
  return a.report.profile != b.report.profile || a.report.status != b.report.status ||
      a.report.liquidus.measured != b.report.liquidus.measured ||
      a.report.liquidus.value != b.report.liquidus.value ||
      am.heating_slope_measured != bm.heating_slope_measured ||
      am.heating_slope_celsius_per_second != bm.heating_slope_celsius_per_second ||
      am.peak_measured != bm.peak_measured || am.peak_celsius != bm.peak_celsius ||
      am.cooling_slope_measured != bm.cooling_slope_measured ||
      am.cooling_slope_celsius_per_second != bm.cooling_slope_celsius_per_second ||
      am.warmup_measured != bm.warmup_measured || am.warmup_ms != bm.warmup_ms ||
      am.hold_error_measured != bm.hold_error_measured ||
      am.hold_rms_error_celsius != bm.hold_rms_error_celsius ||
      am.hold_max_error_celsius != bm.hold_max_error_celsius ||
      am.hold_drift_celsius_per_second != bm.hold_drift_celsius_per_second ||
      a.consecutive_passes != b.consecutive_passes || a.eligible != b.eligible ||
      a.commissioned != b.commissioned || a.needs_revalidation != b.needs_revalidation;
}
void duration(char* out, size_t size, uint32_t seconds) {
  if (seconds >= 3600)
    std::snprintf(out, size, "%lu:%02lu:%02lu", (unsigned long)(seconds / 3600),
                  (unsigned long)(seconds / 60 % 60), (unsigned long)(seconds % 60));
  else std::snprintf(out, size, "%02lu:%02lu", (unsigned long)(seconds / 60),
                     (unsigned long)(seconds % 60));
}
const char* resultName(StudyResult s) {
  switch (s) {
    case StudyResult::Complete: return "DONE";
    case StudyResult::Failed: return "FAILED";
    case StudyResult::Aborted: return "STOPPED";
    case StudyResult::Running: return "RUNNING";
    default: return "NO RUN";
  }
}
const char* faultTitle(FaultCode f) {
  switch (f) {
    case FaultCode::ProcessProbe: return "PROBE INVALID";
    case FaultCode::ProcessOverTemperature: return "OVERTEMPERATURE";
    case FaultCode::HighTemperatureProfileNotCommissioned: return "SAC305 LOCKED";
    case FaultCode::RunTimeout: return "RUN TIMEOUT";
    case FaultCode::TestStartTooHot: return "START TOO HOT";
    case FaultCode::TuneUnstable: return "TUNE REJECTED";
    case FaultCode::NoTuneCandidate: return "NO CANDIDATE";
    case FaultCode::InvalidRecipe: return "INVALID RECIPE";
    case FaultCode::CheckNotApproved: return "CHECK LOCKED";
    case FaultCode::InvalidAnneal: return "INVALID ANNEAL";
    default: return "CHECK CONTROLLER";
  }
}
const char* faultGuidance(FaultCode f) {
  switch (f) {
    case FaultCode::ProcessProbe:
      return "Check probe plug, wiring\nand polarity. Repair before\nacknowledging.";
    case FaultCode::ProcessOverTemperature:
      return "Isolate heater power.\nCheck probe position and\nthe heater control circuit.";
    case FaultCode::HighTemperatureProfileNotCommissioned:
      return "High-temperature profile\nis not commissioned.\nDo not bypass the lock.";
    case FaultCode::RunTimeout:
      return "Check heating, cooling\nand probe contact.\nDo not bypass the timeout.";
    case FaultCode::TestStartTooHot:
      return "Let the process cool to\n60°C or below.\nThen acknowledge.";
    case FaultCode::TuneUnstable:
      return "No reliable cycles found.\nCheck contact and setup.\nOld PID unchanged.\nDo not raise the limits.";
    case FaultCode::NoTuneCandidate:
      return "Complete Tune 100 and its\ncooldown before control\nchecks.";
    case FaultCode::InvalidRecipe:
      return "This recipe ID is retired\nor unknown. No run started.\nCheck command mapping.";
    case FaultCode::CheckNotApproved:
      return "Higher-temperature check\nneeds suitability and plan\napproval first.";
    case FaultCode::InvalidAnneal:
      return "Program or estimate is\nout of range. Review all\nthree anneal values.";
    default: return "Check controller state.\nDo not start heating.";
  }
}
void field(lv_obj_t* parent, const char* name, const char* value, int y,
           uint32_t value_color = Text) {
  label(parent, name, 8, y, &lv_font_montserrat_14, Muted);
  auto* v = label(parent, value, 0, y - 1, &lv_font_montserrat_16, value_color);
  lv_obj_align(v, LV_ALIGN_TOP_RIGHT, -8, y - 1);
}

void chartDraw(lv_event_t* event) {
  auto* d = static_cast<lv_obj_draw_part_dsc_t*>(lv_event_get_param(event));
  if (!d || d->part != LV_PART_ITEMS || !d->line_dsc) return;
  // Chart series descriptor supplied by LVGL 8.4 at line draw time.
  auto* target = static_cast<lv_chart_series_t*>(lv_event_get_user_data(event));
  if (d->sub_part_ptr == target) {
    d->line_dsc->width = 1;
    d->line_dsc->dash_width = 3;
    d->line_dsc->dash_gap = 3;
  } else d->line_dsc->width = 2;
}
}  // namespace

using namespace thermal_ui;
bool OvenUi::begin(uint8_t brightness_percent) {
  g_ui = this;
  brightness_percent_ = std::max<uint8_t>(kMinimumBrightnessPercent,
      std::min<uint8_t>(100U, brightness_percent));
  ledcSetup(kBacklightPwmChannel, kBacklightPwmFrequencyHz, kBacklightPwmResolutionBits);
  ledcAttachPin(board::kTftBacklight, kBacklightPwmChannel);
  ledcWrite(kBacklightPwmChannel, 0);
  g_tft.begin();
  g_tft.setRotation(0);
  ledcWrite(kBacklightPwmChannel, 0);
  g_tft.fillScreen(TFT_BLACK);
  const bool touch_ready = g_touch.begin();
  playBootAnimation();

  lv_init();
  lv_disp_draw_buf_init(&g_draw_buffer, g_pixel_buffer, nullptr,
                        static_cast<uint32_t>(kScreenWidth) * 32U);
  static lv_disp_drv_t display_driver;
  lv_disp_drv_init(&display_driver);
  display_driver.hor_res = kScreenWidth;
  display_driver.ver_res = kScreenHeight;
  display_driver.flush_cb = displayFlush;
  display_driver.draw_buf = &g_draw_buffer;
  lv_disp_drv_register(&display_driver);

  static lv_indev_drv_t touch_driver;
  lv_indev_drv_init(&touch_driver);
  touch_driver.type = LV_INDEV_TYPE_POINTER;
  touch_driver.read_cb = touchRead;
  touch_driver.long_press_time = 2000;
  lv_indev_drv_register(&touch_driver);

  last_lv_tick_ms_ = millis();
  last_activity_ms_ = last_lv_tick_ms_;
  backlight_awake_ = true;
  showHome();
#ifdef TOASTER_BOOT_ANIMATION
  for (int step = 0; step <= 52; ++step) {
    g_boot_reveal = step;
    lv_obj_invalidate(lv_scr_act());
    lv_refr_now(nullptr);
    delay(16);
  }
  g_boot_reveal = -1;
  lv_obj_invalidate(lv_scr_act());
  lv_refr_now(nullptr);
  last_lv_tick_ms_ = last_activity_ms_ = millis();
#endif
  return touch_ready;
}

void OvenUi::playBootAnimation() {
#ifdef TOASTER_BOOT_ANIMATION
  const size_t embedded_bytes = static_cast<size_t>(kBootFramesEnd - kBootFramesStart);
  const size_t expected_bytes = static_cast<size_t>(kBootFrameBytes) * kBootFrameCount;
  if (embedded_bytes != expected_bytes) {
    ledcWrite(kBacklightPwmChannel,
              static_cast<uint32_t>(brightness_percent_) * 255U / 100U);
    return;
  }

  uint16_t line[kBootDisplayWidth];
  g_tft.setRotation(1);
  g_tft.fillScreen(TFT_BLACK);
  const uint32_t started_ms = millis();
  g_tft.setSwapBytes(true);

  for (uint16_t frame = 0; frame < kBootFrameCount; ++frame) {
    const uint32_t due_ms = started_ms + static_cast<uint32_t>(frame) * kBootFramePeriodMs;
    while (static_cast<int32_t>(millis() - due_ms) < 0) {
      delay(1);
    }

    const uint8_t* frame_data =
        kBootFramesStart + static_cast<uint32_t>(frame) * kBootFrameBytes;
    g_tft.startWrite();
    g_tft.setAddrWindow(0, 0, kBootDisplayWidth, kBootDisplayHeight);
    for (uint16_t row = 0; row < kBootDisplayHeight; ++row) {
      const uint32_t source_row = static_cast<uint32_t>(row) * kBootHeight / kBootDisplayHeight;
      for (uint16_t column = 0; column < kBootDisplayWidth; ++column) {
        const uint32_t source_column = kBootCropX +
            static_cast<uint32_t>(column) * kBootCropWidth / kBootDisplayWidth;
        std::memcpy(&line[column], frame_data +
            (source_row * kBootWidth + source_column) * sizeof(uint16_t), sizeof(uint16_t));
      }
      g_tft.pushPixels(line, kBootDisplayWidth);
    }
    g_tft.endWrite();

    if (frame == 0U) {
      ledcWrite(kBacklightPwmChannel,
                static_cast<uint32_t>(brightness_percent_) * 255U / 100U);
    }
  }

  while (millis() - started_ms < kBootDurationMs) {
    delay(1);
  }
  g_tft.setSwapBytes(false);
  g_tft.setRotation(0);
  // Keep the final HOT frame on the glass until the menu overpaints it.
#else
  g_tft.fillScreen(TFT_BLACK);
  ledcWrite(kBacklightPwmChannel,
            static_cast<uint32_t>(brightness_percent_) * 255U / 100U);
#endif
}

void OvenUi::tick(uint32_t now_ms) {
  const uint32_t elapsed = now_ms - last_lv_tick_ms_;
  if (elapsed) { lv_tick_inc(elapsed); last_lv_tick_ms_ = now_ms; }
  lv_timer_handler();
  updateHold();
  updateBrightnessGesture(now_ms);
  const bool menu = screen_ != Screen::Run && screen_ != Screen::Fault &&
      screen_ != Screen::Complete;
  if (backlight_awake_ && engine_state_ == EngineState::Idle && menu &&
      !physical_touch_down_ && now_ms - last_activity_ms_ >= kMenuSleepMs) {
    setBacklight(false);
  }
}

void OvenUi::setBacklight(bool awake) {
  if (backlight_awake_ == awake) return;
  backlight_awake_ = awake;
  ledcWrite(kBacklightPwmChannel, awake ?
      static_cast<uint32_t>(brightness_percent_) * 255U / 100U : 0U);
}

void OvenUi::setBrightness(uint8_t brightness_percent) {
  const uint8_t clamped = std::max<uint8_t>(kMinimumBrightnessPercent,
      std::min<uint8_t>(100U, brightness_percent));
  if (clamped == brightness_percent_) return;
  brightness_percent_ = clamped;
  if (backlight_awake_)
    ledcWrite(kBacklightPwmChannel,
              static_cast<uint32_t>(brightness_percent_) * 255U / 100U);
}

bool OvenUi::consumeBrightnessChange() {
  const bool changed = brightness_change_pending_;
  brightness_change_pending_ = false;
  return changed;
}

void OvenUi::updateBrightnessGesture(uint32_t now_ms) {
  if (!brightness_gesture_armed_ || !physical_touch_down_) return;
  if (!brightness_adjusting_) {
    if (now_ms - brightness_gesture_started_ms_ < kBrightnessHoldMs) return;
    brightness_adjusting_ = true;
  }
  const int delta = brightness_gesture_start_y_ - touch_y_;
  const int percent = static_cast<int>(brightness_gesture_start_percent_) +
      (delta * (100 - kMinimumBrightnessPercent) +
       (delta >= 0 ? kBrightnessTravelPixels / 2 : -kBrightnessTravelPixels / 2)) /
      kBrightnessTravelPixels;
  setBrightness(static_cast<uint8_t>(std::max<int>(kMinimumBrightnessPercent,
      std::min<int>(100, percent))));
}

void OvenUi::cancelBrightnessGesture(bool restore) {
  if (!brightness_gesture_armed_) return;
  if (restore && brightness_adjusting_) setBrightness(brightness_gesture_start_percent_);
  brightness_gesture_armed_ = false;
  brightness_adjusting_ = false;
}

void OvenUi::updateHold() {
  if (!held_button_ || !hold_progress_) return;
  const uint32_t now = millis();
  if (now - hold_refresh_ms_ < 40) return;
  hold_refresh_ms_ = now;
  const uint32_t elapsed = std::min(now - hold_started_ms_, HoldMs);
  const int width = std::max(1, static_cast<int>(lv_obj_get_width(held_button_) * elapsed / HoldMs));
  lv_obj_set_width(hold_progress_, width);
  lv_obj_clear_flag(hold_progress_, LV_OBJ_FLAG_HIDDEN);
}

void OvenUi::update(const EngineSnapshot& snapshot, const PidStudy& study) {
  temperature_trend_.update(millis(), snapshot.process_celsius, snapshot.probe_healthy);
  bool study_changed = study.saved != study_.saved || study.save_failed != study_.save_failed ||
      study.candidate_ready != study_.candidate_ready || study.tune_report.sequence != study_.tune_report.sequence;
  for (int i = 0; i < 3; ++i)
    study_changed = study_changed || study.checks[i].result != study_.checks[i].result ||
                    study.checks[i].attempts != study_.checks[i].attempts;
  study_changed = study_changed || study.required_checks_mask != study_.required_checks_mask ||
      study.approved_checks_mask != study_.approved_checks_mask ||
      study.checked_scope_mask != study_.checked_scope_mask ||
      study.setup_revision != study_.setup_revision ||
      study.checked_setup_revision != study_.checked_setup_revision ||
      study.candidate_revision != study_.candidate_revision;
  const bool state_changed = snapshot.state != snapshot_.state;
  const bool fault_changed = snapshot.fault != snapshot_.fault;
  const bool recipe_changed = snapshot.recipe != snapshot_.recipe;
  if (snapshot.state != EngineState::Idle && !backlight_awake_) {
    setBacklight(true);
    last_activity_ms_ = millis();
  }
  study_ = study;
  snapshot_ = snapshot;
  engine_state_ = snapshot.state;
  probe_healthy_ = snapshot.probe_healthy && std::isfinite(snapshot.process_celsius);
  test_start_cool_ = probe_healthy_ && snapshot.process_celsius <= 60.0F;
  const bool active = snapshot.state == EngineState::Running || snapshot.state == EngineState::Cooling;
  if (snapshot.recipe && (active || snapshot.state != EngineState::Idle)) selected_recipe_ = snapshot.recipe->id;

  if (snapshot.state == EngineState::Fault) {
    if (fault_changed || (screen_ != Screen::Fault && screen_ != Screen::TuneDetails)) showFault();
  } else if (active) {
    if (screen_ != Screen::Run || recipe_changed) showRun();
  } else if (snapshot.state == EngineState::Complete || snapshot.state == EngineState::Aborted) {
    if (screen_ != Screen::Complete || state_changed || recipe_changed || profile_validation_dirty_)
      showComplete();
  } else if (screen_ == Screen::ProfileResults && (profile_validation_dirty_ || state_changed)) {
    showProfileResults();
  } else if (profile_validation_dirty_ && screen_ == Screen::Recipe) {
    showRecipeList();
  } else if (profile_validation_dirty_ && screen_ == Screen::Confirm) {
    showConfirm(selected_recipe_);
  } else if (screen_ == Screen::Confirm && confirm_reason_ != startBlockReason()) {
    showConfirm(selected_recipe_);
  } else if (screen_ == Screen::AnnealReview &&
             confirm_start_allowed_ != customAnnealCanStart()) {
    showAnnealReview();
  } else if (study_changed) {
    if (screen_ == Screen::StudyResults) showStudyResults();
    else if (screen_ == Screen::Study) showStudy();
    else if (screen_ == Screen::StudyDetails) showStudyDetails();
  }
  refreshDynamic(snapshot);
}

void OvenUi::setProfileValidation(const ProfileValidationView* views) {
  if (!views) return;
  for (unsigned i = 0; i < 4; ++i) {
    profile_validation_dirty_ = profile_validation_dirty_ ||
        profileViewChanged(profile_validation_[i], views[i]);
    profile_validation_[i] = views[i];
  }
}

UiCommand OvenUi::consumeCommand() {
  const UiCommand command = pending_command_;
  pending_command_ = UiCommand::None;
  const bool active = engine_state_ == EngineState::Running || engine_state_ == EngineState::Cooling;
  if (command == UiCommand::Stop) return command; // Never delayed by navigation.
  if (active) return UiCommand::None;
  if (command == UiCommand::StartSelected)
    return screen_ == Screen::Confirm && selectedRecipeCanStart() ? command : UiCommand::None;
  if (command == UiCommand::StartCustomAnneal)
    return screen_ == Screen::AnnealReview && customAnnealCanStart() ? command : UiCommand::None;
  if (command == UiCommand::SaveStudy)
    return screen_ == Screen::StudyResults && canSaveStudy() && !study_.saved ? command : UiCommand::None;
  if (command == UiCommand::RecoverTuneCandidate) {
    const auto& report = study_.tune_report;
    const bool recoverable = report.available && report.terminal && !study_.candidate_ready &&
        (report.outcome == TuneOutcome::Timeout || report.outcome == TuneOutcome::Unstable) &&
        report.latest.window_count == 3;
    return screen_ == Screen::StudyResults && engine_state_ == EngineState::Idle && recoverable
        ? command : UiCommand::None;
  }
  if (command == UiCommand::CommissionProfile)
    return screen_ == Screen::ProfileResults && canCommissionSelectedProfile() ? command : UiCommand::None;
  if (command == UiCommand::Acknowledge) {
    if (screen_ == Screen::Complete && profileIndex(selected_recipe_) >= 0) showProfileResults();
    else showHome();
    return command;
  }
  // A fault cannot be escaped by an unrelated queued navigation action.
  if (engine_state_ == EngineState::Fault && command != UiCommand::ShowTuneDetails &&
      command != UiCommand::DetailsPrevious && command != UiCommand::DetailsNext &&
      command != UiCommand::DetailsBack) return UiCommand::None;
  switch (command) {
    case UiCommand::ShowReflowRecipes: showRecipeList(); break;
    case UiCommand::ShowCommissioning: showCommissioning(); break;
    case UiCommand::ShowStudy: showStudy(); break;
    case UiCommand::ShowStudyResults: showStudyResults(); break;
    case UiCommand::ChooseAutotune: showConfirm(RecipeId::Autotune100); break;
    case UiCommand::ChooseCheck100: showConfirm(RecipeId::Check100); break;
    case UiCommand::ChooseCheck150: showConfirm(RecipeId::Check150); break;
    case UiCommand::ChooseCheck200: showConfirm(RecipeId::Check200); break;
    case UiCommand::ChooseCommission100: showConfirm(RecipeId::Commission100); break;
    case UiCommand::ChooseCommission150: showConfirm(RecipeId::Commission150); break;
    case UiCommand::ChooseCommission200: showConfirm(RecipeId::Commission200); break;
    case UiCommand::ChooseLeaded: showConfirm(RecipeId::LeadedReflow); break;
    case UiCommand::ChooseSac305: showConfirm(RecipeId::Sac305Reflow); break;
    case UiCommand::ChooseAnneal: showAnnealEditor(); break;
    case UiCommand::ChooseHold: showConfirm(RecipeId::ChamberHold); break;
    case UiCommand::AnnealReview:
      if (annealFieldsComplete() && annealProgramFitsLimit()) showAnnealReview();
      break;
    case UiCommand::Home: showHome(); break;
    case UiCommand::ShowStudyDetails: details_page_ = 0; showStudyDetails(); break;
    case UiCommand::ShowTuneDetails:
      details_from_fault_ = engine_state_ == EngineState::Fault;
      details_page_ = 0; showTuneDetails(); break;
    case UiCommand::ShowProfileResults: showProfileResults(); break;
    case UiCommand::DetailsPrevious:
    case UiCommand::DetailsNext:
      if (screen_ == Screen::StudyDetails || screen_ == Screen::TuneDetails) {
        details_page_ = (details_page_ + (command == UiCommand::DetailsNext ? 1 : 2)) % 3;
        if (screen_ == Screen::StudyDetails) showStudyDetails(); else showTuneDetails();
      }
      break;
    case UiCommand::DetailsBack:
      if (screen_ == Screen::StudyDetails) showStudyResults();
      else if (screen_ == Screen::TuneDetails) {
        if (details_from_fault_) showFault(); else showStudy();
      } else if (screen_ == Screen::ProfileResults) showConfirm(selected_recipe_);
      break;
    default: break;
  }
  refreshDynamic(snapshot_);
  return UiCommand::None;
}

void OvenUi::prepareScreen(Screen screen) {
  cancelBrightnessGesture(true);
  screen_ = screen;
  // Cancel a held gesture and stale action when a fault/interlock changes the UI.
  if (pending_command_ != UiCommand::Stop) pending_command_ = UiCommand::None;
  held_button_ = hold_progress_ = nullptr;
  title_ = temperature_ = target_ = phase_ = timer_ = output_ = health_ = heater_ = detail_ = nullptr;
  unit_ = error_ = anneal_up_ = anneal_down_ = anneal_total_ = anneal_review_ = nullptr;
  demand_bar_ = chart_empty_ = chart_ = nullptr;
  for (auto& dial : anneal_dials_) dial = nullptr;
  for (auto& value : anneal_values_) value = nullptr;
  brightness_button_ = electronics_temperatures_ = nullptr;
  chart_process_ = chart_target_ = nullptr;
  for (auto& pip : stage_pips_) pip = nullptr;
  last_chart_ms_ = millis();
  auto* root = lv_scr_act();
  lv_obj_clean(root);
  lv_obj_remove_style_all(root);
  lv_obj_set_style_bg_color(root, color(Background), 0);
  lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
  lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
}

void OvenUi::showHome() {
  prepareScreen(Screen::Home);
  auto* r = lv_scr_act();
  label(r, "THERMAL", 8, 7, &lv_font_montserrat_20);
  sigil(r, BrandMask, 210, 10, 6, Muted);
  box(r, 8, 32, 224, 1, Border);
  label(r, "PROCESS", 8, 39, &lv_font_montserrat_12, Muted);
  temperature_ = label(r, "--.-", 8, 55, &lv_font_montserrat_48);
  unit_ = label(r, "°C", 178, 79, &lv_font_montserrat_20);
  brightness_button_ = lv_btn_create(r);
  lv_obj_remove_style_all(brightness_button_);
  lv_obj_set_pos(brightness_button_, 184, 53);
  lv_obj_set_size(brightness_button_, 48, 52);
  lv_obj_set_style_bg_opa(brightness_button_, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(brightness_button_, 0, 0);
  lv_obj_clear_flag(brightness_button_, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(brightness_button_, LV_OBJ_FLAG_PRESS_LOCK);
  lv_obj_add_event_cb(brightness_button_, brightnessEventHandler, LV_EVENT_ALL, nullptr);
  // 12 x 18 pixel bulb: compact native geometry, no font glyph dependency.
  box(brightness_button_, 20, 15, 8, 3, Text);
  box(brightness_button_, 17, 18, 14, 10, Text);
  box(brightness_button_, 20, 28, 8, 3, Text);
  box(brightness_button_, 21, 32, 6, 3, Text);
  health_ = label(r, "CHECK PROBE", 8, 113, &lv_font_montserrat_14, Caution);
  box(r, 8, 135, 224, 1, Border);
  modeButton(r, "REFLOW", 144, Reflow, ReflowMask, UiCommand::ShowReflowRecipes);
  modeButton(r, "ANNEAL", 196, Anneal, AnnealMask, UiCommand::ChooseAnneal);
  modeButton(r, "HEATER TESTS", 248, Test, TestMask, UiCommand::ShowCommissioning);
  electronics_temperatures_ = label(r, "MAX --°C / CPU --°C", 8, 300,
                                    &lv_font_montserrat_12, Muted);
  setElectronicsTemperatures(max_board_celsius_, cpu_celsius_);
}

void OvenUi::setElectronicsTemperatures(float max_celsius, float cpu_celsius) {
  max_board_celsius_ = max_celsius;
  cpu_celsius_ = cpu_celsius;
  if (!electronics_temperatures_) return;
  char max_text[16], cpu_text[16], text[48];
  if (std::isfinite(max_celsius))
    std::snprintf(max_text, sizeof(max_text), "%.0f°C", (double)max_celsius);
  else std::snprintf(max_text, sizeof(max_text), "--°C");
  if (std::isfinite(cpu_celsius))
    std::snprintf(cpu_text, sizeof(cpu_text), "%.0f°C", (double)cpu_celsius);
  else std::snprintf(cpu_text, sizeof(cpu_text), "--°C");
  std::snprintf(text, sizeof(text), "MAX %s / CPU %s", max_text, cpu_text);
  setText(electronics_temperatures_, text);
}

void OvenUi::showRecipeList() {
  profile_validation_dirty_ = false;
  prepareScreen(Screen::Recipe);
  auto* r = lv_scr_act();
  header(r, "REFLOW", "SELECT / 2 PROFILES", Reflow, ReflowMask);
  const RecipeId ids[] = {RecipeId::LeadedReflow, RecipeId::Sac305Reflow};
  for (int i = 0; i < 2; ++i) {
    const auto& p = recipeFor(ids[i]);
    const int y = 64 + i * 80;
    auto* b = button(r, i ? "SAC305" : "SMD291AXT5", 8, y, 224, 72, Surface,
                    i ? UiCommand::ChooseSac305 : UiCommand::ChooseLeaded);
    lv_obj_align(lv_obj_get_child(b, 0), LV_ALIGN_TOP_LEFT, 10, 6);
    box(b, 0, 0, 3, 72, Reflow);
    char text[80];
    std::snprintf(text, sizeof(text), "%.0f°C / liquidus %.0f°C", (double)peakTarget(p), (double)p.liquidus_celsius);
    label(b, text, 10, 29, &lv_font_montserrat_14, Muted);
    const auto& view = profile_validation_[profileIndex(ids[i])];
    label(b, i ? "LOCKED / NOT COMMISSIONED" :
          experimentalLeadedProfile(ids[i], view) ? "EXPERIMENTAL / 50% CAP" :
          profileStatus(view, ids[i]), 10, 51,
          &lv_font_montserrat_12, i ? Muted : Reflow);
    label(b, ">", 207, 7, &lv_font_montserrat_20, Reflow);
  }
  label(r, "100°C tuning is not\nreflow validation.", 8, 226, &lv_font_montserrat_14, Muted);
  button(r, "BACK", 8, 268, 224, 44, Reflow, UiCommand::Home);
}

void OvenUi::showCommissioning() {
  prepareScreen(Screen::Commissioning);
  auto* r = lv_scr_act();
  header(r, "HEATER TESTS", "SUPERVISED TESTS", Test, TestMask);
  label(r, "25% cap / review shows run limits", 8, 58, &lv_font_montserrat_12, Muted);
  button(r, "100°C / 5 MIN HOLD", 8, 80, 224, 40, Test, UiCommand::ChooseCommission100);
  button(r, "150°C / 5 MIN HOLD", 8, 124, 224, 40, Test, UiCommand::ChooseCommission150);
  button(r, "200°C / 5 MIN HOLD", 8, 168, 224, 40, Test, UiCommand::ChooseCommission200);
  button(r, "FIXED PROBE / PID", 8, 212, 224, 40, Test, UiCommand::ShowStudy);
  button(r, "BACK", 8, 268, 224, 44, Surface, UiCommand::Home);
}

bool OvenUi::annealFieldsComplete() const {
  return anneal_temperature_set_ && anneal_soak_set_ && anneal_ramp_set_ &&
      validAnnealProgram(anneal_program_);
}

bool OvenUi::annealProgramFitsLimit() const {
  if (!annealFieldsComplete() || !probe_healthy_ ||
      snapshot_.process_celsius > anneal_program_.target_celsius) return false;
  return annealFitsBudget(anneal_program_, snapshot_.process_celsius);
}

bool OvenUi::customAnnealCanStart() const {
  return engine_state_ == EngineState::Idle && probe_healthy_ &&
      snapshot_.process_celsius <= kAnnealMaximumProcessCelsius && annealProgramFitsLimit();
}

void OvenUi::showAnnealEditor() {
  profile_validation_dirty_ = false;
  selected_recipe_ = RecipeId::CustomAnneal;
  prepareScreen(Screen::AnnealEditor);
  auto* r = lv_scr_act();
  header(r, "ANNEAL", "60-180°C  1-110m  1-60°C/m", Anneal, AnnealMask);

  const char* names[] = {"TEMP", "SOAK", "RAMP"};
  const int minimums[] = {60, 1, 1};
  const int maximums[] = {180, 110, 60};
  const bool configured[] = {anneal_temperature_set_, anneal_soak_set_, anneal_ramp_set_};
  const int configured_values[] = {
      static_cast<int>(std::lround(anneal_program_.target_celsius)),
      static_cast<int>(anneal_program_.soak_seconds / 60U),
      static_cast<int>(std::lround(anneal_program_.ramp_celsius_per_minute))};
  const int xs[] = {82, 16, 144};
  const int ys[] = {55, 132, 132};
  const int sizes[] = {76, 80, 80};
  for (int i = 0; i < 3; ++i) {
    auto* dial = lv_arc_create(r);
    anneal_dials_[i] = dial;
    lv_obj_remove_style_all(dial);
    lv_obj_set_pos(dial, xs[i], ys[i]);
    lv_obj_set_size(dial, sizes[i], sizes[i]);
    lv_arc_set_range(dial, minimums[i], maximums[i]);
    lv_arc_set_bg_angles(dial, 135, 45);
    lv_arc_set_value(dial, configured[i] ? configured_values[i] : minimums[i]);
    lv_arc_set_change_rate(dial, 10000);
    lv_obj_set_style_arc_width(dial, 7, LV_PART_MAIN);
    lv_obj_set_style_arc_color(dial, color(Border), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(dial, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_arc_width(dial, 7, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(dial, color(Anneal), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(dial, true, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(dial, color(Text), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(dial, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_pad_all(dial, 3, LV_PART_KNOB);
    lv_obj_set_style_radius(dial, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_ext_click_area(dial, 7);
    lv_obj_add_flag(dial, LV_OBJ_FLAG_ADV_HITTEST);
    lv_obj_clear_flag(dial, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(dial, annealDialEventHandler, LV_EVENT_ALL,
        reinterpret_cast<void*>(static_cast<uintptr_t>(i)));
    auto* name = label(dial, names[i], 0, 18, &lv_font_montserrat_12, Muted);
    lv_obj_align(name, LV_ALIGN_TOP_MID, 0, 18);
    anneal_values_[i] = label(dial, "--", 0, 36, &lv_font_montserrat_16, Text);
    lv_obj_align(anneal_values_[i], LV_ALIGN_TOP_MID, 0, 36);
  }

  const char* stat_names[] = {"UP", "DOWN", "TOTAL"};
  lv_obj_t** stat_values[] = {&anneal_up_, &anneal_down_, &anneal_total_};
  for (int i = 0; i < 3; ++i) {
    auto* panel = box(r, 8 + i * 76, 220, 72, 44, Surface, Border);
    auto* name = label(panel, stat_names[i], 0, 4, &lv_font_montserrat_12, Muted);
    lv_obj_align(name, LV_ALIGN_TOP_MID, 0, 4);
    *stat_values[i] = label(panel, "--", 0, 22, &lv_font_montserrat_14, Text);
    lv_obj_align(*stat_values[i], LV_ALIGN_TOP_MID, 0, 22);
  }
  button(r, "BACK", 8, 272, 108, 40, Surface, UiCommand::Home);
  anneal_review_ = button(r, "REVIEW", 124, 272, 108, 40, Anneal,
                          UiCommand::AnnealReview);
  lv_obj_set_style_opa(anneal_review_, LV_OPA_40, LV_STATE_DISABLED);
  refreshAnnealEditor();
}

void OvenUi::refreshAnnealEditor() {
  if (screen_ != Screen::AnnealEditor || !anneal_review_) return;
  const bool set[] = {anneal_temperature_set_, anneal_soak_set_, anneal_ramp_set_};
  char text[24];
  for (int i = 0; i < 3; ++i) {
    if (!anneal_dials_[i] || !anneal_values_[i]) continue;
    if (!set[i]) std::snprintf(text, sizeof(text), "--");
    else if (i == 0) std::snprintf(text, sizeof(text), "%.0f°C",
                                   (double)anneal_program_.target_celsius);
    else if (i == 1) std::snprintf(text, sizeof(text), "%lum",
                                   (unsigned long)(anneal_program_.soak_seconds / 60U));
    else std::snprintf(text, sizeof(text), "%.0f°/m",
                       (double)anneal_program_.ramp_celsius_per_minute);
    setText(anneal_values_[i], text);
    lv_obj_set_style_arc_opa(anneal_dials_[i], set[i] ? LV_OPA_COVER : LV_OPA_TRANSP,
                             LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(anneal_dials_[i], set[i] ? LV_OPA_COVER : LV_OPA_TRANSP,
                            LV_PART_KNOB);
  }

  uint32_t up = kInvalidAnnealEstimateSeconds;
  uint32_t down = kInvalidAnnealEstimateSeconds;
  if (anneal_temperature_set_ && anneal_ramp_set_) {
    if (probe_healthy_ && snapshot_.process_celsius <= anneal_program_.target_celsius)
      up = annealDurationSeconds(anneal_program_.target_celsius - snapshot_.process_celsius,
                                 anneal_program_.ramp_celsius_per_minute);
    down = annealDurationSeconds(anneal_program_.target_celsius - kAnnealCooldownTargetCelsius,
                                 anneal_program_.ramp_celsius_per_minute);
  }
  if (up == kInvalidAnnealEstimateSeconds) std::snprintf(text, sizeof(text), "--");
  else std::snprintf(text, sizeof(text), "~%lum", (unsigned long)((up + 59U) / 60U));
  setText(anneal_up_, text);
  if (down == kInvalidAnnealEstimateSeconds) std::snprintf(text, sizeof(text), "--");
  else std::snprintf(text, sizeof(text), "~%lum", (unsigned long)((down + 59U) / 60U));
  setText(anneal_down_, text);

  const bool complete = annealFieldsComplete();
  uint32_t total = kInvalidAnnealEstimateSeconds;
  if (complete && probe_healthy_ && snapshot_.process_celsius <= anneal_program_.target_celsius) {
    total = annealEstimatedTotalSeconds(anneal_program_, snapshot_.process_celsius);
  }
  if (!complete) std::snprintf(text, sizeof(text), "SET 3");
  else if (!probe_healthy_) std::snprintf(text, sizeof(text), "PROBE");
  else if (snapshot_.process_celsius > anneal_program_.target_celsius)
    std::snprintf(text, sizeof(text), "TOO HOT");
  else if (total == kInvalidAnnealEstimateSeconds) std::snprintf(text, sizeof(text), "--");
  else if (total >= kAnnealMaximumRunSeconds) std::snprintf(text, sizeof(text), ">=120m");
  else std::snprintf(text, sizeof(text), "~%lum", (unsigned long)((total + 59U) / 60U));
  setText(anneal_total_, text);
  lv_obj_set_style_text_color(anneal_total_, color(
      complete && !annealProgramFitsLimit() ? Caution : Text), 0);
  anneal_editor_review_allowed_ = complete && annealProgramFitsLimit();
  if (anneal_editor_review_allowed_) lv_obj_clear_state(anneal_review_, LV_STATE_DISABLED);
  else lv_obj_add_state(anneal_review_, LV_STATE_DISABLED);
}

void OvenUi::showAnnealReview() {
  selected_recipe_ = RecipeId::CustomAnneal;
  confirm_start_allowed_ = customAnnealCanStart();
  prepareScreen(Screen::AnnealReview);
  auto* r = lv_scr_act();
  header(r, "ANNEAL", "TRIP 200°C / MAX 120 MIN", Anneal, AnnealMask);
  auto* table = box(r, 8, 59, 224, 108, Surface);
  char text[64];
  std::snprintf(text, sizeof(text), "%.0f°C", (double)anneal_program_.target_celsius);
  field(table, "TARGET", text, 7);
  std::snprintf(text, sizeof(text), "%lu min",
      (unsigned long)(anneal_program_.soak_seconds / 60U));
  field(table, "SOAK", text, 30);
  std::snprintf(text, sizeof(text), "%.0f°C/min", (double)anneal_program_.ramp_celsius_per_minute);
  field(table, "RAMP BOTH", text, 53);
  const uint32_t estimate = probe_healthy_ ?
      annealEstimatedTotalSeconds(anneal_program_, snapshot_.process_celsius) :
      kInvalidAnnealEstimateSeconds;
  if (estimate == kInvalidAnnealEstimateSeconds) std::snprintf(text, sizeof(text), "--");
  else std::snprintf(text, sizeof(text), "%lum", (unsigned long)((estimate + 59U) / 60U));
  field(table, "EST TOTAL", text, 76);
  label(r, "Same rate up/down; ends at 60°C.\nCannot exceed natural cooling.", 8, 176,
        &lv_font_montserrat_12, Muted, 224);
  label(r, confirm_start_allowed_ ? "PROBE VALID / READY" : !probe_healthy_ ?
        "START BLOCKED / CHECK PROBE" :
        snapshot_.process_celsius > anneal_program_.target_celsius ?
        "TOO HOT / COOL BELOW TARGET" :
        !annealFitsBudget(anneal_program_, snapshot_.process_celsius) ?
        "TOTAL EXCEEDS 120 MIN" : "CONTROLLER NOT IDLE",
        8, 209, &lv_font_montserrat_12, confirm_start_allowed_ ? Anneal : Caution);
  if (confirm_start_allowed_)
    button(r, "HOLD 2s TO START", 8, 224, 224, 40, Anneal,
           UiCommand::StartCustomAnneal, true);
  else {
    auto* b = box(r, 8, 224, 224, 40, Surface, Border);
    auto* t = label(b, "START BLOCKED", 0, 0, &lv_font_montserrat_16, Caution);
    lv_obj_center(t);
  }
  button(r, "BACK", 8, 268, 224, 44, Surface, UiCommand::ChooseAnneal);
}

uint8_t OvenUi::startBlockReason() const {
  const auto& r = recipeFor(selected_recipe_);
  if (engine_state_ != EngineState::Idle) return 1;
  if (r.requires_high_temperature_commissioning) return 2;
  if (r.mode == RunMode::Validation && !study_.candidate_ready) return 3;
  const uint8_t approval_bit = approvalBit(selected_recipe_);
  if (approval_bit && !(study_.approved_checks_mask & approval_bit)) return 7;
  if (!probe_healthy_) return 4;
  if (snapshot_.process_celsius > r.maximum_process_celsius) return 5;
  if ((r.mode == RunMode::Commissioning || isStudyRecipe(selected_recipe_)) && !test_start_cool_) return 6;
  return 0;
}
bool OvenUi::selectedRecipeCanStart() const { return startBlockReason() == 0; }

void OvenUi::showConfirm(RecipeId id) {
  profile_validation_dirty_ = false;
  selected_recipe_ = id;
  confirm_reason_ = startBlockReason();
  confirm_start_allowed_ = confirm_reason_ == 0;
  prepareScreen(Screen::Confirm);
  auto* r = lv_scr_act();
  const auto& recipe = recipeFor(id);
  const uint32_t accent = modeColor(id);
  header(r, modeName(id), confirm_start_allowed_ ? "REVIEW / READY" : "REVIEW / BLOCKED", accent, modeMask(id));
  label(r, recipeName(id), 8, 57, &lv_font_montserrat_16);
  auto* table = box(r, 8, 80, 224, 96, Surface);
  char text[64];
  std::snprintf(text, sizeof(text), "%.0f°C", (double)peakTarget(recipe));
  field(table, "TARGET", text, 7);
  if (recipe.mode == RunMode::Autotune) field(table, "CYCLES", "5-10", 29);
  else if (recipe.liquidus_celsius > 0) {
    std::snprintf(text, sizeof(text), "%.0f°C", (double)recipe.liquidus_celsius);
    field(table, "LIQUIDUS", text, 29);
  } else {
    std::snprintf(text, sizeof(text), "%lu min", (unsigned long)(holdSeconds(recipe) / 60));
    field(table, "HOLD", text, 29);
  }
  std::snprintf(text, sizeof(text), "%.0f°C", (double)recipe.maximum_process_celsius);
  field(table, "HARD LIMIT", text, 51);
  std::snprintf(text, sizeof(text), "%lum / %.0f%%", (unsigned long)(recipe.maximum_run_seconds / 60),
                (double)recipe.maximum_output_percent);
  field(table, "TIME / CAP", text, 73);
  const char* notes[] = {
    "Check probe and setup.\nSupervise the entire run.",
    "Controller is not idle.\nResolve the current state.",
    "Profile not commissioned.\nDo not bypass the lock.",
    "Finish Tune 100 and cooldown.\nThen run the fixed-probe check.",
    "Check probe plug/wiring.\nRepair before starting.",
    "Process exceeds the limit.\nLet it cool; check the setup.",
    "Start must be at or below\n60°C. Let the process cool.",
    "Suitability / check plan is\nnot approved at this target."
  };
  const int profile_index = profileIndex(id);
  const uint32_t heating_deadline = recipeHeatingDeadlineSeconds(recipe);
  if (heating_deadline < recipe.maximum_run_seconds && confirm_reason_ == 0) {
    std::snprintf(text, sizeof(text), "Heat/hold: %lum; total: %lum.\nExtra %lum: heater-off cooling.",
        (unsigned long)(heating_deadline / 60U),
        (unsigned long)(recipe.maximum_run_seconds / 60U),
        (unsigned long)((recipe.maximum_run_seconds - heating_deadline) / 60U));
    label(r, text, 8, 184, &lv_font_montserrat_12, Muted, 224);
  } else if (profile_index >= 0 && confirm_reason_ == 0) {
    char status[96];
    std::snprintf(status, sizeof(status), "CONTROL: %s\nPROFILE: %s",
                  controllerScope(study_), profileStatus(profile_validation_[profile_index], id));
    label(r, status, 8, 184, &lv_font_montserrat_12, Muted);
  } else label(r, notes[confirm_reason_], 8, 184, &lv_font_montserrat_14,
               confirm_start_allowed_ ? Muted : Caution, 224);
  if (confirm_start_allowed_) button(r, "HOLD 2s TO START", 8, 224, 224, 40, accent, UiCommand::StartSelected, true);
  else {
    auto* b = box(r, 8, 224, 224, 40, Surface, Border);
    auto* t = label(b, "START BLOCKED", 0, 0, &lv_font_montserrat_16, Caution);
    lv_obj_center(t);
  }
  const UiCommand back = isStudyRecipe(id) ? UiCommand::ShowStudy :
      recipe.mode == RunMode::Commissioning ? UiCommand::ShowCommissioning :
      recipe.mode == RunMode::Reflow ? UiCommand::ShowReflowRecipes : UiCommand::Home;
  if (profile_index >= 0) {
    button(r, "BACK", 8, 268, 108, 44, Surface, back);
    button(r, "PROFILE", 124, 268, 108, 44, Surface, UiCommand::ShowProfileResults);
  } else button(r, "BACK", 8, 268, 224, 44, Surface, back);
}

void OvenUi::showRun() {
  prepareScreen(Screen::Run);
  auto* r = lv_scr_act();
  mode_color_ = modeColor(selected_recipe_);
  header(r, modeName(selected_recipe_), recipeName(selected_recipe_), mode_color_, modeMask(selected_recipe_));
  phase_ = label(r, "RAMP / 1-3", 8, 57, &lv_font_montserrat_14, mode_color_);
  timer_ = label(r, "00:00", 0, 57, &lv_font_montserrat_14, Muted);
  lv_obj_align(timer_, LV_ALIGN_TOP_RIGHT, -8, 57);
  label(r, "PROCESS", 8, 76, &lv_font_montserrat_12, Muted);
  auto* elapsed_label = label(r, "ELAPSED", 0, 76, &lv_font_montserrat_12, Muted);
  lv_obj_align(elapsed_label, LV_ALIGN_TOP_RIGHT, -8, 76);
  temperature_ = label(r, "--.-", 8, 92, &lv_font_montserrat_48);
  unit_ = label(r, "°C", 180, 117, &lv_font_montserrat_20);
  label(r, "TARGET", 8, 147, &lv_font_montserrat_12, Muted);
  label(r, "ERROR P-T", 128, 147, &lv_font_montserrat_12, Muted);
  target_ = label(r, "--.-°C", 8, 162, &lv_font_montserrat_16);
  error_ = label(r, "--.-°C", 128, 162, &lv_font_montserrat_16);
  box(r, 118, 146, 1, 35, Border);
  const auto& recipe = snapshot_.recipe ? *snapshot_.recipe : recipeFor(selected_recipe_);
  const int count = std::min<int>(recipe.phase_count, 5);
  const int width = (224 - (count - 1) * 2) / count;
  for (int i = 0; i < count; ++i) stage_pips_[i] = box(r, 8 + i * (width + 2), 184, width, 4, Border);
  chart_ = lv_chart_create(r);
  lv_obj_remove_style_all(chart_);
  lv_obj_set_pos(chart_, 8, 194);
  lv_obj_set_size(chart_, 224, 40);
  lv_obj_set_style_bg_color(chart_, color(Surface), 0);
  lv_obj_set_style_bg_opa(chart_, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(chart_, color(Border), 0);
  lv_obj_set_style_border_width(chart_, 1, 0);
  lv_obj_set_style_line_color(chart_, color(Border), LV_PART_MAIN);
  lv_obj_set_style_line_width(chart_, 1, LV_PART_MAIN);
  lv_obj_set_style_size(chart_, 0, LV_PART_INDICATOR);
  lv_obj_clear_flag(chart_, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
  const int top = static_cast<int>(std::ceil(recipe.maximum_process_celsius / 20.0F) * 20);
  lv_chart_set_type(chart_, LV_CHART_TYPE_LINE);
  lv_chart_set_update_mode(chart_, LV_CHART_UPDATE_MODE_SHIFT);
  lv_chart_set_point_count(chart_, ChartPoints);
  lv_chart_set_range(chart_, LV_CHART_AXIS_PRIMARY_Y, 0, top);
  lv_chart_set_div_line_count(chart_, 3, 5);
  chart_process_ = lv_chart_add_series(chart_, color(mode_color_), LV_CHART_AXIS_PRIMARY_Y);
  chart_target_ = lv_chart_add_series(chart_, color(Text), LV_CHART_AXIS_PRIMARY_Y);
  lv_chart_set_all_value(chart_, chart_process_, LV_CHART_POINT_NONE);
  lv_chart_set_all_value(chart_, chart_target_, LV_CHART_POINT_NONE);
  lv_obj_add_event_cb(chart_, chartDraw, LV_EVENT_DRAW_PART_BEGIN, chart_target_);
  chart_empty_ = label(r, "COLLECTING TRACE", 0, 0, &lv_font_montserrat_12, Muted);
  lv_obj_align_to(chart_empty_, chart_, LV_ALIGN_CENTER, 0, 0);
  char text[64];
  std::snprintf(text, sizeof(text), "0-%d°C / last 30s", top);
  label(r, text, 8, 236, &lv_font_montserrat_12, Muted);
  label(r, "P", 168, 236, &lv_font_montserrat_12, mode_color_);
  box(r, 179, 242, 12, 2, mode_color_);
  label(r, "T", 197, 236, &lv_font_montserrat_12, Text);
  for (int i = 0; i < 3; ++i) box(r, 208 + i * 5, 242, 3, 1, Text);
  output_ = label(r, "DEMAND --%", 8, 251, &lv_font_montserrat_14, mode_color_);
  heater_ = label(r, "SSR CMD OFF", 0, 252, &lv_font_montserrat_12, Muted);
  lv_obj_align(heater_, LV_ALIGN_TOP_RIGHT, -8, 252);
  demand_bar_ = box(r, 8, 265, 1, 2, mode_color_);
  auto* stop = button(r, "STOP", 8, 268, 224, 44, Fault, UiCommand::Stop);
  box(stop, 56, 14, 16, 16, Ink);
  lv_obj_align(lv_obj_get_child(stop, 0), LV_ALIGN_TOP_LEFT, 90, 14);
}

void OvenUi::showFault() {
  prepareScreen(Screen::Fault);
  auto* r = lv_scr_act();
  header(r, "FAULT", "COMMAND LATCHED OFF", Fault, 0);
  title_ = label(r, faultTitle(snapshot_.fault), 8, 62, &lv_font_montserrat_20, Fault, 224);
  box(r, 8, 91, 3, 82, Fault);
  detail_ = label(r, faultGuidance(snapshot_.fault), 18, 99, &lv_font_montserrat_14, Text, 214);
  label(r, "Heater command latched off.\nAcknowledge does not restart.", 8, 181,
        &lv_font_montserrat_14, Muted, 224);
  if (study_.tune_report.available && snapshot_.recipe && snapshot_.recipe->mode == RunMode::Autotune)
    button(r, "TUNE DIAGNOSTICS", 8, 224, 224, 40, Surface, UiCommand::ShowTuneDetails);
  button(r, "ACKNOWLEDGE", 8, 268, 224, 44, Fault, UiCommand::Acknowledge);
}

void OvenUi::showComplete() {
  profile_validation_dirty_ = false;
  prepareScreen(Screen::Complete);
  auto* r = lv_scr_act();
  const bool stopped = snapshot_.state == EngineState::Aborted;
  header(r, modeName(selected_recipe_), stopped ? "STOPPED / CMD OFF" : "COMPLETE / CMD OFF",
         modeColor(selected_recipe_), modeMask(selected_recipe_));
  title_ = label(r, stopped ? "RUN STOPPED" : "CYCLE FINISHED", 8, 65, &lv_font_montserrat_20);
  label(r, "PROCESS NOW", 8, 102, &lv_font_montserrat_12, Muted);
  temperature_ = label(r, "--.-", 8, 117, &lv_font_montserrat_48);
  unit_ = label(r, "°C", 180, 143, &lv_font_montserrat_20);
  char text[64]; duration(text, sizeof(text), snapshot_.run_elapsed_seconds);
  auto* p = box(r, 8, 177, 224, 29, Surface);
  field(p, "RUN TIME", text, 7);
  const int profile_index = profileIndex(selected_recipe_);
  const bool profile = snapshot_.recipe && profile_index >= 0 &&
      (snapshot_.recipe->mode == RunMode::Reflow || snapshot_.recipe->mode == RunMode::Anneal ||
       snapshot_.recipe->mode == RunMode::Hold);
  char result[128];
  if (profile) {
    const auto& view = profile_validation_[profile_index];
    if (experimentalLeadedProfile(selected_recipe_, view))
      std::snprintf(result, sizeof(result), "PROFILE: EXPERIMENTAL\nInspect solder wetting.\nACK TO REVIEW MEASUREMENTS");
    else std::snprintf(result, sizeof(result), "PROFILE: %s\nREPEATS: %u / %u\nACK TO REVIEW RESULTS",
                  profileStatus(view, selected_recipe_), view.consecutive_passes, ValidationSequence::kRequiredPasses);
  }
  detail_ = label(r, isStudyRecipe(selected_recipe_) ?
                 "Control check recorded.\nNo profile claim is made.\nCheck before handling." : profile ? result :
                 "Heater command is off.\nCheck temperature before\nhandling the board.",
                 8, 216, &lv_font_montserrat_12, Muted, 224);
  button(r, profile ? "ACK / PROFILE RESULTS" : "HOME", 8, 268, 224, 44,
         modeColor(selected_recipe_), UiCommand::Acknowledge);
}

void OvenUi::showStudy() {
  prepareScreen(Screen::Study);
  auto* r = lv_scr_act();
  char subtitle[48];
  std::snprintf(subtitle, sizeof(subtitle), "KEEP ATTACHED / SETUP %lu",
                (unsigned long)study_.setup_revision);
  header(r, "FIXED PROBE", subtitle, Test, TestMask);
  label(r, "Tune once at 100°C. Keep setup\nand probe position unchanged.", 8, 54, &lv_font_montserrat_12, Muted);
  button(r, "TUNE CONTROLLER 100°C", 8, 88, 224, 40, Test, UiCommand::ChooseAutotune);
  label(r, study_.saved ? "SAVED / ACTIVE PID" : study_.candidate_ready ?
        "CANDIDATE / NOT SAVED" : "CHECKS LOCKED UNTIL TUNED", 8, 133, &lv_font_montserrat_12, Muted);
  button(r, "100°C", 8, 151, 72, 44, Test, UiCommand::ChooseCheck100);
  button(r, study_.approved_checks_mask & 2U ? "150°C" : "150 LCK", 84, 151, 72, 44,
         study_.approved_checks_mask & 2U ? Test : Surface, UiCommand::ChooseCheck150);
  button(r, study_.approved_checks_mask & 4U ? "200°C" : "200 LCK", 160, 151, 72, 44,
         study_.approved_checks_mask & 4U ? Test : Surface, UiCommand::ChooseCheck200);
  button(r, "PID RESULTS / SAVE", 8, 204, 224, 40, Test, UiCommand::ShowStudyResults);
  if (study_.tune_report.available) {
    button(r, "BACK", 8, 268, 108, 44, Surface, UiCommand::ShowCommissioning);
    button(r, "TUNE DATA", 124, 268, 108, 44, Surface, UiCommand::ShowTuneDetails);
  } else button(r, "BACK", 8, 268, 224, 44, Surface, UiCommand::ShowCommissioning);
}

bool OvenUi::canSaveStudy() const {
  return engine_state_ == EngineState::Idle && pidStudyChecksSatisfied(study_);
}

bool OvenUi::canCommissionSelectedProfile() const {
  const int index = profileIndex(selected_recipe_);
  return index >= 0 && engine_state_ == EngineState::Idle &&
      profile_validation_[index].eligible && !profile_validation_[index].commissioned;
}

void OvenUi::showStudyResults() {
  prepareScreen(Screen::StudyResults);
  const bool candidate_warnings = study_.candidate_ready && study_.tune_report.latest.failed_checks != 0;
  auto* r = lv_scr_act();
  header(r, "PID RESULTS", study_.save_failed ? "FAILED / OLD PID KEPT" : study_.saved ? "SAVED / ACTIVE" :
         candidate_warnings ? "CANDIDATE / WARNINGS" :
         study_.candidate_ready ? "CANDIDATE / NOT SAVED" : "ACTIVE / NO CANDIDATE", Test, TestMask);
  auto* table = box(r, 8, 60, 224, 77, Surface);
  char text[64];
  const uint8_t candidate_scope = pidCandidateCheckedScope(study_);
  const uint8_t shown_scope = study_.candidate_ready ? candidate_scope : study_.checked_scope_mask;
  std::snprintf(text, sizeof(text), "%.1f/%.3f/%.0f", (double)study_.active.kp,
                (double)study_.active.ki, (double)study_.active.kd);
  field(table, "ACTIVE P/I/D", text, 7);
  if (study_.candidate_ready) std::snprintf(text, sizeof(text), "%.1f/%.3f/%.0f",
      (double)study_.candidate.kp, (double)study_.candidate.ki, (double)study_.candidate.kd);
  else std::snprintf(text, sizeof(text), "--");
  field(table, "CANDIDATE", text, 30);
  field(table, "SCOPE", controllerScopeShort(shown_scope), 53);
  for (int i = 0; i < 3; ++i) {
    auto* p = box(r, 8 + i * 76, 145, 72, 43, Surface);
    const char* targets[] = {"100°C", "150°C", "200°C"};
    auto* t = label(p, targets[i], 0, 1, &lv_font_montserrat_16); lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 1);
    const bool approved = study_.approved_checks_mask & (1U << i);
    const bool retained = (candidate_scope & (1U << i)) && study_.checks[i].result == StudyResult::Empty;
    t = label(p, approved ? retained ? "SAVED" : resultName(study_.checks[i].result) : "LOCKED", 0, 24, &lv_font_montserrat_12, Muted);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 24);
    if (candidate_scope & (1U << i)) box(p, 0, 40, 72, 3, Test);
  }
  label(r, study_.save_failed ? "Save failed. Old PID kept.\nCandidate remains unsaved." :
        shown_scope == 1U ? "CHECKED AT 100°C ONLY\nPROFILE: SPECS MISSING" :
        shown_scope == 3U ? "CHECKED AT 100/150°C\nPROFILE: SPECS MISSING" :
        "Controller scope shown above.\nPROFILE: SPECS MISSING", 8, 190, &lv_font_montserrat_14, Muted);
  if (study_.saved) {
    auto* b = box(r, 8, 230, 224, 32, Test);
    auto* t = label(b, "SAVED / ACTIVE PID", 0, 0, &lv_font_montserrat_14, Ink); lv_obj_center(t);
  } else if (canSaveStudy()) button(r, study_.save_failed ? "HOLD 2s: RETRY SAVE" :
      "HOLD 2s: SAVE + USE", 8, 224, 224, 40, Test, UiCommand::SaveStudy, true);
  else if (!study_.candidate_ready && study_.tune_report.available && study_.tune_report.terminal &&
           (study_.tune_report.outcome == TuneOutcome::Timeout ||
       study_.tune_report.outcome == TuneOutcome::Unstable) &&
           study_.tune_report.latest.window_count == 3)
    button(r, "USE LAST ESTIMATE", 8, 224, 224, 40, Test, UiCommand::RecoverTuneCandidate);
  else label(r, "REQUIRED CHECKS NEEDED", 8, 239, &lv_font_montserrat_12, Muted);
  button(r, "BACK", 8, 268, 108, 44, Surface, UiCommand::ShowStudy);
  button(r, "DETAILS", 124, 268, 108, 44, Surface, UiCommand::ShowStudyDetails);
}

void OvenUi::showStudyDetails() {
  prepareScreen(Screen::StudyDetails);
  auto* r = lv_scr_act();
  char text[100];
  const unsigned targets[] = {100, 150, 200};
  const bool retained = study_.checks[details_page_].result == StudyResult::Empty &&
      (pidCandidateCheckedScope(study_) & (1U << details_page_));
  std::snprintf(text, sizeof(text), "CHECK %u°C / %s", targets[details_page_],
                retained ? "SAVED" : resultName(study_.checks[details_page_].result));
  header(r, "CHECK DETAIL", text, Test, TestMask);
  const auto& p = study_.checks[details_page_];
  auto* table = box(r, 8, 60, 224, 153, Surface);
  const bool available = p.result != StudyResult::Empty;
  std::snprintf(text, sizeof(text), "%lu", (unsigned long)p.attempts); field(table, "ATTEMPTS", text, 7);
  std::snprintf(text, sizeof(text), "%.1f°C", (double)p.start_celsius); field(table, "START", available ? text : "--", 30);
  std::snprintf(text, sizeof(text), "%.1f°C", (double)p.peak_celsius); field(table, "PEAK", available ? text : "--", 53);
  char rise_label[16];
  std::snprintf(rise_label, sizeof(rise_label), "TO %u°C", targets[details_page_] - 4U);
  std::snprintf(text, sizeof(text), "%lus", (unsigned long)p.rise_seconds);
  field(table, rise_label, p.reached_band ? text : available ? "NOT REACHED" : "--", 76);
  std::snprintf(text, sizeof(text), "%.2f°C", (double)p.hold_rmse); field(table, "HOLD RMS", p.reached_band ? text : "--", 99);
  std::snprintf(text, sizeof(text), "%.1f%%", (double)p.hold_output_percent); field(table, "MEAN DEMAND", p.reached_band ? text : "--", 122);
  if (retained) label(r, "Saved scope; metrics not retained.", 8, 194, &lv_font_montserrat_12, Muted);
  button(r, "< PREV", 8, 224, 108, 40, Test, UiCommand::DetailsPrevious);
  button(r, "NEXT >", 124, 224, 108, 40, Test, UiCommand::DetailsNext);
  button(r, "BACK", 8, 268, 224, 44, Surface, UiCommand::DetailsBack);
}

void OvenUi::showTuneDetails() {
  prepareScreen(Screen::TuneDetails);
  auto* r = lv_scr_act();
  char text[128];
  std::snprintf(text, sizeof(text), "PAGE %u/3 / CMD OFF", details_page_ + 1U);
  header(r, "TUNE DATA", text, details_from_fault_ ? Fault : Test, TestMask);
  const auto& d = study_.tune_report.latest;
  auto* table = box(r, 8, 60, 224, 128, Surface);
  if (!study_.tune_report.available) label(table, "NO TUNE REPORT", 8, 10);
  else if (details_page_ == 0) {
    std::snprintf(text, sizeof(text), "%u", d.cycle); field(table, "CYCLE", text, 7);
    std::snprintf(text, sizeof(text), "%.1fs", (double)d.period); field(table, "PERIOD", text, 30);
    std::snprintf(text, sizeof(text), "%.1f%%", (double)d.fraction * 100); field(table, "HEAT FRACTION", text, 53);
    std::snprintf(text, sizeof(text), "%.2f°C", (double)d.amplitude); field(table, "AMPLITUDE", text, 76);
    std::snprintf(text, sizeof(text), "%.2f°C", (double)d.midpoint); field(table, "MIDPOINT", text, 99);
    label(r, "Reference: P 40-300s / F 30-70%\nA 2-12°C / M 96-104°C", 8, 194, &lv_font_montserrat_12, Muted);
  } else if (details_page_ == 1) {
    std::snprintf(text, sizeof(text), "%u/3", d.window_count); field(table, "WINDOW", text, 7);
    std::snprintf(text, sizeof(text), "%.2f", (double)d.period_ratio); field(table, "PERIOD RATIO", text, 30);
    std::snprintf(text, sizeof(text), "%.2f", (double)d.amplitude_ratio); field(table, "AMPL. RATIO", text, 53);
    std::snprintf(text, sizeof(text), "%.2f°C", (double)d.midpoint_span); field(table, "MIDPOINT SPAN", text, 76);
    std::snprintf(text, sizeof(text), "0x%04lX", (unsigned long)d.failed_checks); field(table, "CHECK FLAGS", text, 99);
    label(r, "Ratios <=1.2 / span <=1°C.\nQuality warnings; test response.", 8, 194, &lv_font_montserrat_12, Muted);
  } else {
    // Latest three measurements, all in one small table. Limits stay on page 1.
    label(table, "CYCLE / PERIOD / AMPL / MID", 8, 7, &lv_font_montserrat_12, Muted);
    for (unsigned i = 0; i < 3; ++i) {
      if (i < d.window_count) {
        const auto& w = d.window[i];
        std::snprintf(text, sizeof(text), "%u / %.0fs / %.1f / %.1f", w.cycle,
                      (double)w.period, (double)w.amplitude, (double)w.midpoint);
      } else std::snprintf(text, sizeof(text), "-- / -- / -- / --");
      label(table, text, 8, 30 + i * 28, &lv_font_montserrat_14);
    }
    label(r, "Amplitude / midpoint in °C.\nFull diagnostic flags: USB log.", 8, 194, &lv_font_montserrat_12, Muted);
  }
  button(r, "< PREV", 8, 224, 108, 40, details_from_fault_ ? Surface : Test, UiCommand::DetailsPrevious);
  button(r, "NEXT >", 124, 224, 108, 40, details_from_fault_ ? Surface : Test, UiCommand::DetailsNext);
  button(r, "BACK", 8, 268, 224, 44, Surface, UiCommand::DetailsBack);
}

void OvenUi::showProfileResults() {
  const int index = profileIndex(selected_recipe_);
  if (index < 0) { showHome(); return; }
  prepareScreen(Screen::ProfileResults);
  profile_validation_dirty_ = false;
  auto* r = lv_scr_act();
  const auto& view = profile_validation_[index];
  const auto& report = view.report;
  const uint32_t accent = modeColor(selected_recipe_);
  const bool experimental = experimentalLeadedProfile(selected_recipe_, view);
  header(r, "PROFILE REVIEW", profileStatusShort(view, selected_recipe_), accent, modeMask(selected_recipe_));
  auto* identity = box(r, 8, 60, 224, 68, Surface);
  field(identity, "CONTROL", controllerScopeShort(study_), 7);
  field(identity, "PROFILE", profileStatusShort(view, selected_recipe_), 29);
  char text[64];
  if (experimental) std::snprintf(text, sizeof(text), "205°C / 50%%");
  else std::snprintf(text, sizeof(text), "%u / %u", view.consecutive_passes,
                     ValidationSequence::kRequiredPasses);
  field(identity, experimental ? "RECIPE" : "REPEATS", text, 51);
  auto* metrics = box(r, 8, 132, 224, 90, Surface);
  if (report.profile == ValidationProfile::Anneal) {
    if (report.metrics.warmup_measured) std::snprintf(text, sizeof(text), "%.1fs",
        (double)report.metrics.warmup_ms / 1000.0); else std::snprintf(text, sizeof(text), "--");
    field(metrics, "WARM-UP", text, 7);
    if (report.metrics.hold_error_measured) std::snprintf(text, sizeof(text), "%.2f°C",
        (double)report.metrics.hold_rms_error_celsius); else std::snprintf(text, sizeof(text), "--");
    field(metrics, "HOLD RMS", text, 29);
    if (report.metrics.hold_error_measured) std::snprintf(text, sizeof(text), "%.2f°C",
        (double)report.metrics.hold_max_error_celsius); else std::snprintf(text, sizeof(text), "--");
    field(metrics, "MAX ERROR", text, 51);
    if (report.metrics.hold_error_measured) std::snprintf(text, sizeof(text), "%.4f°C/s",
        (double)report.metrics.hold_drift_celsius_per_second); else std::snprintf(text, sizeof(text), "--");
    field(metrics, "DRIFT", text, 73);
  } else {
    if (report.metrics.heating_slope_measured) std::snprintf(text, sizeof(text), "%.2f°C/s",
        (double)report.metrics.heating_slope_celsius_per_second); else std::snprintf(text, sizeof(text), "--");
    field(metrics, "HEAT SLOPE", text, 7);
    if (report.metrics.peak_measured) std::snprintf(text, sizeof(text), "%.1f°C",
        (double)report.metrics.peak_celsius); else std::snprintf(text, sizeof(text), "--");
    field(metrics, "PEAK", text, 29);
    if (report.liquidus.measured) std::snprintf(text, sizeof(text), experimental ? "~%.0fs" : "%.1fs",
        (double)report.liquidus.value); else std::snprintf(text, sizeof(text), "--");
    field(metrics, experimental ? "TAL / 183°C" : "LIQUIDUS", text, 51);
    if (report.metrics.cooling_slope_measured) std::snprintf(text, sizeof(text), "%.2f°C/s",
        (double)report.metrics.cooling_slope_celsius_per_second); else std::snprintf(text, sizeof(text), "--");
    field(metrics, "COOL SLOPE", text, 73);
  }
  if (view.commissioned) {
    auto* b = box(r, 8, 224, 224, 40, accent);
    auto* t = label(b, "COMMISSIONED / FIXED SETUP", 0, 0, &lv_font_montserrat_12, Ink); lv_obj_center(t);
  } else if (canCommissionSelectedProfile()) {
    button(r, "HOLD 2s: COMMISSION", 8, 224, 224, 40, accent,
           UiCommand::CommissionProfile, true);
  } else {
    auto* b = box(r, 8, 224, 224, 40, Surface, Border);
    auto* t = label(b, experimental ? "TRIAL / NOT VALIDATED" :
                    report.status == ValidationStatus::CriteriaMissing ? "SPECS MISSING" :
                    view.consecutive_passes < ValidationSequence::kRequiredPasses ? "REPEATS REQUIRED" :
                    "NOT ELIGIBLE", 0, 0, &lv_font_montserrat_14, Caution); lv_obj_center(t);
  }
  button(r, "BACK", 8, 268, 224, 44, Surface, UiCommand::DetailsBack);
}

void OvenUi::refreshDynamic(const EngineSnapshot& s) {
  char text[192];
  if (temperature_) {
    if (probe_healthy_) std::snprintf(text, sizeof(text), "%.1f", (double)s.process_celsius);
    else std::snprintf(text, sizeof(text), "--.-");
    setText(temperature_, text);
    lv_point_t size;
    lv_txt_get_size(&size, text, &lv_font_montserrat_48, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    const int reading_width = screen_ == Screen::Home ? 140 : 184;
    const lv_font_t* font = size.x <= reading_width ? &lv_font_montserrat_48 : &lv_font_montserrat_32;
    lv_obj_set_style_text_font(temperature_, font, 0);
    lv_txt_get_size(&size, text, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    if (unit_) {
      lv_obj_set_x(unit_, std::min<int>(194, 8 + size.x + 4));
      lv_obj_set_y(unit_, lv_obj_get_style_y(temperature_, LV_PART_MAIN) + (font == &lv_font_montserrat_48 ? 26 : 12));
    }
  }
  if (screen_ == Screen::Home) {
    if (!probe_healthy_) std::snprintf(text, sizeof(text), "CHECK PROBE");
    else if (!temperature_trend_.ready()) std::snprintf(text, sizeof(text), "MEASURING TREND");
    else if (temperature_trend_.state() == TrendState::Stable)
      std::snprintf(text, sizeof(text), "STABLE");
    else std::snprintf(text, sizeof(text), "%s %.1f°C/min",
        temperature_trend_.state() == TrendState::Rising ? LV_SYMBOL_UP : LV_SYMBOL_DOWN,
        (double)std::fabs(temperature_trend_.rateCelsiusPerMinute()));
    setText(health_, text);
    lv_obj_set_style_text_color(health_, color(probe_healthy_ ? Muted : Caution), 0);
  }
  if (screen_ == Screen::Fault) {
    setText(title_, faultTitle(s.fault));
    setText(detail_, faultGuidance(s.fault));
  }
  if (screen_ == Screen::AnnealEditor) refreshAnnealEditor();
  if (screen_ != Screen::Run) return;
  const bool cooling = s.state == EngineState::Cooling;
  if (cooling) { setText(target_, "OFF"); setText(error_, "--"); }
  else {
    if (std::isfinite(s.target_celsius)) std::snprintf(text, sizeof(text), "%.1f°C", (double)s.target_celsius);
    else std::snprintf(text, sizeof(text), "--");
    setText(target_, text);
    if (probe_healthy_ && std::isfinite(s.target_celsius))
      std::snprintf(text, sizeof(text), "%+.1f°C", (double)(s.process_celsius - s.target_celsius));
    else std::snprintf(text, sizeof(text), "--");
    setText(error_, text);
  }
  if (s.recipe && s.phase_index < s.recipe->phase_count) {
    const char* phase = toString(s.recipe->phases[s.phase_index].kind);
    if (s.recipe->phases[s.phase_index].kind == PhaseKind::ControlledCool)
      phase = "RAMP DOWN";
    if (s.recipe->mode == RunMode::Reflow) {
      const char* stages[] = {"PREHEAT", "SOAK", "TO PEAK", "PEAK", "COOL"};
      if (s.phase_index < 5) phase = stages[s.phase_index];
    }
    if (s.recipe->mode == RunMode::Autotune && !cooling)
      std::snprintf(text, sizeof(text), "CYCLE %u/10", study_.cycles);
    else std::snprintf(text, sizeof(text), "%s %u/%u", phase,
                       s.phase_index + 1U, (unsigned)s.recipe->phase_count);
    setText(phase_, text);
    for (int i = 0; i < 5; ++i) if (stage_pips_[i])
      lv_obj_set_style_bg_color(stage_pips_[i], color(i <= s.phase_index ? mode_color_ : Border), 0);
  }
  duration(text, sizeof(text), s.run_elapsed_seconds); setText(timer_, text);
  if (std::isfinite(s.output_percent)) std::snprintf(text, sizeof(text), "DEMAND %.0f%%", (double)s.output_percent);
  else std::snprintf(text, sizeof(text), "DEMAND --");
  setText(output_, text);
  setText(heater_, s.heater_commanded_on ? "SSR CMD ON" : "SSR CMD OFF");
  lv_obj_set_style_text_color(heater_, color(s.heater_commanded_on ? mode_color_ : Muted), 0);
  const float demand = std::isfinite(s.output_percent) ? std::max(0.0F, std::min(100.0F, s.output_percent)) : 0;
  lv_obj_set_width(demand_bar_, std::max(1, static_cast<int>(224 * demand / 100)));
  if (demand <= 0) lv_obj_add_flag(demand_bar_, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_clear_flag(demand_bar_, LV_OBJ_FLAG_HIDDEN);
  const uint32_t now = millis();
  if (now - last_chart_ms_ >= ChartPeriodMs && chart_) {
    // A delayed UI update creates a gap rather than a fake continuous history.
    if (now - last_chart_ms_ >= ChartPeriodMs * 2) {
      const uint32_t missed = std::min<uint32_t>(ChartPoints, (now - last_chart_ms_) / ChartPeriodMs - 1);
      for (uint32_t i = 0; i < missed; ++i) {
        lv_chart_set_next_value(chart_, chart_process_, LV_CHART_POINT_NONE);
        lv_chart_set_next_value(chart_, chart_target_, LV_CHART_POINT_NONE);
      }
    }
    const auto point = [](float v, bool valid) -> lv_coord_t {
      return valid && std::isfinite(v) && v >= 0 && v < 2000 ?
        static_cast<lv_coord_t>(std::lround(v)) : LV_CHART_POINT_NONE;
    };
    lv_chart_set_next_value(chart_, chart_process_, point(s.process_celsius, probe_healthy_));
    lv_chart_set_next_value(chart_, chart_target_, point(s.target_celsius, !cooling));
    if (chart_empty_) lv_obj_add_flag(chart_empty_, LV_OBJ_FLAG_HIDDEN);
    last_chart_ms_ = now;
  }
}

void OvenUi::queue(UiCommand command) {
  if (pending_command_ == UiCommand::Stop && command != UiCommand::Stop) return;
  pending_command_ = command;
}

void OvenUi::annealDialEventHandler(lv_event_t* event) {
  if (!g_ui || g_ui->screen_ != Screen::AnnealEditor ||
      g_ui->engine_state_ != EngineState::Idle) return;
  const lv_event_code_t code = lv_event_get_code(event);
  if (code != LV_EVENT_PRESSED && code != LV_EVENT_VALUE_CHANGED) return;
  const unsigned index = static_cast<unsigned>(
      reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
  if (index >= 3U || lv_event_get_target(event) != g_ui->anneal_dials_[index]) return;

  const int value = lv_arc_get_value(g_ui->anneal_dials_[index]);
  if (index == static_cast<unsigned>(AnnealField::Temperature)) {
    g_ui->anneal_program_.target_celsius = static_cast<float>(value);
    g_ui->anneal_temperature_set_ = true;
  } else if (index == static_cast<unsigned>(AnnealField::Soak)) {
    g_ui->anneal_program_.soak_seconds = static_cast<uint32_t>(value) * 60U;
    g_ui->anneal_soak_set_ = true;
  } else {
    g_ui->anneal_program_.ramp_celsius_per_minute = static_cast<float>(value);
    g_ui->anneal_ramp_set_ = true;
  }
  g_ui->refreshAnnealEditor();
}

void OvenUi::eventHandler(lv_event_t* event) {
  if (!g_ui) return;
  const auto command = static_cast<UiCommand>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
  const lv_event_code_t code = lv_event_get_code(event);
  if (command == UiCommand::StartSelected || command == UiCommand::StartCustomAnneal ||
      command == UiCommand::SaveStudy ||
      command == UiCommand::CommissionProfile) {
    auto* b = lv_event_get_target(event);
    if (code == LV_EVENT_PRESSED) {
      g_ui->held_button_ = b;
      g_ui->hold_progress_ = lv_obj_get_child(b, 1);
      g_ui->hold_started_ms_ = millis();
      g_ui->hold_refresh_ms_ = millis() - 40;
    } else if (code == LV_EVENT_LONG_PRESSED) {
      // LVGL enforces the registered 2000 ms hold. Recheck eligibility at consumption.
      g_ui->queue(command);
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST || code == LV_EVENT_DELETE) {
      if (g_ui->held_button_ == b) {
        if (g_ui->hold_progress_ && code != LV_EVENT_DELETE)
          lv_obj_add_flag(g_ui->hold_progress_, LV_OBJ_FLAG_HIDDEN);
        g_ui->held_button_ = g_ui->hold_progress_ = nullptr;
      }
    }
  } else g_ui->queue(command);
}

void OvenUi::brightnessEventHandler(lv_event_t* event) {
  if (!g_ui) return;
  const lv_event_code_t code = lv_event_get_code(event);
  auto* button = lv_event_get_target(event);
  if (code == LV_EVENT_PRESSED) {
    g_ui->brightness_gesture_armed_ = true;
    g_ui->brightness_adjusting_ = false;
    g_ui->brightness_gesture_start_percent_ = g_ui->brightness_percent_;
    g_ui->brightness_gesture_start_y_ = g_ui->touch_y_;
    g_ui->brightness_gesture_started_ms_ = millis();
  } else if (code == LV_EVENT_RELEASED) {
    if (g_ui->brightness_gesture_armed_ && g_ui->brightness_adjusting_ &&
        g_ui->brightness_percent_ != g_ui->brightness_gesture_start_percent_)
      g_ui->brightness_change_pending_ = true;
    g_ui->brightness_gesture_armed_ = false;
    g_ui->brightness_adjusting_ = false;
  } else if ((code == LV_EVENT_PRESS_LOST || code == LV_EVENT_DELETE) &&
             g_ui->brightness_button_ == button) {
    g_ui->cancelBrightnessGesture(code != LV_EVENT_DELETE);
  }
}

void OvenUi::displayFlush(lv_disp_drv_t* display, const lv_area_t* area, lv_color_t* pixels) {
  const uint32_t width = static_cast<uint32_t>(area->x2 - area->x1 + 1);
  const uint32_t height = static_cast<uint32_t>(area->y2 - area->y1 + 1);
#ifdef TOASTER_BOOT_ANIMATION
  if (g_boot_reveal >= 0) {
    g_tft.startWrite();
    for (int y = area->y1; y <= area->y2; ++y) {
      for (int x = area->x1; x <= area->x2;) {
        const int count = std::min(8 - x % 8, area->x2 - x + 1);
        const unsigned tx = x / 8, ty = y / 8;
        const unsigned hash = (tx * 73U + ty * 151U + (tx ^ ty) * 19U);
        const int arrival = static_cast<int>(ty + hash % 11U);
        if (g_boot_reveal >= arrival && g_boot_reveal <= arrival + 2) {
          uint16_t edge[8];
          auto* source = reinterpret_cast<uint16_t*>(pixels) +
              (y - area->y1) * width + x - area->x1;
          if (g_boot_reveal < arrival + 2) {
            const uint32_t palette[] = {Surface, Border, Reflow, Anneal, Chamber};
            const uint16_t ink = lv_color_hex(palette[hash % 5U]).full;
            for (int i = 0; i < count; ++i) edge[i] = ink;
            source = edge;
          }
          g_tft.setAddrWindow(x, y, count, 1);
          g_tft.pushColors(source, count, true);
        }
        x += count;
      }
    }
    g_tft.endWrite();
    lv_disp_flush_ready(display);
    return;
  }
#endif
  g_tft.startWrite();
  g_tft.setAddrWindow(area->x1, area->y1, width, height);
  g_tft.pushColors(reinterpret_cast<uint16_t*>(pixels), width * height, true);
  g_tft.endWrite();
  lv_disp_flush_ready(display);
}
void OvenUi::touchRead(lv_indev_drv_t*, lv_indev_data_t* data) {
  TouchPoint point;
  const bool down = g_touch.read(point);
  if (g_ui) {
    g_ui->physical_touch_down_ = down;
    if (down) g_ui->touch_y_ = point.y;
  }
  if (g_ui && !g_ui->backlight_awake_) {
    if (down) {
      g_ui->setBacklight(true);
      g_ui->consume_wake_touch_ = true;
      g_ui->last_activity_ms_ = millis();
    }
    data->state = LV_INDEV_STATE_REL;
    return;
  }
  if (g_ui && g_ui->consume_wake_touch_) {
    if (!down) g_ui->consume_wake_touch_ = false;
    data->state = LV_INDEV_STATE_REL;
    return;
  }
  if (down) {
    if (g_ui) g_ui->last_activity_ms_ = millis();
    data->point.x = point.x;
    data->point.y = point.y;
    data->state = LV_INDEV_STATE_PR;
  } else data->state = LV_INDEV_STATE_REL;
}
