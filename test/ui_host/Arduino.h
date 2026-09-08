#pragma once
#include <cstdint>
constexpr int OUTPUT = 1;
constexpr int LOW = 0;
constexpr int HIGH = 1;
extern uint32_t host_millis;
inline uint32_t millis() { return host_millis; }
inline void delay(uint32_t ms) { host_millis += ms; }
inline void pinMode(int, int) {}
inline void digitalWrite(int, int) {}
