#pragma once

#include <cstddef>
#include <cstdint>
#include "thermal_engine.h"

// Separate record: the v1 terminal-report layout and NVS key remain readable.
struct TuneHistory {
  uint32_t version = 1;
  uint32_t first_crossing_ms = UINT32_MAX;
  uint32_t accepted_ms = UINT32_MAX;
  uint32_t cooldown_end_ms = UINT32_MAX;
  uint32_t deadline_ms = 1200000;
  uint32_t elapsed_ms = 0;
  uint8_t count = 0;
  bool terminal = false;
  TuneOutcome outcome = TuneOutcome::None;
  TuneCycleDiagnostics cycles[10];
  uint32_t checksum = 0;
};

inline uint32_t tuneHistoryChecksum(const TuneHistory& value) {
  uint32_t hash = 2166136261U;
  const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
  for (size_t i = 0; i < offsetof(TuneHistory, checksum); ++i)
    hash = (hash ^ bytes[i]) * 16777619U;
  return hash;
}

inline bool validTuneHistory(const TuneHistory& value) {
  return value.version == 1 && value.terminal && value.count <= 10 &&
         value.checksum == tuneHistoryChecksum(value);
}
