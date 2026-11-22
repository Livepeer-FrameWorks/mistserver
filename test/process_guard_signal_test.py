#!/usr/bin/env python3
"""A timed-out test that ignores SIGTERM must not strand its guard or children."""
from pathlib import Path
import os
import signal
import subprocess
import sys
import tempfile
import time

if not sys.platform.startswith('linux'):
    sys.exit(77)

with tempfile.TemporaryDirectory(prefix='mist-guard-signal-') as directory:
    marker = Path(directory) / 'child.pid'
    body = ('import os,signal,time;from pathlib import Path;'
            'signal.signal(signal.SIGTERM,signal.SIG_IGN);'
            f'Path({str(marker)!r}).write_text(str(os.getpid()));time.sleep(60)')
    guard = subprocess.Popen([sys.executable, sys.argv[1], sys.executable, '-c', body], start_new_session=True)
    child = None
    try:
        deadline = time.monotonic() + 5
        while not marker.exists() and guard.poll() is None and time.monotonic() < deadline:
            time.sleep(0.02)
        assert marker.exists(), 'the owned child never became ready'
        child = int(marker.read_text())
        guard.send_signal(signal.SIGTERM)
        assert guard.wait(timeout=4) == -signal.SIGTERM, 'guard did not preserve the requested stop signal'
        try:
            os.kill(child, 0)
        except ProcessLookupError:
            pass
        else:
            raise AssertionError('the stopped test left its owned child alive')
        print('PASS guard bounded a SIGTERM-ignoring child and reaped it')
    finally:
        if guard.poll() is None:
            guard.kill()
            guard.wait()
        if child is not None:
            try:
                os.kill(child, signal.SIGKILL)
            except ProcessLookupError:
                pass
