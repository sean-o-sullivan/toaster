#pragma once

#include <cstdint>

#include <lvgl.h>

#include "anneal_program.h"
#include "display_settings.h"
#include "profile_validation_state.h"
#include "thermal_engine.h"

enum class UiCommand : uint8_t {
  None,
  ShowReflowRecipes,
  ShowCommissioning,
  ShowStudy,
  ChooseAutotune,
  ChooseCheck100,
  ChooseCheck150,
  ChooseCheck200,
  ShowStudyResults,
  SaveStudy,
  ChooseCommission100,
  ChooseCommission150,
  ChooseCommission200,
  ChooseLeaded,
  ChooseSac305,
  ChooseAnneal,
  ChooseHold,
  StartSelected,
  StartCustomAnneal,
  AnnealReview,
  Stop,
  Acknowledge,
  Home,
  ShowStudyDetails,
  ShowTuneDetails,
  ShowProfileResults,
  CommissionProfile,
  DetailsPrevious,
  DetailsNext,
  DetailsBack,
};

class OvenUi {
 public:
  bool begin(uint8_t brightness_percent = kDefaultBrightnessPercent);
  void tick(uint32_t now_ms);
  void update(const EngineSnapshot& snapshot, const PidStudy& study = PidStudy{});
  void setProfileValidation(const ProfileValidationView* views);
  void setElectronicsTemperatures(float max_celsius, float cpu_celsius);
  UiCommand consumeCommand();
  uint8_t brightnessPercent() const { return brightness_percent_; }
  bool consumeBrightnessChange();
  RecipeId selectedRecipe() const { return selected_recipe_; }
  const AnnealProgram& annealProgram() const { return anneal_program_; }
  static void eventHandler(lv_event_t* event);

 private:
  enum class Screen : uint8_t { Home, Recipe, Commissioning, Study, StudyResults, Confirm,
    AnnealEditor, AnnealReview, Run, Fault, Complete, StudyDetails,
    TuneDetails, ProfileResults };
  enum class AnnealField : uint8_t { Temperature, Soak, Ramp };

  void prepareScreen(Screen screen);
  void showHome();
  void showRecipeList();
  void showCommissioning();
  void showStudy();
  void showStudyResults();
  void showAnnealEditor();
  void showAnnealReview();
  void showConfirm(RecipeId recipe_id);
  void showRun();
  void showFault();
  void showComplete();
  void showStudyDetails();
  void showTuneDetails();
  void showProfileResults();
  void updateHold();
  bool canSaveStudy() const;
  bool canCommissionSelectedProfile() const;
  uint8_t startBlockReason() const;
  void playBootAnimation();
  void refreshDynamic(const EngineSnapshot& snapshot);
  void queue(UiCommand command);
  bool selectedRecipeCanStart() const;
  bool annealFieldsComplete() const;
  bool annealProgramFitsLimit() const;
  bool customAnnealCanStart() const;
  void setBacklight(bool awake);
  void setBrightness(uint8_t brightness_percent);
  void updateBrightnessGesture(uint32_t now_ms);
  void cancelBrightnessGesture(bool restore);
  void refreshAnnealEditor();

  static void displayFlush(lv_disp_drv_t* display, const lv_area_t* area, lv_color_t* color);
  static void touchRead(lv_indev_drv_t* driver, lv_indev_data_t* data);
  static void brightnessEventHandler(lv_event_t* event);
  static void annealDialEventHandler(lv_event_t* event);

  Screen screen_ = Screen::Home;
  RecipeId selected_recipe_ = RecipeId::LeadedReflow;
  UiCommand pending_command_ = UiCommand::None;
  uint32_t last_lv_tick_ms_ = 0;
  uint32_t last_chart_ms_ = 0;
  uint32_t last_activity_ms_ = 0;
  EngineSnapshot snapshot_;
  uint8_t confirm_reason_ = 0;
  uint8_t details_page_ = 0;
  bool details_from_fault_ = false;
  lv_obj_t* held_button_ = nullptr;
  lv_obj_t* hold_progress_ = nullptr;
  uint32_t hold_started_ms_ = 0;
  uint32_t hold_refresh_ms_ = 0;
  uint32_t mode_color_ = 0;
  bool probe_healthy_ = false;
  bool test_start_cool_ = false;
  bool confirm_start_allowed_ = false;
  bool backlight_awake_ = true;
  bool physical_touch_down_ = false;
  bool consume_wake_touch_ = false;
  bool brightness_change_pending_ = false;
  bool brightness_gesture_armed_ = false;
  bool brightness_adjusting_ = false;
  uint8_t brightness_percent_ = kDefaultBrightnessPercent;
  uint8_t brightness_gesture_start_percent_ = kDefaultBrightnessPercent;
  int16_t touch_y_ = 0;
  int16_t brightness_gesture_start_y_ = 0;
  uint32_t brightness_gesture_started_ms_ = 0;
  AnnealProgram anneal_program_;
  bool anneal_temperature_set_ = false;
  bool anneal_soak_set_ = false;
  bool anneal_ramp_set_ = false;
  bool anneal_editor_review_allowed_ = false;
  PidStudy study_;
  ProfileValidationView profile_validation_[4];
  bool profile_validation_dirty_ = false;
  EngineState engine_state_ = EngineState::Idle;

  lv_obj_t* title_ = nullptr;
  lv_obj_t* temperature_ = nullptr;
  lv_obj_t* target_ = nullptr;
  lv_obj_t* phase_ = nullptr;
  lv_obj_t* timer_ = nullptr;
  lv_obj_t* output_ = nullptr;
  lv_obj_t* health_ = nullptr;
  lv_obj_t* heater_ = nullptr;
  lv_obj_t* detail_ = nullptr;
  lv_obj_t* unit_ = nullptr;
  lv_obj_t* error_ = nullptr;
  lv_obj_t* anneal_dials_[3] = {};
  lv_obj_t* anneal_values_[3] = {};
  lv_obj_t* anneal_up_ = nullptr;
  lv_obj_t* anneal_down_ = nullptr;
  lv_obj_t* anneal_total_ = nullptr;
  lv_obj_t* anneal_review_ = nullptr;
  lv_obj_t* demand_bar_ = nullptr;
  lv_obj_t* chart_empty_ = nullptr;
  lv_obj_t* brightness_button_ = nullptr;
  lv_obj_t* electronics_temperatures_ = nullptr;
  float max_board_celsius_ = NAN;
  float cpu_celsius_ = NAN;
  lv_obj_t* stage_pips_[5] = {};
  lv_obj_t* chart_ = nullptr;
  lv_chart_series_t* chart_process_ = nullptr;
  lv_chart_series_t* chart_target_ = nullptr;
};
