#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>

#include "thermal_engine.h"

struct SavedTuneReport {
  uint32_t version = 1;
  TuneRunReport report;
  uint32_t checksum = 0;
};

inline uint32_t tuneReportChecksum(const SavedTuneReport& data) {
  uint32_t hash = 2166136261U;
  const auto* bytes = reinterpret_cast<const uint8_t*>(&data);
  for (size_t i = 0; i < offsetof(SavedTuneReport, checksum); ++i)
    hash = (hash ^ bytes[i]) * 16777619U;
  return hash;
}

inline bool validSavedTuneReport(const SavedTuneReport& data) {
  const auto& d = data.report.latest;
  bool windows_valid = d.window_count <= 3U;
  for (uint8_t i = 0; i < d.window_count && windows_valid; ++i) {
    const auto& w = d.window[i];
    windows_valid = std::isfinite(w.period) && std::isfinite(w.heat_seconds) &&
                    std::isfinite(w.fraction) && std::isfinite(w.minimum) &&
                    std::isfinite(w.maximum) && std::isfinite(w.amplitude) &&
                    std::isfinite(w.midpoint);
  }
  return windows_valid && data.version == 1 && data.checksum == tuneReportChecksum(data) &&
         data.report.available && data.report.terminal &&
         std::isfinite(d.period) && std::isfinite(d.heat_seconds) &&
         std::isfinite(d.fraction) && std::isfinite(d.minimum) &&
         std::isfinite(d.maximum) && std::isfinite(d.amplitude) &&
         std::isfinite(d.midpoint) && std::isfinite(d.period_ratio) &&
         std::isfinite(d.amplitude_ratio) && std::isfinite(d.midpoint_span);
}
