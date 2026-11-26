#!/usr/bin/env python3
"""A ranged MP4 recording that starts at an open-GOP keyframe keeps video and audio aligned.

Leading pictures of an open GOP present before their keyframe but reference the previous
GOP. The recording starts at the keyframe, so its edit list must start video at the
keyframe's presentation, the same point the audio track and the movie timeline start from.
"""
from pathlib import Path
import json
import subprocess
import sys
import urllib.request
from controller_capacity_lifecycle_integration import Fixture

build, ffmpeg, ffprobe, codec = Path(sys.argv[1]).resolve(), sys.argv[2], sys.argv[3], sys.argv[4]
f = Fixture(build / 'MistController', ffmpeg)
f.env['UDP_API'] = 'udp://127.0.0.1:0'
success = False


def packets(path, selector):
    probe = json.loads(subprocess.check_output([ffprobe, '-v', 'error', '-select_streams', selector, '-show_packets',
                                                '-show_data_hash', 'MD5', '-of', 'json', str(path)],
                                               env=f.tool_env, text=True, timeout=30))
    return probe['packets']


try:
    source, output = f.root / 'source.mkv', f.root / 'clip.mp4'
    encoder = ['libx264', '-x264-params', 'open-gop=1:keyint=20:min-keyint=20:scenecut=0:bframes=3'] if codec == 'h264' else \
              ['libx265', '-x265-params', 'open-gop=1:keyint=20:min-keyint=20:scenecut=0:bframes=3:pools=1:frame-threads=1:log-level=error']
    subprocess.run([ffmpeg, '-v', 'error', '-y', '-f', 'lavfi', '-i', 'testsrc2=size=320x180:rate=10:duration=8',
                    '-f', 'lavfi', '-i', 'sine=frequency=440:sample_rate=48000:duration=8', '-c:v', *encoder,
                    '-threads', '1', '-c:a', 'aac', str(source)], env=f.tool_env, check=True, timeout=60)
    video = packets(source, 'v:0')
    keys = [i for i, p in enumerate(video) if 'K' in p['flags']]
    leading = [k for k in keys[1:] if any(float(p['pts_time']) < float(video[k]['pts_time']) for p in video[k + 1:k + 6])]
    assert leading, ('fixture has no open-GOP keyframe with leading pictures', codec)
    key = leading[0]
    f.start(streams_override={f.stream: {'source': str(source)}})
    # The video decode clock keeps the frame cadence, so the keyframe decodes this far after the first.
    with urllib.request.urlopen('http://127.0.0.1:%d/json_%s.js' % (f.http, f.stream), timeout=30) as response:
        tracks = {t['type']: t for t in json.load(response)['meta']['tracks'].values()}
    start = round((float(video[key]['pts_time']) - float(video[0]['pts_time'])) * 1000) + tracks['video']['firstms']
    with (f.root / 'recording.log').open('wb') as log:
        subprocess.run([str(build / 'MistOutMP4'), '-s', f.stream, str(output) + '?start=%d&duration=3' % start],
                       env=f.env, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=40)
    recorded_video, recorded_audio, source_audio = packets(output, 'v:0'), packets(output, 'a:0'), packets(source, 'a:0')
    by_hash = {p['data_hash']: i for i, p in enumerate(video)}
    first = by_hash[recorded_video[0]['data_hash']]
    assert 'K' in video[first]['flags'], ('recording did not start at a keyframe', first)
    # The keyframe presents at the start of the movie's video; leading pictures may only fall before it.
    video_shift = float(recorded_video[0]['pts_time']) - float(video[first]['pts_time'])
    audio_index = {p['data_hash']: i for i, p in enumerate(source_audio)}[recorded_audio[0]['data_hash']]
    audio_shift = float(recorded_audio[0]['pts_time']) - float(source_audio[audio_index]['pts_time'])
    drift = video_shift - audio_shift
    assert abs(drift) < 0.012, ('%s open-GOP clip moved video %+.3f s against audio' % (codec, drift), video_shift, audio_shift)
    shown = subprocess.run([ffmpeg, '-v', 'error', '-i', str(output), '-map', '0:v:0', '-f', 'null', '-'],
                           env=f.tool_env, capture_output=True, text=True, timeout=30)
    assert shown.returncode == 0, shown.stderr
    success = True
    print('PASS %s open-GOP clip: video starts at the keyframe in step with audio (drift %+.3f s)' % (codec, drift), flush=True)
finally:
    f.close(failed=not success)
