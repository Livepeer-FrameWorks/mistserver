#!/usr/bin/env python3
"""Keep video/audio presentation alignment while deriving DTS from Matroska."""
from pathlib import Path
import json
import os
import subprocess
import sys
from controller_capacity_lifecycle_integration import Fixture
from h264_strip_restriction import fix

build, ffmpeg, ffprobe = Path(sys.argv[1]).resolve(), sys.argv[2], sys.argv[3]
f = Fixture(build / 'MistController', ffmpeg)
f.env['UDP_API'] = 'udp://127.0.0.1:0'
success = False
try:
    source, output = f.root / ('bframes.' + os.environ.get('MIST_TIMELINE_CONTAINER', 'mkv')), f.root / 'recording.mp4'
    fps = int(os.environ.get('MIST_TIMELINE_FPS', '10'))
    duration = int(os.environ.get('MIST_TIMELINE_DURATION', '8'))
    subprocess.run([ffmpeg, '-v', 'error', '-y', '-f', 'lavfi', '-i', 'testsrc2=size=320x180:rate=%d:duration=%d' % (fps, duration),
                    '-f', 'lavfi', '-i', 'sine=frequency=440:sample_rate=48000:duration=%d' % duration,
                    '-c:v', 'libx264', '-threads', '1', '-preset', 'fast', '-bf', '3' if os.environ.get('MIST_TIMELINE_FPS') else '2', '-g', str(fps), '-c:a', 'aac', *(['-output_ts_offset', os.environ['MIST_TIMELINE_ORIGIN']] if 'MIST_TIMELINE_ORIGIN' in os.environ else []), str(source)],
                   check=True, timeout=30)
    if os.environ.get('MIST_TIMELINE_STRIP'):
        data = source.read_bytes()
        probe = json.loads(subprocess.check_output([ffprobe, '-v', 'error', '-select_streams', 'v:0',
            '-show_streams', '-show_data', '-of', 'json', str(source)], text=True))
        dump = probe['streams'][0]['extradata']
        avcc = bytes.fromhex(''.join(line.split(':', 1)[1].split('  ', 1)[0].strip()
                                    for line in dump.splitlines() if ':' in line))
        length = int.from_bytes(avcc[6:8], 'big')
        sps = avcc[8:8 + length]
        replacement = fix(sps)
        assert replacement != sps and len(replacement) <= length, 'fixture did not remove restriction'
        assert data.count(sps) == 1, 'ambiguous CodecPrivate SPS'
        source.write_bytes(data.replace(sps, replacement.ljust(length, b'\x00')))
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
    if os.environ.get('MIST_TIMELINE_FPS'):
        def boxes(data, begin=0, end=None):
            end = len(data) if end is None else end
            while begin < end:
                size = int.from_bytes(data[begin:begin + 4], 'big')
                assert size >= 8 and begin + size <= end, ('bad MP4 box', begin, size)
                yield data[begin + 4:begin + 8], data[begin + 8:begin + size]
                begin += size
        moov = next(payload for kind, payload in boxes(output.read_bytes()) if kind == b'moov')
        for kind, track in boxes(moov):
            if kind != b'trak':
                continue
            children = dict(boxes(track))
            media = dict(boxes(children[b'mdia']))
            if media[b'hdlr'][8:12] != b'vide':
                continue
            table = dict(boxes(dict(boxes(media[b'minf']))[b'stbl']))
            stts = table[b'stts']
            entries = [(int.from_bytes(stts[i:i + 4], 'big'), int.from_bytes(stts[i + 4:i + 8], 'big'))
                       for i in range(8, len(stts), 8)]
            mdhd = media[b'mdhd']
            assert mdhd[0] == 0
            scale = int.from_bytes(mdhd[12:16], 'big')
            span = int.from_bytes(mdhd[16:20], 'big')
            assert min(delta for count, delta in entries) >= scale / fps - 1, ('startup STTS samples are compressed', entries[:8])
            assert abs(span / scale - duration) < 0.002, ('MP4 decode duration is short', span, scale, duration)
            assert sum(count * delta for count, delta in entries) == span, 'MDHD differs from STTS'
            edits = dict(boxes(children[b'edts']))[b'elst']
            media_times = [int.from_bytes(edits[i + 4:i + 8], 'big') for i in range(8, len(edits), 12)]
            assert any(0 < time < 0xffffffff for time in media_times), 'decode lead is missing from edit media_time'
        stream = json.loads(subprocess.check_output([ffprobe, '-v', 'error', '-select_streams', 'v:0',
            '-show_streams', '-of', 'json', str(output)], text=True))['streams'][0]
        assert stream['r_frame_rate'] == '%d/1' % fps, ('wrong MP4 frame rate', stream['r_frame_rate'])
        print('PASS progressive MP4 startup: frames=%s rate=%s STTS=%s MDHD=%d/%d edit media_times=%s' %
              (stream['nb_frames'], stream['r_frame_rate'], entries[:8], span, scale, media_times), flush=True)
    success = True
    print('PASS Matroska B-frame MP4: every frame retained, monotonic DTS, unchanged A/V presentation alignment', flush=True)
finally:
    f.close(failed=not success)
