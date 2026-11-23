#!/usr/bin/env python3
"""An exhausted, stopped audio rendition must not be reselected by a bounded recording."""
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
from controller_capacity_lifecycle_integration import Fixture, until

build, ffmpeg, ffprobe = Path(sys.argv[1]).resolve(), sys.argv[2], sys.argv[3]
f = Fixture(build / 'MistController', ffmpeg)
f.env['UDP_API'] = 'udp://127.0.0.1:0'
success = False
try:
    source = f.root / 'source.mkv'
    subprocess.run([ffmpeg, '-v', 'error', '-y', '-f', 'lavfi', '-i', 'testsrc2=size=320x180:rate=10:duration=8',
                    '-f', 'lavfi', '-i', 'sine=frequency=701:sample_rate=48000:duration=8',
                    '-c:v', 'libx264', '-preset', 'ultrafast', '-threads', '1', '-bf', '0', '-g', '10',
                    '-c:a', 'libmp3lame', '-b:a', '128k', str(source)], check=True, env=f.tool_env, timeout=30)
    policy = [{'process': 'AV', 'codec': 'AAC', 'x-LSP-kind': 'audio', 'track_select': 'audio=MP3&video=none',
               'restart_type': 'disabled', 'source_mask': 4, 'target_mask': 3, 'bitrate': 128000}]
    f.start(streams_override={f.stream: {'source': str(source), 'process_controlled_realtime': True,
                                       'realtime_speed': 1, 'processes': policy}})
    input_log = (f.root / 'input.log').open('wb')
    source_process = subprocess.Popen([str(build / 'MistInEBML'), '-g', '4', '-r', '-s', f.stream, str(source)],
                                      env=f.env, stdout=input_log, stderr=subprocess.STDOUT)
    f.children.append(source_process)
    until(lambda: 'VoD metadata has' in (f.root / 'input.log').read_text(errors='replace'),
          15, 'the realtime input never initialized its buffer')
    recording = f.root / 'recording.mkv'
    with (f.root / 'output.log').open('wb') as log:
        child = subprocess.Popen([str(build / 'MistOutEBML'), '-s', f.stream,
                                  str(recording) + '?duration=6&video=all&audio=all&subtitle=none&meta=none'],
                                 env=f.env, stdout=log, stderr=subprocess.STDOUT)
        f.children.append(child)
        def produced_audio():
            records = f.request({'proc_list': f.stream}).get('proc_list') or {}
            for pid, info in records.items():
                if (info.get('process') == 'AV' and not info.get('terminated') and
                        info.get('ainfo', {}).get('sinkTime', 0) >= 1000):
                    return int(pid)
            return None
        pid = until(produced_audio, 15, 'the audio encoder never produced its first second')
        os.kill(pid, signal.SIGTERM)
        assert child.wait(timeout=35) == 0, 'bounded recording failed after its audio producer stopped'
    probe = json.loads(subprocess.check_output([ffprobe, '-v', 'error', '-show_streams', '-show_packets', '-of', 'json',
                                               str(recording)], env=f.tool_env, timeout=15))
    audio = [track for track in probe['streams'] if track['codec_name'] == 'aac']
    assert len(audio) == 1, audio
    packets = [packet for packet in probe['packets'] if packet['stream_index'] == audio[0]['index']]
    times = [float(packet['pts_time']) for packet in packets]
    assert times and times[-1] < 5, ('the stopped encoder was not an early-ending track', times[-1:])
    duplicates = [(before, after) for before, after in zip(times, times[1:]) if after <= before and before > 0]
    assert not duplicates, ('stopped audio rendition replayed its tail', len(packets), len(duplicates), duplicates[:5])
    video = [packet for packet in probe['packets'] if packet['codec_type'] == 'video']
    assert len(video) >= 59, ('stopped audio truncated the continuing video', len(video))
    success = True
    print('PASS stopped AAC rendition retained %d unique packets; continuing video retained %d frames' %
          (len(packets), len(video)), flush=True)
finally:
    if not success:
        for name in ('input.log', 'output.log', 'mist.log'):
            path = f.root / name
            if path.exists():
                print(name + '\n' + path.read_text(errors='replace')[-12000:], file=sys.stderr)
    f.close(failed=not success)
