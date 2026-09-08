#include "max31855.h"

#include <Arduino.h>
#include <driver/gpio.h>

#include "board_pins.h"
#include "max31855_decode.h"

namespace {

constexpr int kSpiClockHz = 100000;

spi_device_interface_config_t makeDeviceConfig(gpio_num_t chip_select) {
  spi_device_interface_config_t config = {};
  config.clock_speed_hz = kSpiClockHz;
  config.mode = 0;
  // Receive-only device; IDF applies CS setup cycles in half-duplex mode.
  config.flags = SPI_DEVICE_HALFDUPLEX;
  // MAX31855 requires >=100 ns CS setup/hold; one clock gives 10 us.
  config.cs_ena_pretrans = 1;
  config.cs_ena_posttrans = 1;
  config.spics_io_num = chip_select;
  config.queue_size = 1;
  return config;
}

}  // namespace

bool Max31855::begin() {
  gpio_set_pull_mode(board::kProcessThermocoupleCs, GPIO_PULLUP_ONLY);
  // Keep the retired module deselected while the old harness remains attached.
  gpio_set_level(board::kUnusedThermocoupleCs, 1);
  gpio_set_direction(board::kUnusedThermocoupleCs, GPIO_MODE_OUTPUT);
  gpio_set_pull_mode(board::kUnusedThermocoupleCs, GPIO_PULLUP_ONLY);

  spi_bus_config_t bus_config = {};
  bus_config.mosi_io_num = -1;
  bus_config.miso_io_num = board::kThermocoupleMiso;
  bus_config.sclk_io_num = board::kThermocoupleSclk;
  bus_config.quadwp_io_num = -1;
  bus_config.quadhd_io_num = -1;
  bus_config.max_transfer_sz = 4;

  if (spi_bus_initialize(SPI3_HOST, &bus_config, SPI_DMA_DISABLED) != ESP_OK) {
    return false;
  }
  // Supplement the external 47k pull-down: an absent module reads all zeros.
  gpio_set_pull_mode(board::kThermocoupleMiso, GPIO_PULLDOWN_ONLY);

  const auto process_config = makeDeviceConfig(board::kProcessThermocoupleCs);
  if (spi_bus_add_device(SPI3_HOST, &process_config, &process_device_) != ESP_OK) {
    return false;
  }
  return true;
}

ThermocoupleReading Max31855::readProcess() { return read(process_device_); }


ThermocoupleReading Max31855::read(spi_device_handle_t device) {
  ThermocoupleReading reading;
  reading.sample_ms = millis();
  if (device == nullptr) {
    return reading;
  }

  spi_transaction_t transaction = {};
  transaction.flags = SPI_TRANS_USE_RXDATA;
  transaction.length = 0;  // No transmit phase; exactly 32 receive clocks.
  transaction.rxlength = 32;
  const esp_err_t result = spi_device_transmit(device, &transaction);
  if (result != ESP_OK) {
    return reading;
  }

  const uint32_t raw = (static_cast<uint32_t>(transaction.rx_data[0]) << 24U) |
                       (static_cast<uint32_t>(transaction.rx_data[1]) << 16U) |
                       (static_cast<uint32_t>(transaction.rx_data[2]) << 8U) |
                       transaction.rx_data[3];
  reading = decodeMax31855(raw, millis());
#ifdef TOASTER_SENSOR_DIAGNOSTICS
  static uint32_t last_log_ms = 0;
  if (reading.sample_ms - last_log_ms >= 1000) {
    last_log_ms = reading.sample_ms;
    char line[96];
    const int length = snprintf(line, sizeof(line),
        "MAX P raw=%08lX fault=%u tc=%.2f cj=%.2f\n",
        static_cast<unsigned long>(raw),
        static_cast<unsigned>(reading.fault), reading.celsius, reading.internal_celsius);
    // Diagnostics must never wait for a USB host or delay heater control.
    if (length > 0 && length < static_cast<int>(sizeof(line)) && Serial &&
        Serial.availableForWrite() >= length) {
      Serial.write(reinterpret_cast<const uint8_t*>(line), length);
    }
  }
#endif
  return reading;
}
