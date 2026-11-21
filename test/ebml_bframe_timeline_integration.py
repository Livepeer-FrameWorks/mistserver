#!/usr/bin/env python3
"""Keep video/audio presentation alignment while deriving DTS from Matroska."""
from pathlib import Path
import json
import os
import subprocess
import sys
from controller_capacity_lifecycle_integration import Fixture

build, ffmpeg, ffprobe = Path(sys.argv[1]).resolve(), sys.argv[2], sys.argv[3]
f = Fixture(build / 'MistController', ffmpeg)
f.env['UDP_API'] = 'udp://127.0.0.1:0'
success = False
try:
    source, output = f.root / 'bframes.mkv', f.root / 'recording.mp4'
    subprocess.run([ffmpeg, '-v', 'error', '-y', '-f', 'lavfi', '-i', 'testsrc2=size=320x180:rate=10:duration=8',
                    '-f', 'lavfi', '-i', 'sine=frequency=440:sample_rate=48000:duration=8',
                    '-c:v', 'libx264', '-threads', '1', '-preset', 'fast', '-bf', '2', '-g', '10', '-c:a', 'aac', str(source)],
                   check=True, timeout=30)
    f.start(streams_override={f.stream: {'source': str(source)}})
    with (f.root / 'recording.log').open('wb') as log:
        subprocess.run([str(build / 'MistOutMP4'), '-s', f.stream, str(output) + '?video=all&audio=all&subtitle=none&meta=none'],
                       env=f.env, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=40)
    def packets(path, selector):
        return json.loads(subprocess.check_output([ffprobe, '-v', 'error', '-select_streams', selector, '-show_packets', '-of', 'json', str(path)], text=True))['packets']
    shifts = []
    for selector in ('v:0', 'a:0'):
        original, recorded = packets(source, selector), packets(output, selector)
        assert len(original) == len(recorded), (selector, 'missing packets', len(original), len(recorded))
        changes = [float(after['pts_time']) - float(before['pts_time']) for before, after in zip(original, recorded)]
        assert max(changes) - min(changes) < 0.003, (selector, 'presentation times changed unevenly', changes)
        shifts.append(changes[0])
        times = [float(packet['dts_time']) for packet in recorded]
        assert all(after > before for before, after in zip(times, times[1:])), (selector, 'DTS is clamped or non-monotonic', times[:10])
    assert abs(shifts[0] - shifts[1]) < 0.025, ('video/audio sync shifted', shifts)
    decoded = []
    for path in (source, output):
        result = subprocess.run([ffmpeg, '-v', 'error', '-xerror', '-i', str(path), '-map', '0:v:0', '-fps_mode', 'passthrough', '-pix_fmt', 'yuv420p', '-f', 'rawvideo', '-'], capture_output=True, timeout=30)
        assert result.returncode == 0 and not result.stderr.strip(), result.stderr.decode(errors='replace')
        decoded.append(result.stdout)
    assert decoded[0] == decoded[1], ('passthrough lost or changed decoded frames', len(decoded[0]), len(decoded[1]))
    success = True
    print('PASS Matroska B-frame MP4: every frame retained, monotonic DTS, unchanged A/V presentation alignment', flush=True)
finally:
    f.close(failed=not success)
