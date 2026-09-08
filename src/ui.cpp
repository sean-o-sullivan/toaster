#include "ui.h"

#include <Arduino.h>
#include <TFT_eSPI.h>

#include <algorithm>
#include <cstring>
#include <cstdio>

#include "board_pins.h"
#include "touch_ft6336.h"

namespace {

constexpr uint32_t kGraphite = 0x080B09;
constexpr uint32_t kPanel = 0x111713;
constexpr uint32_t kBorder = 0x39483E;
constexpr uint32_t kText = 0xE4EBDD;
constexpr uint32_t kMuted = 0x9DAA9F;
constexpr uint32_t kAmber = 0xFFB454;
constexpr uint32_t kCyan = 0xC5F76B;  // Signal accent; chart/ready role retained.
constexpr uint32_t kRed = 0xFF6259;
constexpr uint16_t kScreenWidth = 240;
constexpr uint16_t kScreenHeight = 320;
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

void setLabel(lv_obj_t* label, const char* text, const lv_font_t* font, lv_color_t text_color,
              lv_align_t align, int16_t x, int16_t y) {
  lv_label_set_text(label, text);
  lv_obj_set_style_text_font(label, font, 0);
  lv_obj_set_style_text_color(label, text_color, 0);
  lv_obj_align(label, align, x, y);
}

lv_obj_t* makeLabel(lv_obj_t* parent, const char* text, const lv_font_t* font,
                    lv_color_t text_color, lv_align_t align, int16_t x, int16_t y) {
  lv_obj_t* label = lv_label_create(parent);
  setLabel(label, text, font, text_color, align, x, y);
  return label;
}

lv_obj_t* makeWrappedLabel(lv_obj_t* parent, const char* text, const lv_font_t* font,
                           lv_color_t text_color, int16_t width, lv_align_t align, int16_t x,
                           int16_t y) {
  lv_obj_t* label = makeLabel(parent, text, font, text_color, align, x, y);
  lv_obj_set_width(label, width);
  lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_line_space(label, 3, 0);
  return label;
}

lv_obj_t* makePanel(lv_obj_t* parent, int16_t x, int16_t y, int16_t width, int16_t height) {
  lv_obj_t* panel = lv_obj_create(parent);
  lv_obj_set_pos(panel, x, y);
  lv_obj_set_size(panel, width, height);
  lv_obj_set_style_bg_color(panel, color(kPanel), 0);
  lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(panel, color(kBorder), 0);
  lv_obj_set_style_border_width(panel, 1, 0);
  lv_obj_set_style_radius(panel, 0, 0);
  lv_obj_set_style_pad_all(panel, 0, 0);
  lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
  return panel;
}

void makeRule(lv_obj_t* parent, int16_t y) {
  lv_obj_t* rule = lv_obj_create(parent);
  lv_obj_set_pos(rule, 12, y);
  lv_obj_set_size(rule, 216, 1);
  lv_obj_set_style_bg_color(rule, color(kBorder), 0);
  lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(rule, 0, 0);
  lv_obj_set_style_pad_all(rule, 0, 0);
  lv_obj_clear_flag(rule, LV_OBJ_FLAG_SCROLLABLE);
}

lv_obj_t* makeButtonBase(lv_obj_t* parent, int16_t x, int16_t y, int16_t width,
                         int16_t height, lv_color_t accent, UiCommand command,
                         bool long_press = false) {
  lv_obj_t* button = lv_btn_create(parent);
  lv_obj_set_pos(button, x, y);
  lv_obj_set_size(button, width, height);
  lv_obj_set_style_bg_color(button, color(kPanel), 0);
  lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(button, color(kBorder), LV_STATE_PRESSED);
  lv_obj_set_style_border_color(button,
      accent.full == color(kRed).full ? accent : color(kBorder), 0);
  lv_obj_set_style_border_color(button, accent, LV_STATE_PRESSED);
  lv_obj_set_style_border_width(button, 1, 0);
  lv_obj_set_style_border_width(button, 2, LV_STATE_PRESSED);
  lv_obj_set_style_radius(button, 0, 0);
  lv_obj_set_style_shadow_width(button, 0, 0);
  lv_obj_set_style_pad_all(button, 0, 0);
  lv_obj_clear_flag(button, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(button, OvenUi::eventHandler,
                      long_press ? LV_EVENT_LONG_PRESSED : LV_EVENT_CLICKED,
                      reinterpret_cast<void*>(static_cast<uintptr_t>(command)));
  return button;
}

lv_obj_t* makeButton(lv_obj_t* parent, const char* text, int16_t x, int16_t y, int16_t width,
                     int16_t height, lv_color_t accent, UiCommand command,
                     bool long_press = false) {
  lv_obj_t* button = makeButtonBase(parent, x, y, width, height, accent, command, long_press);
  makeLabel(button, text, &lv_font_montserrat_16, accent, LV_ALIGN_CENTER, 0, 0);
  return button;
}

void makeProfileCard(lv_obj_t* parent, const char* name, const char* specification,
                     const char* badge, int16_t y, lv_color_t accent, UiCommand command) {
  lv_obj_t* button = makeButtonBase(parent, 12, y, 216, 76, accent, command);
  makeLabel(button, name, &lv_font_montserrat_20, accent, LV_ALIGN_TOP_LEFT, 10, 10);
  makeLabel(button, badge, &lv_font_montserrat_14, accent, LV_ALIGN_TOP_RIGHT, -10, 13);
  makeLabel(button, specification, &lv_font_montserrat_14, color(kMuted), LV_ALIGN_BOTTOM_LEFT,
            10, -11);
}

lv_obj_t* makeHeader(lv_obj_t* parent, const char* title, const char* status,
                     lv_color_t status_color) {
  makeLabel(parent, title, &lv_font_montserrat_16, color(kText), LV_ALIGN_TOP_LEFT, 12, 9);
  lv_obj_t* status_label =
      makeLabel(parent, status, &lv_font_montserrat_14, status_color, LV_ALIGN_TOP_RIGHT, -12, 11);
  makeRule(parent, 35);
  return status_label;
}

const char* recipeDisplayName(RecipeId id) {
  switch (id) {
    case RecipeId::LeadedReflow: return "SnPb REFLOW";
    case RecipeId::Sac305Reflow: return "SAC305 REFLOW";
    case RecipeId::Nylon6Anneal: return "NYLON-6 ANNEAL";
    case RecipeId::ChamberHold: return "CHAMBER HOLD";
    case RecipeId::Commission100: return "100 C HEATER TEST";
    case RecipeId::Commission150: return "150 C HEATER TEST";
    case RecipeId::Commission200: return "200 C HEATER TEST";
    case RecipeId::Autotune100: return "TUNE A / 100 C";
    case RecipeId::ValidateA: return "TEST A / 100 C";
    case RecipeId::ValidateB: return "TEST B / 100 C";
    case RecipeId::ValidateC: return "TEST C / 100 C";
  }
  return "THERMAL PROCESS";
}

const char* recipeHeaderName(RecipeId id) {
  switch (id) {
    case RecipeId::LeadedReflow: return "SnPb REFLOW";
    case RecipeId::Sac305Reflow: return "SAC305 REFLOW";
    case RecipeId::Nylon6Anneal: return "NYLON ANNEAL";
    case RecipeId::ChamberHold: return "CHAMBER HOLD";
    case RecipeId::Commission100: return "100 C TEST";
    case RecipeId::Commission150: return "150 C TEST";
    case RecipeId::Commission200: return "200 C TEST";
    case RecipeId::Autotune100: return "TUNE A";
    case RecipeId::ValidateA: return "TEST A";
    case RecipeId::ValidateB: return "TEST B";
    case RecipeId::ValidateC: return "TEST C";
  }
  return "THERMAL RUN";
}

float peakTarget(const Recipe& recipe) {
  float peak = 0.0F;
  for (uint8_t index = 0; index < recipe.phase_count; ++index) {
    peak = std::max(peak, recipe.phases[index].target_celsius);
  }
  return peak;
}

uint32_t timedHoldSeconds(const Recipe& recipe) {
  uint32_t total = 0;
  for (uint8_t index = 0; index < recipe.phase_count; ++index) {
    if (recipe.phases[index].kind == PhaseKind::Hold) {
      total += recipe.phases[index].duration_seconds;
    }
  }
  return total;
}

const char* faultGuidance(FaultCode fault) {
  switch (fault) {
    case FaultCode::ProcessProbe:
      return "PROCESS PROBE INVALID\nCheck plug and wiring.\nCheck polarity.\nRepair, then acknowledge.";
    case FaultCode::ProcessOverTemperature:
      return "PROCESS TOO HOT\nIsolate heater power.\nCheck probe position\nand the control circuit.";
    case FaultCode::HighTemperatureProfileNotCommissioned:
      return "SAC305 STILL LOCKED\nHardware/profile proof\nrequired before release.";
    case FaultCode::RunTimeout:
      return "RUN LIMIT REACHED\nCheck heat and probe.\nCheck cooling.\nDo not bypass timeout.";
    case FaultCode::TestStartTooHot:
      return "COOL BEFORE TESTING\nProcess must be\n60 C or cooler.\nThen acknowledge.";
    case FaultCode::TuneUnstable:
      return "NO RELIABLE CYCLES\nCheck contact and setup.\nOld PID unchanged.\nDo not raise the limits.";
    case FaultCode::NoTuneCandidate:
      return "TUNE A FIRST\nComplete tune/cooldown.\nThen test A, B and C.";
    case FaultCode::None:
      return "NO ACTIVE FAULT\nHeater output remains latched off.";
  }
  return "UNKNOWN FAULT\nHeater output remains latched off.";
}

}  // namespace

bool OvenUi::begin() {
  g_ui = this;
  pinMode(board::kTftBacklight, OUTPUT);
  digitalWrite(board::kTftBacklight, LOW);
  g_tft.begin();
  g_tft.setRotation(0);
  digitalWrite(board::kTftBacklight, LOW);
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
  showHome();
  return touch_ready;
}

void OvenUi::playBootAnimation() {
#ifdef TOASTER_BOOT_ANIMATION
  const size_t embedded_bytes = static_cast<size_t>(kBootFramesEnd - kBootFramesStart);
  const size_t expected_bytes = static_cast<size_t>(kBootFrameBytes) * kBootFrameCount;
  if (embedded_bytes != expected_bytes) {
    digitalWrite(board::kTftBacklight, HIGH);
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
      digitalWrite(board::kTftBacklight, HIGH);
    }
  }

  while (millis() - started_ms < kBootDurationMs) {
    delay(1);
  }
  g_tft.setSwapBytes(false);
  g_tft.setRotation(0);
  g_tft.fillScreen(TFT_BLACK);
#else
  g_tft.fillScreen(TFT_BLACK);
  digitalWrite(board::kTftBacklight, HIGH);
#endif
}

void OvenUi::tick(uint32_t now_ms) {
  const uint32_t elapsed = now_ms - last_lv_tick_ms_;
  if (elapsed > 0U) {
    lv_tick_inc(elapsed);
    last_lv_tick_ms_ = now_ms;
  }
  lv_timer_handler();
}

void OvenUi::update(const EngineSnapshot& snapshot, const PidStudy& study) {
  const bool study_changed = study.saved != study_.saved || study.save_failed != study_.save_failed;
  study_ = study;
  engine_state_ = snapshot.state;
  if (study_changed && screen_ == Screen::StudyResults) showStudyResults();
  probe_healthy_ = snapshot.probe_healthy;
  test_start_cool_ = probe_healthy_ && snapshot.process_celsius <= 60.0F;

  if (snapshot.state == EngineState::Fault && screen_ != Screen::Fault) {
    showFault();
  } else if ((snapshot.state == EngineState::Running || snapshot.state == EngineState::Cooling) &&
             screen_ != Screen::Run) {
    showRun();
  } else if ((snapshot.state == EngineState::Complete || snapshot.state == EngineState::Aborted) &&
             screen_ != Screen::Complete) {
    showComplete();
  } else if (screen_ == Screen::Confirm &&
             confirm_start_allowed_ != selectedRecipeCanStart()) {
    showConfirm(selected_recipe_);
  }

  refreshDynamic(snapshot);
}

UiCommand OvenUi::consumeCommand() {
  const UiCommand command = pending_command_;
  pending_command_ = UiCommand::None;
  switch (command) {
    case UiCommand::ShowReflowRecipes:
      showRecipeList();
      return UiCommand::None;
    case UiCommand::ShowStudy:
      showStudy();
      return UiCommand::None;
    case UiCommand::ShowStudyResults:
      showStudyResults();
      return UiCommand::None;
    case UiCommand::ChooseAutotune:
      showConfirm(RecipeId::Autotune100);
      return UiCommand::None;
    case UiCommand::ChooseValidateA:
      showConfirm(RecipeId::ValidateA);
      return UiCommand::None;
    case UiCommand::ChooseValidateB:
      showConfirm(RecipeId::ValidateB);
      return UiCommand::None;
    case UiCommand::ChooseValidateC:
      showConfirm(RecipeId::ValidateC);
      return UiCommand::None;
    case UiCommand::ShowCommissioning:
      showCommissioning();
      return UiCommand::None;
    case UiCommand::ChooseCommission100:
      showConfirm(RecipeId::Commission100);
      return UiCommand::None;
    case UiCommand::ChooseCommission150:
      showConfirm(RecipeId::Commission150);
      return UiCommand::None;
    case UiCommand::ChooseCommission200:
      showConfirm(RecipeId::Commission200);
      return UiCommand::None;
    case UiCommand::ChooseLeaded:
      showConfirm(RecipeId::LeadedReflow);
      return UiCommand::None;
    case UiCommand::ChooseSac305:
      showConfirm(RecipeId::Sac305Reflow);
      return UiCommand::None;
    case UiCommand::ChooseAnneal:
      showConfirm(RecipeId::Nylon6Anneal);
      return UiCommand::None;
    case UiCommand::ChooseHold:
      showConfirm(RecipeId::ChamberHold);
      return UiCommand::None;
    case UiCommand::StartSelected:
      return confirm_start_allowed_ ? command : UiCommand::None;
    case UiCommand::Home:
      showHome();
      return UiCommand::None;
    case UiCommand::Acknowledge:
      showHome();
      return command;
    default:
      return command;
  }
}

void OvenUi::prepareScreen(Screen screen) {
  screen_ = screen;
  title_ = nullptr;
  temperature_ = nullptr;
  target_ = nullptr;
  phase_ = nullptr;
  timer_ = nullptr;
  output_ = nullptr;
  health_ = nullptr;
  heater_ = nullptr;
  detail_ = nullptr;
  chart_ = nullptr;
  chart_process_ = nullptr;
  chart_target_ = nullptr;
  last_chart_ms_ = 0;

  lv_obj_t* root = lv_scr_act();
  lv_obj_clean(root);
  lv_obj_set_style_bg_color(root, color(kGraphite), 0);
  lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(root, 0, 0);
  lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
}

void OvenUi::showHome() {
  prepareScreen(Screen::Home);
  lv_obj_t* screen = lv_scr_act();

  health_ = makeHeader(screen, "THERMAL", "CHECKING", color(kMuted));
  lv_obj_t* telemetry = makePanel(screen, 12, 44, 216, 92);
  makeLabel(telemetry, "PROCESS", &lv_font_montserrat_14, color(kMuted), LV_ALIGN_TOP_LEFT, 8, 7);
  temperature_ =
      makeLabel(telemetry, "---.-", &lv_font_montserrat_32, color(kText), LV_ALIGN_TOP_LEFT, 8, 25);
  makeLabel(telemetry, "C", &lv_font_montserrat_20, color(kMuted), LV_ALIGN_TOP_LEFT, 169, 36);

  makeLabel(screen, "MODES", &lv_font_montserrat_14, color(kMuted), LV_ALIGN_TOP_LEFT, 12, 146);
  makeButton(screen, "REFLOW", 12, 165, 216, 44, color(kAmber), UiCommand::ShowReflowRecipes);
  makeButton(screen, "ANNEAL", 12, 217, 104, 44, color(kCyan), UiCommand::ChooseAnneal);
  makeButton(screen, "CHAMBER", 124, 217, 104, 44, color(kText), UiCommand::ChooseHold);

  makeButton(screen, "HEATER TESTS", 12, 269, 216, 39, color(kMuted),
             UiCommand::ShowCommissioning);
}

void OvenUi::showCommissioning() {
  prepareScreen(Screen::Commissioning);
  lv_obj_t* screen = lv_scr_act();
  makeHeader(screen, "TESTS", "SUPERVISED", color(kAmber));
  makeLabel(screen, "Start low. Review each result.", &lv_font_montserrat_14,
            color(kMuted), LV_ALIGN_TOP_LEFT, 12, 45);
  makeButton(screen, "100 C / 5 MIN HOLD", 12, 67, 216, 38, color(kAmber),
             UiCommand::ChooseCommission100);
  makeButton(screen, "150 C / 5 MIN HOLD", 12, 111, 216, 38, color(kAmber),
             UiCommand::ChooseCommission150);
  makeButton(screen, "200 C / 5 MIN HOLD", 12, 155, 216, 38, color(kAmber),
             UiCommand::ChooseCommission200);
  makeButton(screen, "PID / 3-POINT STUDY", 12, 199, 216, 34, color(kCyan), UiCommand::ShowStudy);
  makeLabel(screen, "25% max / 20 min run limit", &lv_font_montserrat_14,
            color(kMuted), LV_ALIGN_TOP_LEFT, 12, 239);
  makeButton(screen, "BACK", 12, 264, 216, 44, color(kText), UiCommand::Home);
}

void OvenUi::showRecipeList() {
  prepareScreen(Screen::Recipe);
  lv_obj_t* screen = lv_scr_act();

  makeHeader(screen, "REFLOW", "2 PROFILES", color(kMuted));
  makeProfileCard(screen, "SnPb", "PEAK 205 C  /  LIQ 183 C", "READY", 48,
                  color(kAmber), UiCommand::ChooseLeaded);
  makeProfileCard(screen, "SAC305", "PEAK 235 C  /  LIQ 217 C", "LOCKED", 132,
                  color(kMuted), UiCommand::ChooseSac305);

  lv_obj_t* note = makePanel(screen, 12, 216, 216, 39);
  makeLabel(note, "235 C PROFILE LOCKED", &lv_font_montserrat_14, color(kMuted),
            LV_ALIGN_CENTER, 0, 0);
  makeButton(screen, "BACK", 12, 264, 216, 44, color(kText), UiCommand::Home);
}

void OvenUi::showConfirm(RecipeId recipe_id) {
  selected_recipe_ = recipe_id;
  confirm_start_allowed_ = selectedRecipeCanStart();
  prepareScreen(Screen::Confirm);
  lv_obj_t* screen = lv_scr_act();
  const Recipe& recipe = recipeFor(recipe_id);
  const bool commissioned = !recipe.requires_high_temperature_commissioning &&
      (recipe.mode != RunMode::Validation || study_.candidate_ready);
  const bool test = recipe.mode == RunMode::Commissioning || isStudyRecipe(recipe_id);
  const char* status = !commissioned ? "LOCKED" : !probe_healthy_ ? "CHECK PROBE"
                      : test && !test_start_cool_ ? "COOL FIRST" : "READY";
  const lv_color_t status_color = confirm_start_allowed_ ? color(kCyan) : color(kRed);

  makeHeader(screen, "REVIEW", status, status_color);
  title_ = makeLabel(screen, recipeDisplayName(recipe_id), &lv_font_montserrat_20, color(kText),
                     LV_ALIGN_TOP_LEFT, 12, 48);

  lv_obj_t* profile = makePanel(screen, 12, 79, 216, 107);
  char value[24];
  std::snprintf(value, sizeof(value), "%.0f C", static_cast<double>(peakTarget(recipe)));
  makeLabel(profile, "TARGET", &lv_font_montserrat_14, color(kMuted), LV_ALIGN_TOP_LEFT, 8, 9);
  makeLabel(profile, value, &lv_font_montserrat_16, color(kText), LV_ALIGN_TOP_RIGHT, -8, 7);

  if (recipe.liquidus_celsius > 0.0F) {
    std::snprintf(value, sizeof(value), "%.0f C", static_cast<double>(recipe.liquidus_celsius));
    makeLabel(profile, "LIQUIDUS", &lv_font_montserrat_14, color(kMuted), LV_ALIGN_TOP_LEFT, 8, 33);
  } else {
    const uint32_t hold_minutes = timedHoldSeconds(recipe) / 60U;
    if (recipe.mode == RunMode::Autotune) std::snprintf(value, sizeof(value), "5-10 cycles");
    else std::snprintf(value, sizeof(value), "%lu min", static_cast<unsigned long>(hold_minutes));
    makeLabel(profile, recipe.mode == RunMode::Autotune ? "CYCLES" : "HOLD",
              &lv_font_montserrat_14, color(kMuted), LV_ALIGN_TOP_LEFT, 8, 33);
  }
  makeLabel(profile, value, &lv_font_montserrat_16, color(kText), LV_ALIGN_TOP_RIGHT, -8, 31);
  std::snprintf(value, sizeof(value), "%lu min", static_cast<unsigned long>(recipe.maximum_run_seconds / 60U));
  makeLabel(profile, "MAX RUN", &lv_font_montserrat_14, color(kMuted), LV_ALIGN_TOP_LEFT, 8, 57);
  makeLabel(profile, value, &lv_font_montserrat_16, color(kText), LV_ALIGN_TOP_RIGHT, -8, 55);
  std::snprintf(value, sizeof(value), "%.0f%%", static_cast<double>(recipe.maximum_output_percent));
  makeLabel(profile, "OUTPUT CAP", &lv_font_montserrat_14, color(kMuted), LV_ALIGN_TOP_LEFT, 8, 81);
  makeLabel(profile, value, &lv_font_montserrat_16, color(kText), LV_ALIGN_TOP_RIGHT, -8, 79);

  makeLabel(screen, test ? "SUPERVISE / START <=60 C" : "PROBE / HARD CUTOFF", &lv_font_montserrat_14,
            color(kMuted), LV_ALIGN_TOP_LEFT, 12, 196);

  const UiCommand back_command = isStudyRecipe(recipe_id) ? UiCommand::ShowStudy : test ? UiCommand::ShowCommissioning
      : recipe.mode == RunMode::Reflow ? UiCommand::ShowReflowRecipes : UiCommand::Home;
  if (confirm_start_allowed_) {
    makeButton(screen, "HOLD 2s TO START", 12, 215, 216, 49, color(kAmber),
               UiCommand::StartSelected, true);
  } else {
    lv_obj_t* blocked = makePanel(screen, 12, 215, 216, 49);
    const char* reason = !commissioned ? (recipe.mode == RunMode::Validation ? "LOCKED / TUNE A FIRST" : "LOCKED / COMMISSION") : !probe_healthy_
        ? "LOCKED / CHECK PROBE" : "LOCKED / COOL TO 60 C";
    makeLabel(blocked, reason, &lv_font_montserrat_14,
              commissioned ? color(kRed) : color(kMuted), LV_ALIGN_CENTER, 0, 0);
  }
  makeButton(screen, "BACK", 12, 270, 216, 42, color(kText), back_command);
}

void OvenUi::showRun() {
  prepareScreen(Screen::Run);
  lv_obj_t* screen = lv_scr_act();

  health_ = makeHeader(screen, recipeHeaderName(selected_recipe_), "HEATING", color(kAmber));
  temperature_ =
      makeLabel(screen, "---.-", &lv_font_montserrat_48, color(kText), LV_ALIGN_TOP_LEFT, 12, 42);
  makeLabel(screen, "C", &lv_font_montserrat_20, color(kMuted), LV_ALIGN_TOP_LEFT, 176, 77);
  target_ = makeLabel(screen, "SET  --.- C", &lv_font_montserrat_14, color(kAmber),
                      LV_ALIGN_TOP_LEFT, 14, 106);
  phase_ = makeLabel(screen, "1/1  RAMP", &lv_font_montserrat_14, color(kText),
                     LV_ALIGN_TOP_LEFT, 14, 130);
  timer_ = makeLabel(screen, "00:00", &lv_font_montserrat_14, color(kMuted),
                     LV_ALIGN_TOP_RIGHT, -14, 130);

  chart_ = lv_chart_create(screen);
  lv_obj_set_pos(chart_, 12, 151);
  lv_obj_set_size(chart_, 216, 72);
  lv_chart_set_type(chart_, LV_CHART_TYPE_LINE);
  lv_chart_set_update_mode(chart_, LV_CHART_UPDATE_MODE_SHIFT);
  lv_chart_set_point_count(chart_, 30);
  lv_chart_set_range(chart_, LV_CHART_AXIS_PRIMARY_Y, 0, 260);
  lv_chart_set_div_line_count(chart_, 4, 0);
  lv_obj_set_style_bg_color(chart_, color(kPanel), 0);
  lv_obj_set_style_bg_opa(chart_, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(chart_, color(kBorder), 0);
  lv_obj_set_style_border_width(chart_, 1, 0);
  lv_obj_set_style_radius(chart_, 0, 0);
  lv_obj_set_style_line_color(chart_, color(kBorder), LV_PART_MAIN);
  lv_obj_set_style_line_width(chart_, 1, LV_PART_MAIN);
  lv_obj_set_style_size(chart_, 0, LV_PART_INDICATOR);
  chart_process_ = lv_chart_add_series(chart_, color(kCyan), LV_CHART_AXIS_PRIMARY_Y);
  chart_target_ = lv_chart_add_series(chart_, color(kAmber), LV_CHART_AXIS_PRIMARY_Y);

  output_ = makeLabel(screen, "OUTPUT  --%", &lv_font_montserrat_16, color(kAmber),
                      LV_ALIGN_TOP_LEFT, 14, 234);
  heater_ = makeLabel(screen, "SSR OFF", &lv_font_montserrat_14, color(kMuted),
                      LV_ALIGN_TOP_RIGHT, -14, 237);
  makeButton(screen, "STOP", 12, 264, 216, 44, color(kRed), UiCommand::Stop);
}

void OvenUi::showFault() {
  prepareScreen(Screen::Fault);
  lv_obj_t* screen = lv_scr_act();

  health_ = makeHeader(screen, "FAULT", "HEATER OFF", color(kRed));
  title_ = makeWrappedLabel(screen, "UNKNOWN FAULT", &lv_font_montserrat_20, color(kRed), 216,
                            LV_ALIGN_TOP_LEFT, 12, 51);
  lv_obj_t* explanation = makePanel(screen, 12, 106, 216, 111);
  detail_ = makeWrappedLabel(explanation, "Heater output is latched off.",
                             &lv_font_montserrat_14, color(kText), 196, LV_ALIGN_TOP_LEFT, 10, 12);
  makeButton(screen, "ACKNOWLEDGE", 12, 244, 216, 52, color(kRed), UiCommand::Acknowledge);
  makeLabel(screen, "OUTPUT LATCHED LOW", &lv_font_montserrat_14, color(kMuted),
            LV_ALIGN_BOTTOM_MID, 0, -5);
}

void OvenUi::showComplete() {
  prepareScreen(Screen::Complete);
  lv_obj_t* screen = lv_scr_act();

  makeHeader(screen, "RUN RESULT", "HEATER OFF", color(kMuted));
  title_ = makeLabel(screen, "PROCESS COMPLETE", &lv_font_montserrat_20, color(kCyan),
                     LV_ALIGN_TOP_LEFT, 12, 56);
  temperature_ = makeLabel(screen, "---.- C", &lv_font_montserrat_32, color(kText),
                           LV_ALIGN_TOP_LEFT, 12, 104);
  detail_ = makeWrappedLabel(screen, "Wait for safe handling temperature.",
                             &lv_font_montserrat_14, color(kMuted), 216, LV_ALIGN_TOP_LEFT, 12, 158);
  makeButton(screen, "HOME", 12, 244, 216, 52, color(kText), UiCommand::Acknowledge);
  makeLabel(screen, "OUTPUT LATCHED LOW", &lv_font_montserrat_14, color(kMuted),
            LV_ALIGN_BOTTOM_MID, 0, -5);
}

void OvenUi::refreshDynamic(const EngineSnapshot& snapshot) {
  char buffer[96];
  if (temperature_ != nullptr) {
    if (snapshot.probe_healthy) {
      const char* suffix = screen_ == Screen::Complete ? " C" : "";
      std::snprintf(buffer, sizeof(buffer), "%.1f%s", static_cast<double>(snapshot.process_celsius),
                    suffix);
    } else {
      std::snprintf(buffer, sizeof(buffer), screen_ == Screen::Complete ? "---.- C" : "---.-");
    }
    lv_label_set_text(temperature_, buffer);
  }

  if (screen_ == Screen::Home) {
    if (health_ != nullptr) {
      lv_label_set_text(health_, snapshot.probe_healthy ? "READY" : "CHECK PROBE");
      lv_obj_set_style_text_color(health_, snapshot.probe_healthy ? color(kCyan) : color(kRed), 0);
    }
  }

  if (screen_ == Screen::Run) {
    if (health_ != nullptr) {
      const bool cooling = snapshot.state == EngineState::Cooling;
      lv_label_set_text(health_, cooling ? "COOLING" : "HEATING");
      lv_obj_set_style_text_color(health_, cooling ? color(kCyan) : color(kAmber), 0);
    }
    if (target_ != nullptr) {
      if (snapshot.state == EngineState::Cooling) {
        lv_label_set_text(target_, "SET  OFF");
      } else {
        std::snprintf(buffer, sizeof(buffer), "SET  %.1f C",
                      static_cast<double>(snapshot.target_celsius));
        lv_label_set_text(target_, buffer);
      }
    }
    if (phase_ != nullptr && snapshot.recipe != nullptr &&
        snapshot.phase_index < snapshot.recipe->phase_count) {
      const RecipePhase& phase = snapshot.recipe->phases[snapshot.phase_index];
      if (snapshot.recipe->mode == RunMode::Autotune && snapshot.state == EngineState::Running)
        std::snprintf(buffer, sizeof(buffer), "CYCLES %u/10", study_.cycles);
      else std::snprintf(buffer, sizeof(buffer), "%u/%u  %s", snapshot.phase_index + 1U,
                    static_cast<unsigned int>(snapshot.recipe->phase_count),
                    toString(phase.kind));
      lv_label_set_text(phase_, buffer);
    }
    if (timer_ != nullptr) {
      const uint32_t hours = snapshot.run_elapsed_seconds / 3600U;
      const uint32_t minutes = (snapshot.run_elapsed_seconds / 60U) % 60U;
      const uint32_t seconds = snapshot.run_elapsed_seconds % 60U;
      if (hours > 0U) {
        std::snprintf(buffer, sizeof(buffer), "%lu:%02lu:%02lu",
                      static_cast<unsigned long>(hours), static_cast<unsigned long>(minutes),
                      static_cast<unsigned long>(seconds));
      } else {
        std::snprintf(buffer, sizeof(buffer), "%02lu:%02lu",
                      static_cast<unsigned long>(minutes), static_cast<unsigned long>(seconds));
      }
      lv_label_set_text(timer_, buffer);
    }
    if (output_ != nullptr) {
      std::snprintf(buffer, sizeof(buffer), "OUTPUT  %3.0f%%",
                    static_cast<double>(snapshot.output_percent));
      lv_label_set_text(output_, buffer);
    }
    if (heater_ != nullptr) {
      lv_label_set_text(heater_, snapshot.heater_commanded_on ? "SSR ON" : "SSR OFF");
      lv_obj_set_style_text_color(heater_, snapshot.heater_commanded_on ? color(kAmber) : color(kMuted), 0);
    }

    const uint32_t now = millis();
    if (chart_ != nullptr && chart_process_ != nullptr && chart_target_ != nullptr &&
        now - last_chart_ms_ >= 1000U) {
      const lv_coord_t process =
          static_cast<lv_coord_t>(std::max(0.0F, snapshot.process_celsius));
      const lv_coord_t target =
          static_cast<lv_coord_t>(std::max(0.0F, snapshot.target_celsius));
      lv_chart_set_next_value(chart_, chart_process_, process);
      lv_chart_set_next_value(chart_, chart_target_, target);
      last_chart_ms_ = now;
    }
  }

  if (screen_ == Screen::Fault) {
    if (title_ != nullptr) {
      lv_label_set_text(title_, toString(snapshot.fault));
    }
    if (detail_ != nullptr) {
      lv_label_set_text(detail_, faultGuidance(snapshot.fault));
    }
  }

  if (screen_ == Screen::Complete) {
    const bool aborted = snapshot.state == EngineState::Aborted;
    if (title_ != nullptr) {
      lv_label_set_text(title_, aborted ? "RUN ABORTED" : "PROCESS COMPLETE");
      lv_obj_set_style_text_color(title_, aborted ? color(kAmber) : color(kCyan), 0);
    }
    if (detail_ != nullptr) {
      if (aborted) {
        lv_label_set_text(detail_, "Stopped by operator.\nHeater output is latched off.");
      } else if (snapshot.recipe != nullptr) {
        std::snprintf(buffer, sizeof(buffer), isStudyRecipe(snapshot.recipe->id)
                      ? "%s\nResults in PID study.\nCheck board before moving."
                      : "%s\nWait for safe handling temperature.",
                      recipeDisplayName(snapshot.recipe->id));
        lv_label_set_text(detail_, buffer);
      }
    }
  }
}

bool OvenUi::selectedRecipeCanStart() const {
  const Recipe& recipe = recipeFor(selected_recipe_);
  return probe_healthy_ && !recipe.requires_high_temperature_commissioning &&
         (recipe.mode != RunMode::Validation || study_.candidate_ready) &&
         ((recipe.mode != RunMode::Commissioning && !isStudyRecipe(selected_recipe_)) || test_start_cool_);
}

void OvenUi::queue(UiCommand command) { pending_command_ = command; }

void OvenUi::displayFlush(lv_disp_drv_t* display, const lv_area_t* area,
                          lv_color_t* color_buffer) {
  const uint32_t width = static_cast<uint32_t>(area->x2 - area->x1 + 1);
  const uint32_t height = static_cast<uint32_t>(area->y2 - area->y1 + 1);
  g_tft.startWrite();
  g_tft.setAddrWindow(area->x1, area->y1, width, height);
  g_tft.pushColors(reinterpret_cast<uint16_t*>(color_buffer), width * height, true);
  g_tft.endWrite();
  lv_disp_flush_ready(display);
}

void OvenUi::touchRead(lv_indev_drv_t*, lv_indev_data_t* data) {
  TouchPoint point;
  if (g_touch.read(point)) {
    data->point.x = point.x;
    data->point.y = point.y;
    data->state = LV_INDEV_STATE_PR;
  } else {
    data->state = LV_INDEV_STATE_REL;
  }
}

void OvenUi::eventHandler(lv_event_t* event) {
  if (g_ui == nullptr) {
    return;
  }
  const auto command =
      static_cast<UiCommand>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
  g_ui->queue(command);
}

void OvenUi::showStudy() {
  prepareScreen(Screen::Study);
  auto* screen = lv_scr_act();
  makeHeader(screen, "PID STUDY", "100 C", color(kCyan));
  makeWrappedLabel(screen, "Tune at A. Test the same PID\nat A, B and C. Cool each run.", &lv_font_montserrat_14,
                   color(kMuted), 216, LV_ALIGN_TOP_LEFT, 12, 44);
  makeButton(screen, "TUNE A / NEW STUDY", 12, 87, 216, 42, color(kAmber), UiCommand::ChooseAutotune);
  makeLabel(screen, study_.saved ? "PID SAVED / TESTS AVAILABLE" : study_.candidate_ready
            ? "CANDIDATE / NOT YET SAVED" : "TESTS LOCKED UNTIL TUNED",
            &lv_font_montserrat_14, color(kMuted), LV_ALIGN_TOP_LEFT, 12, 140);
  makeButton(screen, "TEST A", 12, 164, 68, 44, color(kCyan), UiCommand::ChooseValidateA);
  makeButton(screen, "TEST B", 86, 164, 68, 44, color(kCyan), UiCommand::ChooseValidateB);
  makeButton(screen, "TEST C", 160, 164, 68, 44, color(kCyan), UiCommand::ChooseValidateC);
  makeButton(screen, "RESULTS / SAVE PID", 12, 217, 216, 39, color(kText), UiCommand::ShowStudyResults);
  makeButton(screen, "BACK", 12, 269, 216, 39, color(kMuted), UiCommand::ShowCommissioning);
}

void OvenUi::showStudyResults() {
  prepareScreen(Screen::StudyResults);
  auto* screen = lv_scr_act();
  makeHeader(screen, "STUDY RESULTS", "100 C", color(kCyan));
  char text[192];
  const auto& gains = study_.candidate_ready ? study_.candidate : study_.active;
  std::snprintf(text, sizeof(text), "%s\nP %.3f   I %.5f\nD %.2f",
                study_.candidate_ready ? "CANDIDATE PID" : "ACTIVE PID",
                static_cast<double>(gains.kp), static_cast<double>(gains.ki), static_cast<double>(gains.kd));
  makeWrappedLabel(screen, text, &lv_font_montserrat_14, color(kText), 216, LV_ALIGN_TOP_LEFT, 12, 44);
  bool complete = study_.candidate_ready;
  for (unsigned i = 0; i < 3; ++i) {
    const auto& r = study_.points[i];
    const char* status = r.result == StudyResult::Complete ? "DONE" :
        r.result == StudyResult::Failed ? "FAILED" : r.result == StudyResult::Aborted ? "STOPPED" : "NO RUN";
    complete = complete && r.result == StudyResult::Complete;
    std::snprintf(text, sizeof(text), "%c %s  #%lu  peak %.1f C\n96 C: %lus  hold RMS %.2f C",
                  'A' + i, status, static_cast<unsigned long>(r.attempts), static_cast<double>(r.peak_celsius),
                  static_cast<unsigned long>(r.rise_seconds), static_cast<double>(r.hold_rmse));
    makeWrappedLabel(screen, text, &lv_font_montserrat_14, color(kMuted), 216,
                     LV_ALIGN_TOP_LEFT, 12, 103 + i * 42);
  }
  if (study_.save_failed) {
    makeLabel(screen, "SAVE FAILED / OLD PID KEPT", &lv_font_montserrat_14, color(kRed),
              LV_ALIGN_TOP_LEFT, 12, 233);
  } else if (study_.saved) {
    makeLabel(screen, "PID SAVED / 100 C STUDY", &lv_font_montserrat_14, color(kCyan),
              LV_ALIGN_TOP_LEFT, 12, 233);
  } else if (complete && engine_state_ == EngineState::Idle) {
    makeButton(screen, "HOLD 2s: SAVE + USE", 12, 231, 216, 33, color(kAmber), UiCommand::SaveStudy, true);
  } else {
    makeLabel(screen, "COMPLETE A/B/C TO SAVE", &lv_font_montserrat_14, color(kMuted),
              LV_ALIGN_TOP_LEFT, 12, 233);
  }
  makeButton(screen, "BACK", 12, 274, 216, 34, color(kText), UiCommand::ShowStudy);
}
