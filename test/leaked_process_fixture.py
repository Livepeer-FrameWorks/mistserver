#!/usr/bin/env python3
"""A deliberately detached child proves the default Meson process audit works."""
import ctypes
import os
from pathlib import Path
import sys
import time

pid = os.fork()
if pid == 0:
    os.setsid()
    ctypes.CDLL(None).prctl(15, b'MistProcLeaked', 0, 0, 0)
    for fd in (0, 1, 2):
        os.close(fd)
    # Bounded even if this fixture is run without its audit wrapper for RED.
    time.sleep(60)
    os._exit(0)
if os.environ.get('MIST_LEAK_PROBE_PID'):
    Path(os.environ['MIST_LEAK_PROBE_PID']).write_text(str(pid))
print('Deliberately left detached Mist child', pid, flush=True)
sys.exit(0)
