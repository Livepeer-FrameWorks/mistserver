#!/usr/bin/env python3
"""Observe a real thumbnail directory and verify the session fixture removes it."""
from pathlib import Path
import os
import subprocess
import sys
import time

child = subprocess.Popen(sys.argv[1:])
owned = Path('/tmp/mist_thumbs') / ('sessstop' + str(child.pid))
observed = False
try:
    deadline = time.monotonic() + 150
    while child.poll() is None:
        observed |= owned.is_dir()
        if time.monotonic() >= deadline:
            raise AssertionError('session-stop fixture did not finish')
        time.sleep(.02)
    assert child.returncode == 0, ('session-stop fixture failed', child.returncode)
    assert observed, 'fixture never created its thumbnail directory; cleanup was not exercised'
    assert not owned.exists(), 'session-stop fixture leaked thumbnail directory ' + str(owned)
    print('PASS session-stop fixture removed its observed thumbnail directory', flush=True)
finally:
    if child.poll() is None:
        child.terminate()
        child.wait(timeout=15)
    if owned.is_dir() and not any(owned.iterdir()):
        owned.rmdir()
