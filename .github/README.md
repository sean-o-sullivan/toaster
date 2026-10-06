# Toaster

Firmware for a 10-litre, 1000 W quartz toaster oven converted for PCB reflow and
annealing. An ESP32-S3 touchscreen reads a K-type thermocouple through a MAX31855
and time-proportions both heating elements through a solid-state relay.

Write-up: <https://sean-osullivan.com/tooling/toaster/>

## Hardware

- Freenove FNK0104B: ESP32-S3 with a 2.8-inch capacitive touchscreen
- MAX31855 K-type thermocouple interface
- Fibreglass-insulated K-type bead thermocouple, taped to the board being heated
- SSR-40DA solid-state relay
- Cecotec Bake&Toast 1090, with its original thermostat and thermal cutoff left in circuit

## Wiring (low voltage)

| Function | Board connector | ESP32-S3 GPIO |
|---|---|---:|
| SSR DC + | P3 pin 1 | 2 |
| MAX31855 SCK | P3 pin 3 | 14 |
| MAX31855 SO | P3 pin 4 | 21 |
| MAX31855 CS | P2 pin 3 (TX) | 43 |
| MAX31855 VIN (3.3 V) | P4 pin 1 | - |
| GND (MAX31855 and SSR DC -) | P4 pin 2 | - |

Fit a 10 kOhm pull-up from CS to 3.3 V and a 47 kOhm pull-down from SO to GND.
GPIO3 is a strapping pin and is left unused. The firmware holds GPIO44 (P2 RX) high;
leave it unconnected. USB-C stays the power, upload and logging connection.

## Mains safety

The SSR is not a safety device and the touchscreen Stop does not replace one.
Keep a fused live, a physical disconnect, an earthed chassis and an independent
thermal cutoff that opens the heater circuit even if the SSR fails short.
Mains wiring needs appropriate competence. Never leave it running unattended.

## Build and flash

```sh
pio run
pio run -t upload
```

## Profiles

- **Leaded reflow trial (SMD291AXT5):** 150°C at 1°C/s, 60 s soak, 205°C at 1.2°C/s,
  20 s hold, then heater off. Heater capped at 50%, trip above 220°C, 30 min deadline.
- **SAC305:** present but locked.
- **Annealing:** temperature, soak time and ramp rate set on three dials.
- **Heater tests and PID tuning:** 100, 150 and 200°C holds, relay autotune and
  control checks.

Every run has a total deadline. Probe faults, stale sensor frames, overtemperature
and timeouts latch a fault and turn the heater command off.

## Logging

```sh
python3 tools/capture_study.py --port /dev/cu.usbmodem1101 logs/run.csv
```

Writes the USB telemetry to CSV and never overwrites an existing file. Ctrl-C stops
the capture only, not the heater.

## Tests

Host-side tests need a C++11 compiler, and `pyserial` for the logger test.

```sh
sh test/run_control_tests.sh
python3 test/capture_study_test.py
```
