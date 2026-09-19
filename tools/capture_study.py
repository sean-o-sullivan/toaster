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
diagnostic_path = args.output + ".tune.csv"
assessment_path = args.output + ".assessment.csv"
tune_columns = ["host_utc", "record"]
with open(args.output, "x", newline="") as output, open(diagnostic_path, "x", newline="") as diagnostic, open(assessment_path, "x", newline="") as assessment, serial.Serial(args.port, 115200, timeout=1) as port:
    writer = csv.writer(output)
    tune_writer = csv.writer(diagnostic)
    assessment_writer = csv.writer(assessment)
    writer.writerow(columns)
    tune_writer.writerow(tune_columns)
    assessment_writer.writerow(tune_columns)
    output.flush()
    diagnostic.flush()
    assessment.flush()
    print("Recording; Ctrl-C stops capture only, NOT the heater. Use touchscreen STOP.")
    try:
        while True:
            line = port.readline().decode("ascii", errors="replace").strip()
            now = datetime.datetime.now(datetime.timezone.utc).isoformat()
            if line.startswith("TRACE,"):
                row = line.split(",")[1:]
                if len(row) == len(columns) - 1:
                    writer.writerow([now] + row)
                    output.flush()
            elif line.startswith(("TUNE,", "TUNEW,", "TUNEH,", "TUNESTAGE,")):
                tune_writer.writerow([now, line])
                diagnostic.flush()
            elif line.startswith(("RUNMETA,", "CHECK,", "VALIDATION,", "METRIC,")):
                assessment_writer.writerow([now, line])
                assessment.flush()
    except KeyboardInterrupt:
        print("Capture stopped. Heater state unchanged.")
