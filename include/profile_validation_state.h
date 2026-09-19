#pragma once

#include "process_validation.h"

struct ProfileValidationView {
  ValidationReport report;
  uint8_t consecutive_passes = 0;
  bool eligible = false;
  bool commissioned = false;
  bool needs_revalidation = false;
};
