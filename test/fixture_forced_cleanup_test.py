#!/usr/bin/env python3
"""A fixture must diagnose a Mist input that requires SIGKILL as a failure."""
import contextlib
from pathlib import Path
import signal
import subprocess
import sys
import time
from controller_capacity_lifecycle_integration import Fixture

fixture = Fixture('/unused/MistController', 'ffmpeg')
marker = fixture.root / 'stubborn.pid'
body = ('import ctypes,os,signal,time;from pathlib import Path;'
        'ctypes.CDLL(None).prctl(15,b"MistInBuffer",0,0,0);'
        'signal.signal(signal.SIGTERM,signal.SIG_IGN);'
        f'Path({str(marker)!r}).write_text(str(os.getpid()));time.sleep(60)')
child = subprocess.Popen([sys.executable, '-c', body], env=fixture.env, start_new_session=True)
try:
    deadline = time.monotonic() + 3
    while not marker.exists() and time.monotonic() < deadline:
        time.sleep(.01)
    assert marker.exists(), 'stubborn input never became ready'
    error = None
    try:
        fixture.close()
    except AssertionError as failure:
        error = failure
    assert error and 'required SIGKILL' in str(error), 'fixture silently killed a leftover Mist input'
    assert child.wait(timeout=2) == -signal.SIGKILL
    print('PASS forced Mist input cleanup is reported as a failed test')
finally:
    if child.poll() is None:
        child.kill()
        child.wait()
    fixture.temp.cleanup()
