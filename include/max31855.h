#pragma once

#include <driver/spi_master.h>

#include "thermocouple_types.h"

class Max31855 {
 public:
  bool begin();
  ThermocoupleReading readProcess();

 private:
  ThermocoupleReading read(spi_device_handle_t device);

  spi_device_handle_t process_device_ = nullptr;
};
