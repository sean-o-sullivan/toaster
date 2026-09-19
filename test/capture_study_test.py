"""Exercise CSV capture against a pseudo-terminal, never an attached oven."""
import csv
import os
from pathlib import Path
import pty
import signal
import subprocess
import sys
import tempfile
import time

tool = Path(__file__).resolve().parents[1] / "tools" / "capture_study.py"
with tempfile.TemporaryDirectory() as directory:
    output = Path(directory) / "test.csv"
    master, slave = pty.openpty()
    child = subprocess.Popen([sys.executable, str(tool), str(output), "--port", os.ttyname(slave)],
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        time.sleep(0.5)
        assert child.poll() is None
        os.write(master, b"MAX P raw=00881620 fault=0 tc=8.50 cj=22.12\nTRACE,bad\n")
        fields = [str(i) for i in range(28)]
        os.write(master, ("TRACE," + ",".join(fields) + "\n").encode())
        tune_record = "TUNE," + ",".join(str(i) for i in range(21))
        window_record = "TUNEW," + ",".join(str(i) for i in range(12))
        os.write(master, (tune_record + "\n" + window_record + "\n").encode())
        assessment_record = "CHECK,1,0,100,2,1"
        recipe_record = "RUNMETA,2,14,2,3,60,0.166666667,0,7200,200,100"
        os.write(master, (assessment_record + "\n" + recipe_record + "\n").encode())
        time.sleep(0.2)
        child.send_signal(signal.SIGINT)
        stdout, stderr = child.communicate(timeout=3)
        assert child.returncode == 0, stderr
        with output.open() as stream:
            rows = list(csv.reader(stream))
        assert len(rows) == 2, rows
        assert len(rows[0]) == len(rows[1]) == 29
        assert rows[1][1:] == fields
        with Path(str(output) + ".tune.csv").open() as stream:
            tune_rows = list(csv.reader(stream))
        assert len(tune_rows) == 3
        assert tune_rows[1][1] == tune_record and tune_rows[2][1] == window_record
        with Path(str(output) + ".assessment.csv").open() as stream:
            assessment_rows = list(csv.reader(stream))
        assert len(assessment_rows) == 3 and assessment_rows[1][1] == assessment_record
        assert assessment_rows[2][1] == recipe_record
        existing = output.read_bytes()
        refusal = subprocess.run([sys.executable, str(tool), str(output), "--port", os.ttyname(slave)],
                                 capture_output=True)
        assert refusal.returncode != 0 and output.read_bytes() == existing
    finally:
        if child.poll() is None:
            child.kill()
            child.wait()
        os.close(master)
        os.close(slave)
print("CSV capture schema, filtering and overwrite protection passed")
