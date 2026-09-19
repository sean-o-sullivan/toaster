#pragma once
#include <cstdint>
constexpr int OUTPUT = 1;
constexpr int LOW = 0;
constexpr int HIGH = 1;
extern uint32_t host_millis;
extern int host_backlight_level;
extern uint8_t host_ledc_channel;
extern uint8_t host_ledc_pin;
extern uint32_t host_ledc_frequency;
extern uint8_t host_ledc_resolution;
extern uint32_t host_ledc_duty;
inline uint32_t millis() { return host_millis; }
inline void delay(uint32_t ms) { host_millis += ms; }
inline void pinMode(int, int) {}
inline void digitalWrite(int, int value) { host_backlight_level = value; }
inline double ledcSetup(uint8_t channel, double frequency, uint8_t resolution) {
  host_ledc_channel = channel;
  host_ledc_frequency = static_cast<uint32_t>(frequency);
  host_ledc_resolution = resolution;
  return frequency;
}
inline void ledcAttachPin(uint8_t pin, uint8_t channel) {
  host_ledc_pin = pin;
  host_ledc_channel = channel;
}
inline void ledcWrite(uint8_t channel, uint32_t duty) {
  host_ledc_channel = channel;
  host_ledc_duty = duty;
  host_backlight_level = duty ? 1 : 0;
}
