#!/usr/bin/env python3
"""Use valid EAC3 frames with different block counts in the real ProcAV call path."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

binary, ffmpeg = sys.argv[1:]
with tempfile.TemporaryDirectory(prefix='mist-audio-call-') as directory:
    paths = []
    for rate, bitrate in ((48000, '4000k'), (32000, '384k')):
        path = Path(directory) / (str(rate) + '.eac3')
        subprocess.run([ffmpeg, '-v', 'error', '-f', 'lavfi', '-i', 'sine=frequency=701:sample_rate=%d:duration=0.2' % rate,
                        '-c:a', 'eac3', '-ac', '2', '-b:a', bitrate, str(path)], check=True, timeout=20)
        paths.append(str(path))
    subprocess.run([binary, *paths], check=True, timeout=20)
    control = subprocess.run([binary, *paths], env=dict(os.environ, MIST_AUDIO_BACKLOG_CONTROL='1'),
                             capture_output=True, text=True, timeout=20)
    assert control.returncode == 1 and 'swr_get_delay exceeds128 samples before EOF' in control.stderr, (
        'the direct resampler backlog guard did not reject a one-sample output buffer',
        control.returncode, control.stdout, control.stderr)
    print('PASS direct resampler backlog negative control:', control.stderr.strip(), flush=True)
