#pragma once

#include <driver/gpio.h>

namespace board {

// Freenove FNK0104B 2.8-inch capacitive-touch display. These are board-owned.
constexpr gpio_num_t kTftMosi = GPIO_NUM_11;
constexpr gpio_num_t kTftSclk = GPIO_NUM_12;
constexpr gpio_num_t kTftMiso = GPIO_NUM_13;
constexpr gpio_num_t kTftCs = GPIO_NUM_10;
constexpr gpio_num_t kTftDc = GPIO_NUM_46;
constexpr gpio_num_t kTftBacklight = GPIO_NUM_45;

constexpr gpio_num_t kTouchScl = GPIO_NUM_15;
constexpr gpio_num_t kTouchSda = GPIO_NUM_16;
constexpr gpio_num_t kTouchInterrupt = GPIO_NUM_17;
constexpr gpio_num_t kTouchReset = GPIO_NUM_18;

// External low-voltage harness.
// P3/IO: pin 1 = GPIO2, pin 3 = GPIO14, pin 4 = GPIO21.
// P2/Serial: pin 3 = GPIO43, pin 4 = GPIO44.
constexpr gpio_num_t kSsrGate = GPIO_NUM_2;
constexpr gpio_num_t kThermocoupleSclk = GPIO_NUM_14;
constexpr gpio_num_t kThermocoupleMiso = GPIO_NUM_21;
constexpr gpio_num_t kProcessThermocoupleCs = GPIO_NUM_43;
constexpr gpio_num_t kUnusedThermocoupleCs = GPIO_NUM_44;

constexpr gpio_num_t kStatusLed = GPIO_NUM_42;

constexpr uint8_t kTouchAddress = 0x38;

}  // namespace board
