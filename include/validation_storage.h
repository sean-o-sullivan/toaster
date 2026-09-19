#pragma once

#include <cstddef>
#include "process_validation.h"

struct SavedValidationReport {
  uint32_t version = 1;
  uint8_t profile = 0;
  ValidationReport report;
  uint32_t checksum = 0;
};

inline uint32_t validationChecksum(const SavedValidationReport& value) {
  const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
  uint32_t hash = 2166136261U;
  for (size_t i = 0; i < offsetof(SavedValidationReport, checksum); ++i)
    hash = (hash ^ bytes[i]) * 16777619U;
  return hash;
}

inline bool validSavedValidation(const SavedValidationReport& value) {
  return value.version == 1 && value.profile < 4 &&
      static_cast<uint8_t>(value.report.status) <= static_cast<uint8_t>(ValidationStatus::CriteriaMissing) &&
      value.report.status != ValidationStatus::Running &&
      value.checksum == validationChecksum(value);
}
