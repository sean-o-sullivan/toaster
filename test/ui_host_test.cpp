#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>

#define private public
#include "ui.h"
#undef private
#include "touch_ft6336.h"

uint32_t host_millis = 0;
uint16_t host_pixels[240 * 320] = {};
bool Ft6336Touch::begin() { return true; }
bool Ft6336Touch::read(TouchPoint&) { return false; }

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

void capture(const char* directory, const char* name) {
  lv_obj_update_layout(lv_scr_act());
  lv_refr_now(nullptr);
  checkBounds(lv_scr_act());
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

int main(int argc, char** argv) {
  assert(argc == 2);
  OvenUi ui;
  assert(ui.begin());
  EngineSnapshot snapshot;
  snapshot.probe_healthy = true;
  snapshot.process_celsius = 25.0F;
  ui.update(snapshot);
  capture(argv[1], "home");
  ui.queue(UiCommand::ShowCommissioning);
  assert(ui.consumeCommand() == UiCommand::None);
  capture(argv[1], "heater-tests");
  const UiCommand commands[] = {UiCommand::ChooseCommission100, UiCommand::ChooseCommission150,
                                 UiCommand::ChooseCommission200};
  for (auto command : commands) {
    ui.queue(command);
    assert(ui.consumeCommand() == UiCommand::None);
    assert(ui.confirm_start_allowed_);
    capture(argv[1], "test-review");
  }
  snapshot.process_celsius = 61.0F;
  ui.update(snapshot);
  assert(!ui.confirm_start_allowed_);
  ui.queue(UiCommand::StartSelected);
  assert(ui.consumeCommand() == UiCommand::None);
  capture(argv[1], "test-too-hot");
  snapshot.process_celsius = 25.0F;
  snapshot.probe_healthy = false;
  ui.update(snapshot);
  assert(!ui.confirm_start_allowed_);
  capture(argv[1], "test-check-probe");
  snapshot.probe_healthy = true;
  ui.update(snapshot);
  assert(ui.confirm_start_allowed_);
  assert(lv_indev_get_next(nullptr)->driver->long_press_time == 2000U);
  lv_obj_t* start_button = nullptr;
  for (uint32_t i = 0; i < lv_obj_get_child_cnt(lv_scr_act()); ++i) {
    auto* child = lv_obj_get_child(lv_scr_act(), i);
    if (!lv_obj_check_type(child, &lv_btn_class)) continue;
    auto* label = lv_obj_get_child(child, 0);
    if (std::strcmp(lv_label_get_text(label), "HOLD 2s TO START") == 0) start_button = child;
  }
  assert(start_button);
  lv_event_send(start_button, LV_EVENT_CLICKED, nullptr);
  assert(ui.consumeCommand() == UiCommand::None);
  lv_event_send(start_button, LV_EVENT_LONG_PRESSED, nullptr);
  assert(ui.consumeCommand() == UiCommand::StartSelected);
  snapshot.state = EngineState::Running;
  snapshot.recipe = &recipeFor(ui.selectedRecipe());
  snapshot.target_celsius = 100.0F;
  snapshot.output_percent = 25.0F;
  ui.update(snapshot);
  capture(argv[1], "test-running");
  ui.queue(UiCommand::Stop);
  assert(ui.consumeCommand() == UiCommand::Stop);
  snapshot.state = EngineState::Fault;
  snapshot.fault = FaultCode::RunTimeout;
  ui.update(snapshot);
  capture(argv[1], "run-timeout");
  const FaultCode faults[] = {FaultCode::ProcessProbe,
      FaultCode::ProcessOverTemperature,
      FaultCode::HighTemperatureProfileNotCommissioned, FaultCode::TestStartTooHot};
  for (auto fault : faults) {
    snapshot.fault = fault;
    ui.update(snapshot);
    capture(argv[1], toString(fault));
  }
  ui.showConfirm(RecipeId::Sac305Reflow);
  assert(!ui.confirm_start_allowed_);
  capture(argv[1], "sac-locked");

  snapshot.state = EngineState::Idle;
  PidStudy study;
  ui.update(snapshot, study);
  ui.queue(UiCommand::ShowStudy);
  assert(ui.consumeCommand() == UiCommand::None);
  capture(argv[1], "pid-study-locked");
  ui.queue(UiCommand::ChooseValidateB);
  assert(ui.consumeCommand() == UiCommand::None);
  assert(!ui.confirm_start_allowed_);
  capture(argv[1], "pid-no-candidate");
  ui.queue(UiCommand::ChooseAutotune);
  assert(ui.consumeCommand() == UiCommand::None);
  assert(ui.confirm_start_allowed_);
  capture(argv[1], "pid-tune-review");
  study.candidate_ready = true;
  ui.update(snapshot, study);
  ui.showStudy();
  capture(argv[1], "pid-study-ready");
  ui.showStudyResults();
  capture(argv[1], "pid-results-empty");
  for (auto& point : study.points) {
    point.result = StudyResult::Complete;
    point.attempts = 2;
    point.peak_celsius = 103.25F;
    point.rise_seconds = 220;
    point.hold_rmse = 1.23F;
  }
  ui.update(snapshot, study);
  ui.showStudyResults();
  capture(argv[1], "pid-results-complete");
  lv_obj_t* save_button = nullptr;
  for (uint32_t i = 0; i < lv_obj_get_child_cnt(lv_scr_act()); ++i) {
    auto* child = lv_obj_get_child(lv_scr_act(), i);
    if (!lv_obj_check_type(child, &lv_btn_class)) continue;
    auto* label = lv_obj_get_child(child, 0);
    if (std::strcmp(lv_label_get_text(label), "HOLD 2s: SAVE + USE") == 0) save_button = child;
  }
  assert(save_button);
  lv_event_send(save_button, LV_EVENT_CLICKED, nullptr);
  assert(ui.consumeCommand() == UiCommand::None);
  lv_event_send(save_button, LV_EVENT_LONG_PRESSED, nullptr);
  assert(ui.consumeCommand() == UiCommand::SaveStudy);
  study.save_failed = true;
  ui.update(snapshot, study);
  capture(argv[1], "pid-save-failed");
  study.save_failed = false;
  study.saved = true;
  ui.update(snapshot, study);
  capture(argv[1], "pid-saved");
  ui.showStudy();
  capture(argv[1], "pid-study-saved");
  for (auto& point : study.points) {
    point.result = StudyResult::Aborted;
    point.attempts = 99;
    point.peak_celsius = 119.75F;
    point.rise_seconds = 1199;
    point.hold_rmse = 20.25F;
  }
  ui.update(snapshot, study);
  ui.showStudyResults();
  capture(argv[1], "pid-results-stopped");
  ui.showConfirm(RecipeId::ValidateC);
  assert(ui.confirm_start_allowed_);
  snapshot.process_celsius = 61;
  ui.update(snapshot, study);
  assert(!ui.confirm_start_allowed_);
  capture(argv[1], "pid-validation-hot");
  snapshot.state = EngineState::Fault;
  for (auto fault : {FaultCode::TuneUnstable, FaultCode::NoTuneCandidate}) {
    snapshot.fault = fault;
    ui.update(snapshot, study);
    capture(argv[1], toString(fault));
  }
  std::puts("LVGL screen bounds, labels, navigation and interlocks passed");
}
