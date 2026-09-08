#pragma once

#include <cstddef>
#include <cstdint>
#include "pid_control.h"

struct SavedPid {
  uint32_t version = 1;
  PidGains gains;
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
