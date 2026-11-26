#!/usr/bin/env python3
"""Ranged playback (start/duration) covers one presentation interval on every track.

The range snaps to a video keyframe's decode time; the keyframe presents a reorder delay
later. Audio must start where that keyframe presents, as a ranged recording does, so the
delivered tracks share one interval instead of audio leading video by the reorder delay.
"""
from pathlib import Path
import json
import subprocess
import sys
import urllib.request
from controller_capacity_lifecycle_integration import Fixture

build, ffmpeg, ffprobe, target = Path(sys.argv[1]).resolve(), sys.argv[2], sys.argv[3], sys.argv[4]
f = Fixture(build / 'MistController', ffmpeg)
f.env['UDP_API'] = 'udp://127.0.0.1:0'
success = False


def packets(path, selector):
    probe = json.loads(subprocess.check_output([ffprobe, '-v', 'error', '-select_streams', selector, '-show_packets',
                                                '-of', 'json', str(path)], env=f.tool_env, text=True, timeout=30))
    return probe['packets']


try:
    source, output = f.root / 'source.mp4', f.root / ('range.' + target)
    subprocess.run([ffmpeg, '-v', 'error', '-y', '-f', 'lavfi', '-i', 'testsrc2=size=320x180:rate=10:duration=8',
                    '-f', 'lavfi', '-i', 'sine=frequency=440:sample_rate=48000:duration=8', '-c:v', 'libx264',
                    '-threads', '1', '-preset', 'veryfast', '-g', '20', '-c:a', 'aac', str(source)],
                   env=f.tool_env, check=True, timeout=30)
    f.start(streams_override={f.stream: {'source': str(source)}}, extra_protocols=[{'connector': 'HTTPTS'}])
    with urllib.request.urlopen('http://127.0.0.1:%d/json_%s.js' % (f.http, f.stream), timeout=30) as response:
        tracks = {t['type']: t for t in json.load(response)['meta']['tracks'].values()}
    start = tracks['video']['firstms'] + 2000  # the second keyframe's decode time
    url = 'http://127.0.0.1:%d/%s.%s?start=%d&duration=3' % (f.http, f.stream, target, start)
    with urllib.request.urlopen(url, timeout=30) as response:
        output.write_bytes(response.read())
    video, audio = packets(output, 'v:0'), packets(output, 'a:0')
    assert video and audio, ('range lost a track', len(video), len(audio))
    key = next(p for p in video if 'K' in p['flags'])
    gap = float(audio[0]['pts_time']) - float(key['pts_time'])
    assert abs(gap) < 0.022, ('%s range starts audio %+.3f s from the keyframe presentation' % (target, gap))
    if target == 'mp4':
        duration = float(json.loads(subprocess.check_output([ffprobe, '-v', 'error', '-show_entries', 'format=duration',
                                                             '-of', 'json', str(output)], env=f.tool_env, text=True))['format']['duration'])
        assert abs(duration - 3) < 0.002, ('mp4 range does not cover one 3 s presentation interval', duration)
    success = True
    print('PASS %s range: audio starts with the keyframe presentation (gap %+.3f s)' % (target, gap), flush=True)
finally:
    f.close(failed=not success)
