#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <cmath>
#include <limits>
#include "ui_theme.h"

#define private public
#include "ui.h"
#undef private
#include "touch_ft6336.h"

uint32_t host_millis = 0;
int host_backlight_level = 1;
uint8_t host_ledc_channel = 0;
uint8_t host_ledc_pin = 0;
uint32_t host_ledc_frequency = 0;
uint8_t host_ledc_resolution = 0;
uint32_t host_ledc_duty = 0;
uint16_t host_pixels[240 * 320] = {};
bool Ft6336Touch::begin() { return true; }
bool host_touch_down = false;
TouchPoint host_touch_point = {};
bool Ft6336Touch::read(TouchPoint& point) { point = host_touch_point; return host_touch_down; }

// A one-byte stand-in skips animation; boot video has separate asset validation.
asm(".globl _binary_assets_boot_history_hot_240x135_12_5fps_rgb565_start\n"
    "_binary_assets_boot_history_hot_240x135_12_5fps_rgb565_start:\n.byte 0\n"
    ".globl _binary_assets_boot_history_hot_240x135_12_5fps_rgb565_end\n"
    "_binary_assets_boot_history_hot_240x135_12_5fps_rgb565_end:\n");

void checkBounds(lv_obj_t* parent) {
  for (uint32_t i = 0; i < lv_obj_get_child_cnt(parent); ++i) {
    auto* child = lv_obj_get_child(parent, i);
    lv_area_t box;
    lv_obj_get_coords(child, &box);
    if (lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN)) continue;
    if (lv_obj_check_type(child, &lv_btn_class)) {
      assert(lv_obj_get_width(child) >= 40 && lv_obj_get_height(child) >= 40);
      assert(box.x1 >= 0 && box.y1 >= 0 && box.x2 < 240 && box.y2 < 320);
    }
    if (lv_obj_check_type(child, &lv_label_class)) {
      lv_area_t parent_box;
      lv_obj_get_coords(parent, &parent_box);
      if (box.x1 < 0 || box.y1 < 0 || box.x2 >= 240 || box.y2 >= 320 ||
          box.x1 < parent_box.x1 || box.y1 < parent_box.y1 ||
          box.x2 > parent_box.x2 || box.y2 > parent_box.y2) {
        std::fprintf(stderr, "Label out of bounds: %s (%d,%d)-(%d,%d)\n",
                     lv_label_get_text(child), box.x1, box.y1, box.x2, box.y2);
        assert(false);
      }
      for (uint32_t j = 0; j < i; ++j) {
        auto* other = lv_obj_get_child(parent, j);
        if (!lv_obj_check_type(other, &lv_label_class)) continue;
        lv_area_t other_box;
        lv_obj_get_coords(other, &other_box);
        const bool overlap = box.x1 <= other_box.x2 && box.x2 >= other_box.x1 &&
                             box.y1 <= other_box.y2 && box.y2 >= other_box.y1;
        if (overlap) {
          std::fprintf(stderr, "Overlapping labels: %s (%d,%d)-(%d,%d) / %s (%d,%d)-(%d,%d)\n",
                       lv_label_get_text(child), box.x1, box.y1, box.x2, box.y2,
                       lv_label_get_text(other), other_box.x1, other_box.y1, other_box.x2, other_box.y2);
          assert(false);
        }
      }
    }
    checkBounds(child);
  }
}

unsigned capture_count = 0;
uint32_t minimum_free = UINT32_MAX;
uint32_t minimum_largest = UINT32_MAX;
void capture(const char* directory, const char* name) {
  ++capture_count;
  lv_obj_update_layout(lv_scr_act());
  lv_refr_now(nullptr);
  checkBounds(lv_scr_act());
  assert(lv_mem_test() == LV_RES_OK);
  lv_mem_monitor_t memory; lv_mem_monitor(&memory);
  minimum_free = std::min(minimum_free, memory.free_size);
  minimum_largest = std::min(minimum_largest, memory.free_biggest_size);
  const std::string path = std::string(directory) + "/" + name + ".ppm";
  FILE* output = std::fopen(path.c_str(), "wb");
  assert(output);
  std::fprintf(output, "P6\n240 320\n255\n");
  for (uint16_t pixel : host_pixels) {
    const unsigned char rgb[] = {static_cast<unsigned char>(((pixel >> 11) & 31) * 255 / 31),
                                 static_cast<unsigned char>(((pixel >> 5) & 63) * 255 / 63),
                                 static_cast<unsigned char>((pixel & 31) * 255 / 31)};
    assert(std::fwrite(rgb, 1, 3, output) == 3);
  }
  std::fclose(output);
}

lv_obj_t* findButton(const char* text) {
  auto* root = lv_scr_act();
  for (uint32_t i = 0; i < lv_obj_get_child_cnt(root); ++i) {
    auto* o = lv_obj_get_child(root, i);
    if (!lv_obj_check_type(o, &lv_btn_class)) continue;
    auto* t = lv_obj_get_child(o, 0);
    if (t && lv_obj_check_type(t, &lv_label_class) && std::strcmp(lv_label_get_text(t), text) == 0) return o;
  }
  return nullptr;
}
lv_obj_t* findLabelContaining(lv_obj_t* parent, const char* text) {
  for (uint32_t i = 0; i < lv_obj_get_child_cnt(parent); ++i) {
    auto* child = lv_obj_get_child(parent, i);
    if (lv_obj_check_type(child, &lv_label_class) &&
        std::strstr(lv_label_get_text(child), text)) return child;
    if (auto* match = findLabelContaining(child, text)) return match;
  }
  return nullptr;
}
void send(OvenUi& ui, UiCommand cmd) { ui.queue(cmd); assert(ui.consumeCommand() == UiCommand::None); }
void advance(OvenUi& ui, unsigned ms) {
  for (unsigned n = 0; n < ms; n += 10) { host_millis += 10; ui.tick(host_millis); }
}
void press(OvenUi& ui, lv_obj_t* button) {
  assert(button);
  lv_obj_update_layout(lv_scr_act());
  lv_area_t a; lv_obj_get_coords(button, &a);
  host_touch_point.x = (a.x1 + a.x2) / 2;
  host_touch_point.y = (a.y1 + a.y2) / 2;
  host_touch_down = true; advance(ui, 40);
}
void release(OvenUi& ui) { host_touch_down = false; advance(ui, 40); }
void touchPoint(OvenUi& ui, int x, int y, unsigned held_ms = 100) {
  host_touch_point.x = static_cast<int16_t>(x);
  host_touch_point.y = static_cast<int16_t>(y);
  host_touch_down = true;
  advance(ui, held_ms);
  release(ui);
}
void turnDial(OvenUi& ui, lv_obj_t* dial, int value, int minimum, int maximum) {
  assert(dial && value >= minimum && value <= maximum);
  lv_obj_update_layout(lv_scr_act());
  lv_area_t area;
  lv_obj_get_coords(dial, &area);
  const double fraction = static_cast<double>(value - minimum) /
                          static_cast<double>(maximum - minimum);
  const double radians = (135.0 + fraction * 270.0) * 3.14159265358979323846 / 180.0;
  const int cx = area.x1 + lv_obj_get_width(dial) / 2;
  const int cy = area.y1 + lv_obj_get_height(dial) / 2;
  lv_point_t point = {static_cast<lv_coord_t>(
                          cx + static_cast<int>(std::lround(std::cos(radians) * 35.0))),
                      static_cast<lv_coord_t>(
                          cy + static_cast<int>(std::lround(std::sin(radians) * 35.0)))};
  assert(lv_obj_hit_test(dial, &point));
  touchPoint(ui, point.x, point.y);
}
void tap(OvenUi& ui, const char* text) {
  press(ui, findButton(text));
  release(ui);
  assert(ui.consumeCommand() == UiCommand::None);
}

int main(int argc, char** argv) {
  assert(argc == 2);
  const char* out = argv[1];
  OvenUi ui; assert(ui.begin(55));
  assert(ui.brightnessPercent() == 55);
  assert(host_ledc_channel == 7);
  assert(host_ledc_pin == 45);
  assert(host_ledc_frequency == 5000);
  assert(host_ledc_resolution == 8);
  assert(host_ledc_duty == 55U * 255U / 100U);
  EngineSnapshot s;
  PidStudy study;
  study.setup_revision = 1;
  ProfileValidationView profiles[4];
  for (auto& profile : profiles) profile.report.status = ValidationStatus::CriteriaMissing;
  profiles[2].report.profile = ValidationProfile::Anneal;
  ui.setProfileValidation(profiles);
  s.probe_healthy = true; s.process_celsius = 25;
  ui.update(s, study);
  ui.setElectronicsTemperatures(26.0F, 41.0F);
  assert(findLabelContaining(lv_scr_act(), "MAX 26°C / CPU 41°C"));
  capture(out, "01-home");
  // Proposed home trend treatment: synthetic render fixtures, not live telemetry.
  lv_label_set_text(ui.health_, LV_SYMBOL_UP " 2.4°C/min");
  capture(out, "01-home-trend-rising");
  lv_label_set_text(ui.health_, LV_SYMBOL_DOWN " 0.8°C/min");
  capture(out, "01-home-trend-falling");
  lv_label_set_text(ui.health_, "STABLE");
  capture(out, "01-home-trend-stable");
  ui.refreshDynamic(s);
  ui.setElectronicsTemperatures(NAN, NAN);
  assert(findLabelContaining(lv_scr_act(), "MAX --°C / CPU --°C"));
  ui.setElectronicsTemperatures(26.0F, 41.0F);
  assert(ui.brightness_button_);
  assert(lv_obj_get_width(ui.brightness_button_) >= 40);
  assert(lv_obj_get_height(ui.brightness_button_) >= 40);
  assert(!findLabelContaining(ui.brightness_button_, "%"));
  // Short tap: neither adjustment nor persistence request.
  press(ui, ui.brightness_button_); advance(ui, 250); release(ui);
  assert(ui.brightnessPercent() == 55);
  assert(!ui.consumeBrightnessChange());
  // Real LVGL pointer path: 40 px upward raises 55% to 87%.
  press(ui, ui.brightness_button_); advance(ui, 310);
  host_touch_point.y -= 40; advance(ui, 20);
  assert(ui.brightnessPercent() == 87);
  release(ui);
  assert(ui.consumeBrightnessChange());
  assert(!ui.consumeBrightnessChange());
  // Relative downward drag dims and clamps at 5%. One completed gesture -> one save.
  press(ui, ui.brightness_button_); advance(ui, 310);
  host_touch_point.y += 120; advance(ui, 20);
  assert(ui.brightnessPercent() == 5);
  assert(host_ledc_duty == 5U * 255U / 100U);
  release(ui);
  assert(ui.consumeBrightnessChange());
  assert(!ui.consumeBrightnessChange());
  // Upward direction brightens and clamps at 100%.
  ui.touch_y_ = 180;
  lv_event_send(ui.brightness_button_, LV_EVENT_PRESSED, nullptr);
  ui.touch_y_ = 40; ui.physical_touch_down_ = true;
  host_millis += 310; ui.updateBrightnessGesture(host_millis);
  assert(ui.brightnessPercent() == 100);
  lv_event_send(ui.brightness_button_, LV_EVENT_RELEASED, nullptr);
  ui.physical_touch_down_ = false;
  assert(ui.consumeBrightnessChange());
  assert(!ui.consumeBrightnessChange());
  // Screen replacement cancels an active adjustment and restores its starting value.
  ui.touch_y_ = 80; ui.physical_touch_down_ = true;
  lv_event_send(ui.brightness_button_, LV_EVENT_PRESSED, nullptr);
  ui.touch_y_ = 140; host_millis += 310; ui.updateBrightnessGesture(host_millis);
  assert(ui.brightnessPercent() < 100);
  ui.showCommissioning();
  ui.physical_touch_down_ = false;
  assert(ui.brightnessPercent() == 100);
  assert(!ui.consumeBrightnessChange());
  ui.showHome(); ui.refreshDynamic(s); capture(out, "01-home-brightness");
  // Unit is placed relative to the measured number, not stale layout coordinates.
  assert(lv_obj_get_y(ui.unit_) > lv_obj_get_y(ui.temperature_));
  assert(lv_obj_get_x(ui.unit_) > lv_obj_get_x(ui.temperature_));
  assert(!findButton("CHAMBER"));
  const uint16_t masks[] = {thermal_ui::ReflowMask, thermal_ui::AnnealMask,
                            thermal_ui::TestMask};
  const uint32_t accents[] = {thermal_ui::Reflow, thermal_ui::Anneal, thermal_ui::Test};
  const unsigned mode_y[] = {144, 196, 248};
  for (unsigned i = 0; i < 3; ++i)
    for (unsigned r = 0; r < 3; ++r)
      for (unsigned c = 0; c < 3; ++c) {
        const uint16_t expected = lv_color_hex(thermal_ui::occupied(masks[i], r, c) ? thermal_ui::Ink : accents[i]).full;
        assert(host_pixels[(mode_y[i] + 8 + r * 8 + 4) * 240 + 16 + c * 8 + 4] == expected);
      }
  for (auto mask : masks) {
    assert(mask < 512);
    for (unsigned r = 0; r < 3; ++r) {
      bool row = false, col = false;
      for (unsigned c = 0; c < 3; ++c) {
        row |= thermal_ui::occupied(mask, r, c);
        col |= thermal_ui::occupied(mask, c, r);
      }
      assert(row && col);
    }
  }

  // Anneal starts blank. Ring interaction, including an explicit minimum touch,
  // is the only way a field becomes set.
  tap(ui, "ANNEAL");
  assert(ui.screen_ == OvenUi::Screen::AnnealEditor);
  assert(ui.selectedRecipe() == RecipeId::CustomAnneal);
  assert(ui.annealProgram().target_celsius == 0.0F);
  assert(ui.annealProgram().soak_seconds == 0U);
  assert(ui.annealProgram().ramp_celsius_per_minute == 0.0F);
  assert(findButton("REVIEW") == ui.anneal_review_);
  assert(lv_obj_has_state(ui.anneal_review_, LV_STATE_DISABLED));
  lv_obj_update_layout(lv_scr_act());
  for (unsigned i = 0; i < 3; ++i) {
    assert(ui.anneal_dials_[i]);
    assert(lv_obj_check_type(ui.anneal_dials_[i], &lv_arc_class));
    assert(lv_obj_get_width(ui.anneal_dials_[i]) >= 40);
    assert(lv_obj_get_height(ui.anneal_dials_[i]) >= 40);
    assert(lv_obj_has_flag(ui.anneal_dials_[i], LV_OBJ_FLAG_ADV_HITTEST));
    assert(std::strcmp(lv_label_get_text(ui.anneal_values_[i]), "--") == 0);
  }
  capture(out, "01-anneal-editor-blank");

  // Arc advanced hit testing rejects the centre; a ring touch at min is explicit.
  lv_area_t temp_area; lv_obj_get_coords(ui.anneal_dials_[0], &temp_area);
  touchPoint(ui, (temp_area.x1 + temp_area.x2) / 2, (temp_area.y1 + temp_area.y2) / 2);
  assert(!ui.anneal_temperature_set_);
  turnDial(ui, ui.anneal_dials_[0], 60, 60, 180);
  assert(ui.anneal_temperature_set_ && ui.annealProgram().target_celsius == 60.0F);
  turnDial(ui, ui.anneal_dials_[0], 180, 60, 180);
  assert(ui.annealProgram().target_celsius == 180.0F);
  turnDial(ui, ui.anneal_dials_[1], 110, 1, 110);
  assert(ui.anneal_soak_set_ && ui.annealProgram().soak_seconds == 6600U);
  turnDial(ui, ui.anneal_dials_[2], 60, 1, 60);
  assert(ui.anneal_ramp_set_ && ui.annealProgram().ramp_celsius_per_minute == 60.0F);
  turnDial(ui, ui.anneal_dials_[1], 1, 1, 110);
  turnDial(ui, ui.anneal_dials_[2], 1, 1, 60);
  assert(ui.annealProgram().soak_seconds == 60U);
  assert(ui.annealProgram().ramp_celsius_per_minute == 1.0F);
  assert(lv_obj_has_state(ui.anneal_review_, LV_STATE_DISABLED));
  assert(std::strcmp(lv_label_get_text(ui.anneal_total_), ">=120m") == 0);

  // Configure a valid program through real pointer gestures.
  turnDial(ui, ui.anneal_dials_[0], 120, 60, 180);
  turnDial(ui, ui.anneal_dials_[1], 30, 1, 110);
  turnDial(ui, ui.anneal_dials_[2], 5, 1, 60);
  assert(!lv_obj_has_state(ui.anneal_review_, LV_STATE_DISABLED));
  assert(ui.annealProgram().target_celsius == 120.0F);
  assert(ui.annealProgram().soak_seconds == 1800U);
  assert(ui.annealProgram().ramp_celsius_per_minute == 5.0F);
  assert(std::strcmp(lv_label_get_text(ui.anneal_up_), "~19m") == 0);
  assert(std::strcmp(lv_label_get_text(ui.anneal_down_), "~12m") == 0);
  assert(std::strcmp(lv_label_get_text(ui.anneal_total_), "~61m") == 0);
  capture(out, "01-anneal-editor-configured");

  // Live eligibility/ETA refresh never replaces a dial object, including mid-touch.
  auto* stable_dial = ui.anneal_dials_[0];
  temp_area = {}; lv_obj_get_coords(stable_dial, &temp_area);
  host_touch_point.x = (temp_area.x1 + temp_area.x2) / 2;
  host_touch_point.y = temp_area.y1 + 5;
  host_touch_down = true; advance(ui, 40);
  s.process_celsius = 30.0F; ui.update(s, study);
  assert(ui.anneal_dials_[0] == stable_dial);
  release(ui);
  s.probe_healthy = false; ui.update(s, study);
  assert(lv_obj_has_state(ui.anneal_review_, LV_STATE_DISABLED));
  assert(std::strcmp(lv_label_get_text(ui.anneal_up_), "--") == 0);
  assert(std::strcmp(lv_label_get_text(ui.anneal_total_), "PROBE") == 0);
  s.probe_healthy = true; s.process_celsius = 121.0F; ui.update(s, study);
  assert(lv_obj_has_state(ui.anneal_review_, LV_STATE_DISABLED));
  assert(std::strcmp(lv_label_get_text(ui.anneal_up_), "--") == 0);
  assert(std::strcmp(lv_label_get_text(ui.anneal_total_), "TOO HOT") == 0);
  s.process_celsius = 25.0F; ui.update(s, study);
  assert(!lv_obj_has_state(ui.anneal_review_, LV_STATE_DISABLED));
  tap(ui, "REVIEW");
  assert(ui.screen_ == OvenUi::Screen::AnnealReview);
  assert(findLabelContaining(lv_scr_act(), "ends at 60°C"));
  capture(out, "01-anneal-review");
  tap(ui, "BACK");
  assert(ui.screen_ == OvenUi::Screen::AnnealEditor);
  assert(lv_arc_get_value(ui.anneal_dials_[0]) == 120);
  assert(lv_arc_get_value(ui.anneal_dials_[1]) == 30);
  assert(lv_arc_get_value(ui.anneal_dials_[2]) == 5);
  assert(std::strcmp(lv_label_get_text(ui.anneal_values_[0]), "120°C") == 0);
  tap(ui, "REVIEW");
  press(ui, findButton("HOLD 2s TO START")); advance(ui, 2050);
  assert(ui.consumeCommand() == UiCommand::StartCustomAnneal);
  release(ui);
  assert(ui.consumeCommand() == UiCommand::None);
  send(ui, UiCommand::Home);

  s.probe_healthy = false; ui.update(s, study); capture(out, "02-home-probe-invalid");
  assert(std::strcmp(lv_label_get_text(ui.temperature_), "--.-") == 0);
  s.probe_healthy = true; s.process_celsius = -10.5F;
  ui.update(s, study); capture(out, "03-home-negative-reading");
  s.process_celsius = 200.0F; ui.update(s, study); capture(out, "03-home-wide-reading");
  s.process_celsius = std::numeric_limits<float>::quiet_NaN();
  ui.update(s, study); assert(!ui.probe_healthy_);
  assert(std::strcmp(lv_label_get_text(ui.temperature_), "--.-") == 0);
  s.process_celsius = 25; ui.update(s, study);
  send(ui, UiCommand::ShowReflowRecipes); capture(out, "04-reflow-profiles");
  send(ui, UiCommand::ShowCommissioning); capture(out, "05-heater-tests");

  // Review every real recipe, not extrapolated controls from a concept image.
  const RecipeId ids[] = {RecipeId::LeadedReflow, RecipeId::Sac305Reflow,
    RecipeId::Nylon6Anneal, RecipeId::ChamberHold, RecipeId::Commission100,
    RecipeId::Commission150, RecipeId::Commission200, RecipeId::Autotune100,
    RecipeId::Check100, RecipeId::Check150, RecipeId::Check200};
  for (unsigned i = 0; i < sizeof(ids) / sizeof(ids[0]); ++i) {
    ui.showConfirm(ids[i]); ui.refreshDynamic(s);
    char name[80]; std::snprintf(name, sizeof(name), "10-review-%02u", i);
    capture(out, name);
    const bool locked = ids[i] == RecipeId::Sac305Reflow || recipeFor(ids[i]).mode == RunMode::Validation ||
                        ids[i] == RecipeId::Commission150 || ids[i] == RecipeId::Commission200;
    assert(ui.confirm_start_allowed_ == !locked);
  }

  // Profile evidence is distinct from controller scope and engine completion.
  ui.showConfirm(RecipeId::LeadedReflow);
  send(ui, UiCommand::ShowProfileResults); capture(out, "20-profile-specs-missing");
  assert(ui.screen_ == OvenUi::Screen::ProfileResults);
  assert(!findButton("HOLD 2s: COMMISSION"));
  profiles[0].report.status = ValidationStatus::Pass;
  profiles[0].report.profile = ValidationProfile::Reflow;
  profiles[0].report.metrics.peak_measured = true;
  profiles[0].report.metrics.peak_celsius = 183.5F;
  profiles[0].report.liquidus.measured = true;
  profiles[0].report.liquidus.value = 33.5F;
  profiles[0].report.metrics.liquidus_accumulated_ms = 42000;
  profiles[0].consecutive_passes = ValidationSequence::kRequiredPasses;
  profiles[0].eligible = true;
  ui.setProfileValidation(profiles); ui.update(s, study);
  capture(out, "20-profile-eligible");
  assert(findLabelContaining(lv_scr_act(), "33.5s"));
  s.state = EngineState::Complete; s.recipe = &recipeFor(RecipeId::LeadedReflow);
  ui.update(s, study); capture(out, "20-profile-complete-pass");
  assert(findLabelContaining(lv_scr_act(), "PROFILE: PASS"));
  profiles[0].report.status = ValidationStatus::Fail;
  ui.setProfileValidation(profiles); ui.update(s, study);
  capture(out, "20-profile-complete-late-fail");
  assert(findLabelContaining(lv_scr_act(), "PROFILE: FAIL"));
  profiles[0].report.status = ValidationStatus::Pass;
  ui.setProfileValidation(profiles); ui.update(s, study);
  s.state = EngineState::Idle; ui.update(s, study); ui.showProfileResults();
  press(ui, findButton("HOLD 2s: COMMISSION")); advance(ui, 500);
  auto* commission_hold = ui.held_button_;
  ui.setProfileValidation(profiles); ui.update(s, study);
  assert(ui.held_button_ == commission_hold);
  advance(ui, 1550);
  assert(ui.consumeCommand() == UiCommand::CommissionProfile); release(ui);
  profiles[0].eligible = false; profiles[0].commissioned = true;
  ui.setProfileValidation(profiles); ui.update(s, study);
  capture(out, "20-profile-commissioned");
  assert(!findButton("HOLD 2s: COMMISSION"));
  profiles[0].commissioned = false; profiles[0].needs_revalidation = true;
  ui.setProfileValidation(profiles); ui.update(s, study);
  capture(out, "20-profile-needs-revalidation");
  assert(findLabelContaining(lv_scr_act(), "REVALIDATE"));
  send(ui, UiCommand::DetailsBack);
  ui.showConfirm(RecipeId::Commission100);
  s.process_celsius = 60; ui.update(s, study); assert(ui.confirm_start_allowed_);
  s.process_celsius = 60.01F; ui.update(s, study); assert(!ui.confirm_start_allowed_);
  capture(out, "21-review-too-hot");
  send(ui, UiCommand::StartSelected);
  s.process_celsius = 25; s.probe_healthy = false;
  ui.update(s, study); capture(out, "22-review-probe-blocked");
  assert(!ui.confirm_start_allowed_);
  s.probe_healthy = true; ui.update(s, study);

  // Real LVGL input driver: a short tap cannot start; 2 s hold can.
  press(ui, findButton("HOLD 2s TO START")); advance(ui, 1200);
  capture(out, "23-start-hold-progress");
  assert(ui.consumeCommand() == UiCommand::None);
  release(ui); assert(ui.consumeCommand() == UiCommand::None);
  press(ui, findButton("HOLD 2s TO START")); advance(ui, 2050);
  assert(ui.consumeCommand() == UiCommand::StartSelected); release(ui);
  assert(ui.consumeCommand() == UiCommand::None);
  // Sliding off the control cancels an in-progress hold.
  press(ui, findButton("HOLD 2s TO START")); advance(ui, 900);
  host_touch_point.x = 2; host_touch_point.y = 2; advance(ui, 1300);
  assert(ui.consumeCommand() == UiCommand::None); release(ui);
  // A changing interlock cancels a queued start and its visual progress.
  ui.queue(UiCommand::StartSelected); s.probe_healthy = false; ui.update(s, study);
  assert(ui.consumeCommand() == UiCommand::None);
  s.probe_healthy = true; ui.update(s, study);

  // Distinct running/hold/cooling screens; chart points are supplied synthetic
  // snapshots only in this host test. Firmware never seeds a decorative trace.
  const RecipeId runs[] = {RecipeId::LeadedReflow, RecipeId::Nylon6Anneal,
    RecipeId::ChamberHold, RecipeId::Commission100, RecipeId::Autotune100};
  for (unsigned mode = 0; mode < 5; ++mode) {
    const auto& recipe = recipeFor(runs[mode]);
    s.recipe = &recipe; s.state = EngineState::Running; s.phase_index = 0;
    s.target_celsius = recipe.phases[0].target_celsius;
    s.process_celsius = 25; s.output_percent = mode >= 3 ? 25 : 32;
    s.heater_commanded_on = true; s.run_elapsed_seconds = 0;
    ui.update(s, study);
    assert(ui.chart_ && findButton("STOP"));
    assert(!ui.brightness_button_);
    const uint8_t run_brightness = ui.brightnessPercent();
    host_touch_point.x = 208; host_touch_point.y = 78; host_touch_down = true;
    advance(ui, 350); host_touch_point.y = 198; advance(ui, 20);
    host_touch_down = false; advance(ui, 40);
    assert(ui.brightnessPercent() == run_brightness);
    assert(!ui.consumeBrightnessChange());
    // No invented target/process samples at screen creation.
    for (int n = 0; n < thermal_ui::ChartPoints; ++n)
      assert(lv_chart_get_y_array(ui.chart_, ui.chart_process_)[n] == LV_CHART_POINT_NONE);
    for (unsigned k = 0; k < 30; ++k) {
      host_millis += 1000;
      s.run_elapsed_seconds = 100 + k;
      s.process_celsius = 25 + (s.target_celsius - 25) * k / 30.0F;
      ui.update(s, study); ui.tick(host_millis);
    }
    host_millis += 3000; ui.update(s, study);
    const auto* history = lv_chart_get_y_array(ui.chart_, ui.chart_process_);
    unsigned gaps = 0;
    for (int n = 0; n < thermal_ui::ChartPoints; ++n) gaps += history[n] == LV_CHART_POINT_NONE;
    assert(gaps >= 2);
    for (unsigned phase = 0; phase < recipe.phase_count; ++phase) {
      s.phase_index = phase;
      s.state = recipe.phases[phase].kind == PhaseKind::Cooldown ? EngineState::Cooling : EngineState::Running;
      s.target_celsius = recipe.phases[phase].target_celsius;
      s.process_celsius = s.state == EngineState::Cooling ? 84.3F : s.target_celsius - 0.8F;
      s.output_percent = s.state == EngineState::Cooling ? 0 : mode >= 3 ? 25 : 18;
      s.heater_commanded_on = false; // Off portion of burst despite nonzero demand.
      s.run_elapsed_seconds = mode == 1 ? 4005 : 314 + phase * 60;
      ui.update(s, study);
      char name[80]; std::snprintf(name, sizeof(name), "30-run-%u-phase-%u", mode, phase);
      capture(out, name);
      if (s.state == EngineState::Cooling) {
        assert(std::strcmp(lv_label_get_text(ui.target_), "OFF") == 0);
        assert(std::strcmp(lv_label_get_text(ui.error_), "--") == 0);
      }
      auto* stop = findButton("STOP"); assert(stop);
      lv_event_send(stop, LV_EVENT_PRESSED, nullptr);
      ui.queue(UiCommand::Home); // Cannot replace a queued Stop.
      assert(ui.consumeCommand() == UiCommand::Stop);
      send(ui, UiCommand::Home); assert(ui.screen_ == OvenUi::Screen::Run);
    }
    s.state = EngineState::Complete; s.process_celsius = 59.5F;
    ui.update(s, study);
    char name[80]; std::snprintf(name, sizeof(name), "40-complete-%u", mode); capture(out, name);
    if (mode == 0) assert(findLabelContaining(lv_scr_act(), "NEEDS REVALIDATION"));
    if (mode == 0) {
      ui.queue(UiCommand::Acknowledge);
      assert(ui.consumeCommand() == UiCommand::Acknowledge);
      assert(ui.screen_ == OvenUi::Screen::ProfileResults);
      s.state = EngineState::Idle; ui.update(s, study);
      capture(out, "40-profile-after-ack");
      ui.showHome();
    } else { s.state = EngineState::Idle; ui.update(s, study); ui.showHome(); }
  }
  RecipePhase custom_phases[] = {
      {PhaseKind::Ramp, 120.0F, 5.0F / 60.0F, 0U},
      {PhaseKind::Hold, 120.0F, 0.0F, 1800U},
      {PhaseKind::ControlledCool, 60.0F, 5.0F / 60.0F, 0U}};
  Recipe custom_recipe = {RecipeId::CustomAnneal, "Custom anneal", RunMode::Anneal,
      custom_phases, 3, 0.0F, 200.0F, false, 7200U, 100.0F};
  s.recipe = &custom_recipe; s.state = EngineState::Running; s.phase_index = 2;
  s.target_celsius = 91.5F; s.process_celsius = 95.0F; s.output_percent = 0.0F;
  s.heater_commanded_on = false; ui.update(s, study);
  capture(out, "39-custom-anneal-ramp-down");
  assert(findLabelContaining(lv_scr_act(), "ANNEAL"));
  assert(!findLabelContaining(lv_scr_act(), "CHAMBER"));
  assert(std::strcmp(lv_label_get_text(ui.phase_), "RAMP DOWN 3/3") == 0);
  assert(std::strcmp(lv_label_get_text(ui.target_), "91.5°C") == 0);
  assert(std::strcmp(lv_label_get_text(ui.error_), "+3.5°C") == 0);
  s.state = EngineState::Aborted; ui.update(s, study); capture(out, "41-stopped");
  s.state = EngineState::Idle; ui.update(s, study); ui.showHome();

  // Study states, candidate/active isolation, details and held save/retry.
  study = PidStudy{};
  study.setup_revision = 1;
  ui.update(s, study); ui.showStudy(); capture(out, "50-study-locked");
  ui.showStudyResults(); capture(out, "51-results-active-no-candidate");
  study.candidate_ready = true; study.candidate.kp = 2.4F;
  study.candidate.ki = 0.035F; study.candidate.kd = 7;
  ui.update(s, study); ui.showStudy(); capture(out, "52-study-candidate");
  ui.showStudyResults(); capture(out, "53-results-tests-needed");
  assert(!findButton("HOLD 2s: SAVE + USE"));
  for (unsigned i = 0; i < 3; ++i) {
    auto& p = study.checks[i]; p.result = i == 0 ? StudyResult::Complete : StudyResult::Empty;
    p.attempts = 2; p.peak_celsius = 103.2F; p.start_celsius = 24.8F;
    p.reached_band = true; p.rise_seconds = 220; p.hold_rmse = 1.23F;
    p.hold_output_percent = 12.6F; p.elapsed_seconds = 803;
    p.setup_revision = 3; p.candidate_revision = 4;
  }
  study.setup_revision = study.checked_setup_revision = 3;
  study.candidate_revision = 4;
  study.checked_scope_mask = 0;
  ui.update(s, study); capture(out, "54-results-ready-to-save");
  assert(ui.canSaveStudy());
  press(ui, findButton("HOLD 2s: SAVE + USE")); advance(ui, 500); release(ui);
  assert(ui.consumeCommand() == UiCommand::None);
  press(ui, findButton("HOLD 2s: SAVE + USE")); advance(ui, 2050);
  assert(ui.consumeCommand() == UiCommand::SaveStudy); release(ui);
  study.save_failed = true; ui.update(s, study); capture(out, "55-save-failed-retry");
  assert(findButton("HOLD 2s: RETRY SAVE"));
  study.save_failed = false; study.saved = true; study.checked_scope_mask = kControlCheck100Mask;
  study.checked_setup_revision = study.setup_revision; ui.update(s, study);
  capture(out, "56-results-saved"); assert(!findButton("HOLD 2s: SAVE + USE"));
  send(ui, UiCommand::ShowStudyDetails);
  for (int i = 0; i < 3; ++i) {
    char name[80]; std::snprintf(name, sizeof(name), "57-study-detail-%c", 'A' + i);
    capture(out, name); send(ui, UiCommand::DetailsNext);
  }
  send(ui, UiCommand::DetailsBack); assert(ui.screen_ == OvenUi::Screen::StudyResults);
  study.saved = false;
  for (auto& p : study.checks) { p.result = StudyResult::Aborted; p.reached_band = false; p.attempts = 9999; }
  ui.update(s, study); capture(out, "58-results-aborted");
  send(ui, UiCommand::ShowStudyDetails); capture(out, "59-detail-band-not-reached");
  send(ui, UiCommand::DetailsBack);
  assert(!ui.canSaveStudy()); send(ui, UiCommand::SaveStudy);

  // Every fault variant. Faults take over menus/runs; detail navigation never
  // acknowledges, starts, or modifies rejection thresholds.
  study.tune_report.available = study.tune_report.terminal = true;
  auto& d = study.tune_report.latest;
  d.cycle = 9; d.failed_checks = TuneFractionLow | TuneMidpointSpread;
  d.period = 106; d.fraction = .25F; d.amplitude = 5; d.midpoint = 100;
  d.period_ratio = 1.1F; d.amplitude_ratio = 1.1F; d.midpoint_span = 1.25F;
  d.window_count = 3;
  for (unsigned i = 0; i < 3; ++i) {
    d.window[i].cycle = 7 + i; d.window[i].period = 104 + i;
    d.window[i].amplitude = 5; d.window[i].midpoint = 99.5F + 0.625F * i;
  }
  s.recipe = &recipeFor(RecipeId::Autotune100); s.state = EngineState::Fault;
  const FaultCode faults[] = {FaultCode::ProcessProbe, FaultCode::ProcessOverTemperature,
    FaultCode::HighTemperatureProfileNotCommissioned, FaultCode::RunTimeout,
    FaultCode::TestStartTooHot, FaultCode::TuneUnstable, FaultCode::NoTuneCandidate,
    FaultCode::InvalidRecipe, FaultCode::CheckNotApproved, FaultCode::InvalidAnneal};
  for (unsigned i = 0; i < sizeof(faults) / sizeof(faults[0]); ++i) {
    s.fault = faults[i]; ui.update(s, study);
    char name[80]; std::snprintf(name, sizeof(name), "60-fault-%u", i); capture(out, name);
    send(ui, UiCommand::Home); assert(ui.screen_ == OvenUi::Screen::Fault);
    send(ui, UiCommand::StartSelected); send(ui, UiCommand::SaveStudy);
  }
  s.fault = FaultCode::TuneUnstable; ui.update(s, study);
  send(ui, UiCommand::ShowTuneDetails);
  for (int i = 0; i < 3; ++i) {
    char name[80]; std::snprintf(name, sizeof(name), "70-tune-diagnostics-%u", i);
    capture(out, name); ui.update(s, study);
    assert(ui.screen_ == OvenUi::Screen::TuneDetails);
    send(ui, UiCommand::DetailsNext);
  }
  send(ui, UiCommand::DetailsBack); assert(ui.screen_ == OvenUi::Screen::Fault);
  ui.queue(UiCommand::Acknowledge); assert(ui.consumeCommand() == UiCommand::Acknowledge);
  s.state = EngineState::Idle; s.fault = FaultCode::None; ui.update(s, study);
  send(ui, UiCommand::ShowStudy); send(ui, UiCommand::ShowTuneDetails);
  capture(out, "71-last-tune-data-idle");
  send(ui, UiCommand::DetailsBack); assert(ui.screen_ == OvenUi::Screen::Study);

  // Idle menus sleep after 30 s. Wake contact is consumed through release.
  s.state = EngineState::Idle; s.fault = FaultCode::None; s.recipe = nullptr;
  ui.update(s, study); ui.showHome();
  ui.setBacklight(true); ui.last_activity_ms_ = host_millis;
  advance(ui, 29990); assert(host_backlight_level == 1);
  advance(ui, 20); assert(host_backlight_level == 0);
  assert(host_ledc_duty == 0);
  press(ui, findButton("ANNEAL"));
  assert(host_backlight_level == 1);
  assert(host_ledc_duty == ui.brightnessPercent() * 255U / 100U);
  release(ui);
  assert(ui.consumeCommand() == UiCommand::None);
  assert(ui.screen_ == OvenUi::Screen::Home);
  tap(ui, "ANNEAL"); assert(ui.screen_ == OvenUi::Screen::AnnealEditor);
  send(ui, UiCommand::Home);

  // Held contact prevents sleep. Unsigned elapsed arithmetic survives wrap.
  ui.last_activity_ms_ = host_millis - 30000U;
  host_touch_point.x = 2; host_touch_point.y = 2;
  host_touch_down = true; advance(ui, 40);
  assert(host_backlight_level == 1);
  host_touch_down = false; advance(ui, 40);
  host_millis = UINT32_MAX - 10000U;
  ui.last_lv_tick_ms_ = host_millis;
  ui.last_activity_ms_ = host_millis;
  ui.setBacklight(true);
  advance(ui, 29990); assert(host_backlight_level == 1);
  advance(ui, 20); assert(host_backlight_level == 0);

  // Active, cooling, fault and terminal screens never blank. Any non-idle
  // update also wakes a previously blanked menu.
  s.recipe = &custom_recipe; s.probe_healthy = true; s.process_celsius = 80.0F;
  s.target_celsius = 90.0F;
  const EngineState no_sleep[] = {EngineState::Running, EngineState::Cooling,
      EngineState::Fault, EngineState::Complete, EngineState::Aborted};
  for (auto state : no_sleep) {
    s.state = state;
    if (state == EngineState::Fault) s.fault = FaultCode::InvalidAnneal;
    ui.update(s, study);
    assert(host_backlight_level == 1);
    assert(host_ledc_duty == ui.brightnessPercent() * 255U / 100U);
    ui.last_activity_ms_ = host_millis - 30000U;
    advance(ui, 20);
    assert(host_backlight_level == 1);
  }
  s.state = EngineState::Idle; s.fault = FaultCode::None; s.recipe = nullptr;
  ui.update(s, study); ui.showHome();

  // Deterministic navigation soak: screen construction does not accumulate
  // LVGL heap allocations. Compare two full cycles, not unlike screen types.
  auto loop = [&]() {
    for (int i = 0; i < 100; ++i) {
      ui.showHome(); ui.refreshDynamic(s);
      ui.showRecipeList(); ui.showConfirm(RecipeId::LeadedReflow);
      ui.showCommissioning(); ui.showStudy(); ui.showStudyResults();
    }
    ui.showHome(); ui.refreshDynamic(s); lv_obj_update_layout(lv_scr_act()); lv_refr_now(nullptr);
  };
  loop(); lv_mem_monitor_t first; lv_mem_monitor(&first);
  loop(); lv_mem_monitor_t second; lv_mem_monitor(&second);
  assert(second.free_size == first.free_size);
  std::printf("SIGNAL-3: %u native captures; fixed-probe workflow; UI bounds/touch targets; real pointer holds;\n"
              "Stop priority; locked starts; save/retry; fault/detail routing; chart gaps; icon geometry passed.\n"
              "Navigation soak: 200 cycles, stable LVGL free heap %lu bytes.\n",
              capture_count, (unsigned long)second.free_size);
  std::printf("Host LVGL minima across captures: free=%lu bytes; largest block=%lu bytes.\n",
              (unsigned long)minimum_free, (unsigned long)minimum_largest);
}
