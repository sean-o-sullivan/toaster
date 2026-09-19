#pragma once

#include <cstddef>
#include <cstdint>
#include "pid_control.h"

struct SavedPid {
  uint32_t version = 1;
  PidGains gains;
  uint32_t checksum = 0;
};

struct SavedPidV2 {
  uint32_t version = 2;
  PidGains gains;
  uint8_t checked_scope_mask = 0;
  uint8_t reserved[3] = {0, 0, 0};
  uint32_t setup_revision = 0;
  uint32_t checksum = 0;
};

inline uint32_t pidChecksum(const SavedPid& data) {
  uint32_t hash = 2166136261U;
  const auto* bytes = reinterpret_cast<const uint8_t*>(&data);
  for (size_t i = 0; i < offsetof(SavedPid, checksum); ++i) {
    hash = (hash ^ bytes[i]) * 16777619U;
  }
  return hash;
}

inline bool validSavedPid(const SavedPid& data) {
  return data.version == 1 && data.checksum == pidChecksum(data) && validPidGains(data.gains);
}

inline uint32_t pidChecksum(const SavedPidV2& data) {
  uint32_t hash = 2166136261U;
  const auto* bytes = reinterpret_cast<const uint8_t*>(&data);
  for (size_t i = 0; i < offsetof(SavedPidV2, checksum); ++i) {
    hash = (hash ^ bytes[i]) * 16777619U;
  }
  return hash;
}

inline bool validSavedPid(const SavedPidV2& data) {
  return data.version == 2 && (data.checked_scope_mask & ~0x07U) == 0U &&
         (data.checked_scope_mask == 0U || data.setup_revision != 0U) &&
         data.checksum == pidChecksum(data) && validPidGains(data.gains);
}

inline bool migrateSavedPid(const SavedPid& legacy, SavedPidV2& migrated) {
  if (!validSavedPid(legacy)) return false;
  migrated = {};
  migrated.gains = legacy.gains;
  migrated.checked_scope_mask = 0;
  migrated.setup_revision = 0;
  migrated.checksum = pidChecksum(migrated);
  return true;
}
