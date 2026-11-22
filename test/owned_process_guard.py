#!/usr/bin/env python3
"""Reject descendants a test leaves alive, including children that call setsid."""
import ctypes
import os
from pathlib import Path
import signal
import subprocess
import sys
import time


def children():
    path = Path('/proc/self/task') / str(os.getpid()) / 'children'
    return [int(pid) for pid in path.read_text().split()]


def reap():
    while True:
        try:
            pid, _ = os.waitpid(-1, os.WNOHANG)
        except ChildProcessError:
            return
        if not pid:
            return


def cleanup(kill_window=5):
    deadline = time.monotonic() + 0.5
    while True:
        reap()
        alive = children()
        if not alive or time.monotonic() >= deadline:
            break
        time.sleep(0.02)
    if not alive:
        return False
    print('Test leaked adopted descendants: ' + ', '.join(map(str, alive)), file=sys.stderr, flush=True)
    # Every PID comes from this wrapper's own adopted children. Killing a parent
    # adopts any further descendants, so repeat until all owned children exit.
    deadline = time.monotonic() + kill_window
    while alive and time.monotonic() < deadline:
        for pid in alive:
            try:
                os.kill(pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
        time.sleep(0.02)
        reap()
        alive = children()
    if alive:
        print('Owned descendants survived cleanup: ' + ', '.join(map(str, alive)), file=sys.stderr)
    return True


def main():
    if not sys.platform.startswith('linux'):
        raise RuntimeError('the owned-process test guard requires Linux subreaper support')
    libc = ctypes.CDLL(None, use_errno=True)
    if libc.prctl(36, 1, 0, 0, 0):  # PR_SET_CHILD_SUBREAPER
        raise OSError(ctypes.get_errno(), 'could not become a test subreaper')
    child = subprocess.Popen(sys.argv[1:], start_new_session=True)

    stop_signal = 0
    stop_deadline = None

    def stop(signum, _frame):
        nonlocal stop_signal, stop_deadline
        if not stop_signal:
            stop_signal = signum
            stop_deadline = time.monotonic() + 1
        try:
            os.killpg(child.pid, signum)
            os.killpg(child.pid, signal.SIGCONT)
        except ProcessLookupError:
            pass

    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    try:
        while child.poll() is None:
            if stop_deadline is not None and time.monotonic() >= stop_deadline:
                try:
                    os.killpg(child.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
            time.sleep(0.02)
        result = child.wait()
    finally:
        leaked = cleanup(kill_window=2 if stop_signal else 5)
    if stop_signal:
        result = -stop_signal
    if result < 0:
        signum = -result
        if signum not in (signal.SIGKILL, signal.SIGSTOP):
            signal.signal(signum, signal.SIG_DFL)
        os.kill(os.getpid(), signum)
    return 1 if leaked else result


if __name__ == '__main__':
    sys.exit(main())
