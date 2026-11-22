import os
from pathlib import Path
import tempfile
import time

with tempfile.TemporaryDirectory(prefix='mist-adopted-zombie-') as directory:
    marker = Path(directory) / 'grandchild.pid'
    child = os.fork()
    if child == 0:
        grandchild = os.fork()
        if grandchild == 0:
            temporary = marker.with_suffix('.tmp')
            temporary.write_text(str(os.getpid()))
            temporary.replace(marker)
            os._exit(0)
        os._exit(0)
    os.waitpid(child, 0)
    deadline = time.monotonic() + 3
    while not marker.exists() and time.monotonic() < deadline:
        time.sleep(.01)
    assert marker.exists()
    pid = int(marker.read_text())
    while Path('/proc/%d' % pid).exists() and time.monotonic() < deadline:
        time.sleep(.02)
    assert not Path('/proc/%d' % pid).exists(), 'the process audit kept a dead adopted child visible as a live PID until the test exited'
    print('PASS adopted zombie reaped while test remains alive')
