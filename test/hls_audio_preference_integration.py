#!/usr/bin/env python3
"""A browser HLS request prefers AAC when E-AC3 is also available."""
from pathlib import Path
import subprocess
import sys
import urllib.request
from controller_capacity_lifecycle_integration import Fixture

build, ffmpeg = Path(sys.argv[1]).resolve(), sys.argv[2]
fixture = Fixture(build / 'MistController', ffmpeg)
success = False
try:
    source = fixture.root / 'dual-audio.mkv'
    subprocess.run([ffmpeg, '-v', 'error', '-y', '-f', 'lavfi', '-i', 'testsrc2=size=320x180:rate=10:duration=4',
                    '-f', 'lavfi', '-i', 'sine=frequency=701:sample_rate=48000:duration=4',
                    '-map', '0:v', '-map', '1:a', '-map', '1:a', '-c:v', 'libx264', '-threads', '1',
                    '-preset', 'ultrafast', '-g', '10', '-bf', '0', '-c:a:0', 'eac3', '-c:a:1', 'aac', str(source)],
                   check=True, timeout=30)
    fixture.start(streams_override={fixture.stream: {'source': str(source)}})
    with urllib.request.urlopen('http://127.0.0.1:%d/hls/%s/index.m3u8' % (fixture.http, fixture.stream), timeout=30) as response:
        playlist = response.read().decode()
    assert 'mp4a.40.2' in playlist and 'ec-3' not in playlist, ('default HLS selected browser-incompatible audio', playlist)
    print('PASS HLS defaults to AAC over an earlier E-AC3 track')
    success = True
finally:
    fixture.close(failed=not success)
