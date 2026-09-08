#pragma once

#include "thermocouple_types.h"

// Wire-order word: D31 is the most significant bit.
ThermocoupleReading decodeMax31855(uint32_t raw, uint32_t sample_ms);
