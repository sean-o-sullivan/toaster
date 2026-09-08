#pragma once

#include <cstdint>

#include <lvgl.h>

#include "thermal_engine.h"

enum class UiCommand : uint8_t {
  None,
  ShowReflowRecipes,
  ShowCommissioning,
  ShowStudy,
  ChooseAutotune,
  ChooseValidateA,
  ChooseValidateB,
  ChooseValidateC,
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
  Stop,
  Acknowledge,
  Home,
};

class OvenUi {
 public:
  bool begin();
  void tick(uint32_t now_ms);
  void update(const EngineSnapshot& snapshot, const PidStudy& study = PidStudy{});
  UiCommand consumeCommand();
  RecipeId selectedRecipe() const { return selected_recipe_; }
  static void eventHandler(lv_event_t* event);

 private:
  enum class Screen : uint8_t { Home, Recipe, Commissioning, Study, StudyResults, Confirm, Run, Fault, Complete };

  void prepareScreen(Screen screen);
  void showHome();
  void showRecipeList();
  void showCommissioning();
  void showStudy();
  void showStudyResults();
  void showConfirm(RecipeId recipe_id);
  void showRun();
  void showFault();
  void showComplete();
  void playBootAnimation();
  void refreshDynamic(const EngineSnapshot& snapshot);
  void queue(UiCommand command);
  bool selectedRecipeCanStart() const;

  static void displayFlush(lv_disp_drv_t* display, const lv_area_t* area, lv_color_t* color);
  static void touchRead(lv_indev_drv_t* driver, lv_indev_data_t* data);

  Screen screen_ = Screen::Home;
  RecipeId selected_recipe_ = RecipeId::LeadedReflow;
  UiCommand pending_command_ = UiCommand::None;
  uint32_t last_lv_tick_ms_ = 0;
  uint32_t last_chart_ms_ = 0;
  bool probe_healthy_ = false;
  bool test_start_cool_ = false;
  bool confirm_start_allowed_ = false;
  PidStudy study_;
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
  lv_obj_t* chart_ = nullptr;
  lv_chart_series_t* chart_process_ = nullptr;
  lv_chart_series_t* chart_target_ = nullptr;
};
