#!/usr/bin/env python3
"""B-frame video keeps its source alignment with audio from every input container to every output.

Each output may move the whole presentation timeline, but video and audio must move alike.
"""
from pathlib import Path
import json
import statistics
import subprocess
import sys
import urllib.parse
import urllib.request
from controller_capacity_lifecycle_integration import Fixture

build, ffmpeg, ffprobe, container, target = Path(sys.argv[1]).resolve(), sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5]
f = Fixture(build / 'MistController', ffmpeg)
f.env['UDP_API'] = 'udp://127.0.0.1:0'
success = False


def packets(path, selector):
    probe = json.loads(subprocess.check_output([ffprobe, '-v', 'error', '-select_streams', selector, '-show_packets',
                                                '-of', 'json', str(path)], env=f.tool_env, text=True, timeout=30))
    return sorted(float(p['pts_time']) for p in probe['packets'] if 'pts_time' in p)


def fetch(url):
    with urllib.request.urlopen(url, timeout=30) as response:
        assert response.status == 200, ('request refused', url, response.status)
        return response.read()


try:
    # x264 B-frames at 10 fps put two frames (200 ms) of reorder delay before the first presentation;
    # an MP4 source carries it in its video edit list and its AAC priming in the audio edit list.
    source, output = f.root / ('source.' + container), f.root / ('output.' + ('mp4' if target == 'mp4' else 'ts'))
    subprocess.run([ffmpeg, '-v', 'error', '-y', '-f', 'lavfi', '-i', 'testsrc2=size=320x180:rate=10:duration=8',
                    '-f', 'lavfi', '-i', 'sine=frequency=440:sample_rate=48000:duration=8', '-c:v', 'libx264',
                    '-threads', '1', '-preset', 'veryfast', '-g', '20', '-c:a', 'aac',
                    *(['-pes_payload_size', '0'] if container == 'ts' else []), str(source)],
                   env=f.tool_env, check=True, timeout=30)
    f.start(streams_override={f.stream: {'source': str(source)}}, extra_protocols=[{'connector': 'HTTPTS'}])
    if target == 'mp4':
        with (f.root / 'recording.log').open('wb') as log:
            subprocess.run([str(build / 'MistOutMP4'), '-s', f.stream, str(output) + '?video=all&audio=all&subtitle=none&meta=none'],
                           env=f.env, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=40)
    elif target == 'ts':
        output.write_bytes(fetch('http://127.0.0.1:%d/%s.ts' % (f.http, f.stream)))
    else:
        playlist = 'http://127.0.0.1:%d/hls/%s/index.m3u8' % (f.http, f.stream)
        for _ in range(3):
            uris = [line.strip() for line in fetch(playlist).decode().splitlines() if line.strip() and not line.startswith('#')]
            assert uris, 'HLS playlist contains no media'
            if not urllib.parse.urlparse(uris[0]).path.endswith('.m3u8'):
                break
            playlist = urllib.parse.urljoin(playlist, uris[0])
        output.write_bytes(b''.join(fetch(urllib.parse.urljoin(playlist, uri)) for uri in uris))
    shifts = {}
    for selector in ('v:0', 'a:0'):
        original, delivered = packets(source, selector), packets(output, selector)
        common = min(len(original), len(delivered))
        assert common >= 0.9 * len(original), (selector, 'output lost media', len(original), len(delivered))
        shifts[selector] = statistics.median(after - before for before, after in zip(original[:common], delivered[:common]))
    drift = shifts['v:0'] - shifts['a:0']
    assert abs(drift) < 0.012, ('%s input to %s output moved video %+.3f s against audio' % (container, target, drift), shifts)
    success = True
    print('PASS %s -> %s: video and audio keep their source alignment (drift %+.3f s)' % (container, target, drift), flush=True)
finally:
    f.close(failed=not success)
