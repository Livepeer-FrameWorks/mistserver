#!/usr/bin/env python3
"""The timeout guard must finish and report a detached survivor before Meson's kill."""
import contextlib
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time

with tempfile.TemporaryDirectory(prefix='mist-guard-deadline-') as directory:
    root_marker = Path(directory) / 'root.pid'
    detached_marker = Path(directory) / 'detached.pid'
    detached = ('import os,signal,time;from pathlib import Path;'
                'signal.signal(signal.SIGTERM,signal.SIG_IGN);'
                f'Path({str(detached_marker)!r}).write_text(str(os.getpid()));time.sleep(60)')
    body = ('import os,signal,time,subprocess,sys;from pathlib import Path;'
            'signal.signal(signal.SIGTERM,signal.SIG_IGN);'
            f'subprocess.Popen([sys.executable,"-c",{detached!r}],start_new_session=True);'
            f'Path({str(root_marker)!r}).write_text(str(os.getpid()));time.sleep(60)')
    guard = subprocess.Popen([sys.executable, sys.argv[1], sys.executable, '-c', body],
                             start_new_session=True, stderr=subprocess.PIPE)
    owned = []
    try:
        deadline = time.monotonic() + 5
        while not (root_marker.exists() and detached_marker.exists()) and time.monotonic() < deadline:
            time.sleep(.01)
        assert root_marker.exists() and detached_marker.exists(), 'fixture children did not start'
        owned = [int(root_marker.read_text()), int(detached_marker.read_text())]
        started = time.monotonic()
        guard.send_signal(signal.SIGTERM)
        try:
            _, diagnostics = guard.communicate(timeout=.45)
        except subprocess.TimeoutExpired:
            raise AssertionError('guard cleanup missed Meson\'s 0.5-second SIGKILL deadline')
        assert guard.returncode == -signal.SIGTERM, guard.returncode
        assert str(owned[1]).encode() in diagnostics and b'leaked adopted descendants' in diagnostics.lower(), diagnostics
        for pid in owned:
            assert not Path('/proc/%d' % pid).exists(), ('timeout survivor', pid)
        print('PASS guard reported and removed detached timeout survivors in %.3fs' % (time.monotonic() - started))
    finally:
        if guard.poll() is None:
            guard.kill()
            guard.wait()
        for marker in (root_marker, detached_marker):
            if marker.exists():
                with contextlib.suppress(ProcessLookupError):
                    import os
                    os.kill(int(marker.read_text()), signal.SIGKILL)
