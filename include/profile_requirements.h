#pragma once

#include "process_validation.h"

// Deliberately unconfigured. Replace only from the actual paste/polymer
// specification and reviewed measurement windows, not generic alloy examples.
inline Requirements requirementsForProfile(uint8_t recipe_id) {
  Requirements requirements;
  requirements.profile = recipe_id < 2 ? ValidationProfile::Reflow : ValidationProfile::Anneal;
  return requirements;
}

inline uint32_t profileCriteriaRevision(uint8_t) { return 0; }
