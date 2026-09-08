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
        time.sleep(0.2)
        child.send_signal(signal.SIGINT)
        stdout, stderr = child.communicate(timeout=3)
        assert child.returncode == 0, stderr
        with output.open() as stream:
            rows = list(csv.reader(stream))
        assert len(rows) == 2, rows
        assert len(rows[0]) == len(rows[1]) == 29
        assert rows[1][1:] == fields
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
