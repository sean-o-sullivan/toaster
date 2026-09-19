#pragma once
#include <cstdint>

// THERMAL / SIGNAL-3. Single source of truth for colour and mode sigils.
// A sigil is a 3x3 occupancy mask, MSB at top-left. Each occupied cell is
// one solid square, NOT a stroke or a rescaled pictogram. Decorative identity
// marks only: these are not decodable or certified machine-vision fiducials.
namespace thermal_ui {
constexpr uint32_t Background = 0x080F14;
constexpr uint32_t Surface = 0x13212A;
constexpr uint32_t Border = 0x48616D;
constexpr uint32_t Text = 0xF2F6FA;
constexpr uint32_t Muted = 0xB3C5CF;
constexpr uint32_t Ink = 0x05090C;
constexpr uint32_t Reflow = 0xC6FF00;
constexpr uint32_t Anneal = 0xFF45B5;
constexpr uint32_t Chamber = 0x00D9F5;
constexpr uint32_t Test = 0xFFAA00;
constexpr uint32_t Fault = 0xFF484F;
constexpr uint32_t Caution = 0xFFCC66;

// REFLOW: glider. ANNEAL: alternating lattice. CHAMBER: enclosed aperture.
// TEST: calibration cross. All occupy the same three rows and columns.
constexpr uint16_t ReflowMask = 0x08F;   // 010 / 001 / 111
constexpr uint16_t AnnealMask = 0x155;   // 101 / 010 / 101
constexpr uint16_t ChamberMask = 0x1EF;  // 111 / 101 / 111
constexpr uint16_t TestMask = 0x0BA;     // 010 / 111 / 010
constexpr uint16_t BrandMask = 0x08F;
constexpr int16_t Width = 240;
constexpr int16_t Height = 320;
constexpr int16_t Margin = 8;
constexpr int16_t Content = 224;
constexpr int16_t HeaderHeight = 40;
constexpr int16_t ActionHeight = 44;
constexpr int16_t ActionY = 268;
constexpr int16_t IconModule = 8;  // 24x24 glyph within a 40x40 header tile.
constexpr uint32_t HoldMs = 2000;
constexpr uint32_t ChartPeriodMs = 1000;
constexpr uint16_t ChartPoints = 30;
constexpr bool occupied(uint16_t mask, unsigned row, unsigned column) {
  return (mask & (1U << (8U - row * 3U - column))) != 0U;
}
}  // namespace thermal_ui
