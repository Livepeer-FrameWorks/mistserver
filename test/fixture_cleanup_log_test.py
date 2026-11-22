#!/usr/bin/env python3
"""Teardown diagnostics must not overwrite the controller log used as evidence."""
from pathlib import Path
import os
import signal
import subprocess
import sys
import tempfile
from unittest.mock import patch

from controller_capacity_lifecycle_integration import Fixture

with tempfile.TemporaryDirectory(prefix='mist-cleanup-log-') as directory:
    binary = Path(directory) / 'MistController'
    nuke = binary.with_name('MistUtilNuke')
    nuke.write_text('#!/bin/sh\nprintf "cleanup must use a separate diagnostic file\\n"\n')
    nuke.chmod(0o755)
    fixture = Fixture(binary, 'ffmpeg')
    child = None
    try:
        fixture.streams.add(fixture.stream)
        child = subprocess.Popen([sys.executable, '-c', 'import time;time.sleep(60)'],
                                 env=fixture.env, start_new_session=True,
                                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        fixture.process = child
        fixture.group = os.getpgid(child.pid)
        original = 'controller evidence must stay intact\n'
        fixture.log.write(original)
        fixture.log.flush()
        fixture.log.seek(0)
        with patch.object(fixture.temp, 'cleanup'):
            fixture.close()
        assert (fixture.root / 'controller.log').read_text() == original, 'fixture teardown overwrote the controller evidence'
        assert 'cleanup must use' in (fixture.root / 'cleanup.log').read_text()
        print('PASS fixture teardown preserves controller evidence and saves separate cleanup diagnostics')
    finally:
        if child and child.poll() is None:
            child.kill()
            child.wait()
        fixture.temp.cleanup()
