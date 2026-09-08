#!/usr/bin/env python3
"""Capture non-blocking firmware TRACE records. Never sends heater commands."""
import argparse
import csv
import datetime
import serial

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("output", help="New CSV path (existing files are never overwritten)")
parser.add_argument("--port", default="/dev/cu.usbmodem1101")
args = parser.parse_args()
columns = "host_utc,uptime_ms,run_s,recipe,state,phase,temp_c,target_c,demand_pct,ssr_on,kp,ki,kd,point,attempt,peak_c,rise_to_96_s,hold_rmse_c,hold_mean_demand_pct,fault,tune_cycles,probe_valid,start_c,candidate_kp,candidate_ki,candidate_kd,tune_period_s,tune_amplitude_c,candidate_ready".split(",")
with open(args.output, "x", newline="") as output, serial.Serial(args.port, 115200, timeout=1) as port:
    writer = csv.writer(output)
    writer.writerow(columns)
    output.flush()
    print("Recording; Ctrl-C stops capture only, NOT the heater. Use touchscreen STOP.")
    try:
        while True:
            line = port.readline().decode("ascii", errors="replace").strip()
            if not line.startswith("TRACE,"):
                continue
            row = line.split(",")[1:]
            if len(row) != len(columns) - 1:
                continue
            writer.writerow([datetime.datetime.now(datetime.timezone.utc).isoformat()] + row)
            output.flush()
    except KeyboardInterrupt:
        print("Capture stopped. Heater state unchanged.")
