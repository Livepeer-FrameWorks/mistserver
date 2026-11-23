#!/usr/bin/env python3
"""Matroska B-frames must retain a valid decode clock in the shared TS writer."""
from pathlib import Path
import json
import subprocess
import sys
import urllib.parse
import urllib.request
from controller_capacity_lifecycle_integration import Fixture


def timestamp(data):
    return ((data[0] & 14) << 29) | (data[1] << 22) | ((data[2] & 254) << 14) | (data[3] << 7) | (data[4] >> 1)


def video_timestamps(data):
    result = []
    assert len(data) % 188 == 0, 'TS response ended within a packet'
    for start in range(0, len(data), 188):
        packet = data[start:start + 188]
        assert packet[0] == 0x47, 'TS packet lost sync'
        if not packet[1] & 0x40 or not packet[3] & 0x10:
            continue
        offset = 4 + (1 + packet[4] if packet[3] & 0x20 else 0)
        pes = packet[offset:]
        if len(pes) < 14 or pes[:3] != b'\x00\x00\x01' or not 0xe0 <= pes[3] <= 0xef:
            continue
        flags = pes[7] >> 6
        assert flags in (2, 3), 'video PES has no presentation timestamp'
        pts = timestamp(pes[9:14])
        dts = timestamp(pes[14:19]) if flags == 3 else pts
        result.append((pts, dts))
    return result


def assert_pid_media_stable(data):
    kinds = {}
    for start in range(0, len(data), 188):
        packet = data[start:start + 188]
        if not packet[1] & 0x40 or not packet[3] & 0x10:
            continue
        offset = 4 + (1 + packet[4] if packet[3] & 0x20 else 0)
        pes = packet[offset:]
        if len(pes) < 4 or pes[:3] != b'\x00\x00\x01' or not 0xc0 <= pes[3] <= 0xef:
            continue
        pid = ((packet[1] & 0x1f) << 8) | packet[2]
        kind = 'video' if pes[3] >= 0xe0 else 'audio'
        assert pid not in kinds or kinds[pid] == kind, ('PID changed media type before EOF', pid, kinds.get(pid), kind)
        kinds[pid] = kind


build, ffmpeg, ffprobe = Path(sys.argv[1]).resolve(), sys.argv[2], sys.argv[3]
f = Fixture(build / 'MistController', ffmpeg)
f.env['UDP_API'] = 'udp://127.0.0.1:0'
success = False
try:
    source, output = f.root / 'bframes.mkv', f.root / 'recording.ts'
    # Keep audio playing through two track-selection refreshes after video EOF.
    subprocess.run([ffmpeg, '-v', 'error', '-y', '-f', 'lavfi', '-i', 'testsrc2=size=320x180:rate=25:duration=6',
                    '-f', 'lavfi', '-i', 'sine=frequency=440:sample_rate=48000:duration=15',
                    '-c:v', 'libx264', '-threads', '1', '-preset', 'medium', '-bf', '3', '-b_strategy', '0', '-g', '50',
                    '-c:a', 'aac', str(source)], env=f.tool_env, check=True, timeout=30)
    f.start(streams_override={f.stream: {'source': str(source)}}, extra_protocols=[{'connector': 'HTTPTS'}])
    with urllib.request.urlopen('http://127.0.0.1:%d/%s.ts' % (f.http, f.stream), timeout=30) as response:
        assert response.status == 200, 'TS request was refused'
        output.write_bytes(response.read())
    assert_pid_media_stable(output.read_bytes())
    times = video_timestamps(output.read_bytes())
    assert len(times) == 150, ('TS output lost video packets', len(times))
    backwards = [(i, before[1], after[1]) for i, (before, after) in enumerate(zip(times, times[1:]), 1) if after[1] <= before[1]]
    assert not backwards, ('TS video DTS must be strictly increasing', backwards[:5])
    early = [(i, pts, dts) for i, (pts, dts) in enumerate(times) if pts < dts]
    assert not early, ('TS video PTS must be at least DTS', len(early), early[:5])
    def packets(path, selector):
        return json.loads(subprocess.check_output([ffprobe, '-v', 'error', '-select_streams', selector,
                          '-show_packets', '-of', 'json', str(path)], env=f.tool_env, text=True, timeout=30))['packets']
    audio_only = f.root / 'audio.ts'
    with urllib.request.urlopen('http://127.0.0.1:%d/%s.ts?video=none&audio=all' % (f.http, f.stream), timeout=30) as response:
        audio_only.write_bytes(response.read())
    mixed_audio, separate_audio = packets(output, 'a:0'), packets(audio_only, 'a:0')
    common_audio = min(len(mixed_audio), len(separate_audio))
    assert common_audio > 250, ('TS request returned too little audio', common_audio)
    assert [p['pts'] for p in mixed_audio[:common_audio]] == [p['pts'] for p in separate_audio[:common_audio]], ('audio-only TS request changed its clock', len(mixed_audio), len(separate_audio), [p['pts'] for p in mixed_audio[:5]], [p['pts'] for p in separate_audio[:5]])
    playlist_url = 'http://127.0.0.1:%d/hls/%s/index.m3u8' % (f.http, f.stream)
    for _ in range(3):
        with urllib.request.urlopen(playlist_url, timeout=15) as response:
            uris = [line.strip() for line in response.read().decode().splitlines() if line.strip() and not line.startswith('#')]
        assert uris, 'HLS playlist contains no media'
        if not urllib.parse.urlparse(uris[0]).path.endswith('.m3u8'):
            break
        playlist_url = urllib.parse.urljoin(playlist_url, uris[0])
    hls_times = []
    for uri in uris:
        with urllib.request.urlopen(urllib.parse.urljoin(playlist_url, uri), timeout=15) as response:
            hls_times.extend(video_timestamps(response.read()))
    assert len(hls_times) > 100, ('HLS returned too few video packets', len(hls_times))
    assert all(after[1] > before[1] for before, after in zip(hls_times, hls_times[1:])), 'HLS DTS must increase across segments'
    assert all(pts >= dts for pts, dts in hls_times), 'HLS video PTS must be at least DTS'
    full_times = {dts: pts for pts, dts in times}
    assert all(full_times.get(dts) == pts for pts, dts in hls_times), 'HLS segments changed their TS presentation clock'
    shifts = []
    for selector in ('v:0', 'a:0'):
        original, recorded = packets(source, selector), packets(output, selector)
        if selector == 'v:0':
            assert len(recorded) == len(original) == 150, ('TS EOF lost a demuxed video frame', len(original), len(recorded))
        else:
            assert len(recorded) > 250, (selector, 'too little demuxable media', len(recorded))
        changes = [float(after['pts_time']) - float(before['pts_time']) for before, after in zip(original, recorded)]
        assert max(changes) - min(changes) < 0.003, (selector, 'presentation times changed unevenly', changes[:10])
        shifts.append(changes[0])
    assert abs(shifts[0] - shifts[1]) < 0.025, ('video/audio sync shifted', shifts)
    def frame_hashes(path):
        decoded = subprocess.check_output([ffmpeg, '-v', 'error', '-i', str(path), '-map', '0:v:0',
                                           '-fps_mode', 'passthrough', '-f', 'framemd5', '-'],
                                          env=f.tool_env, text=True, timeout=30)
        return [line.rsplit(',', 1)[-1].strip() for line in decoded.splitlines() if line and not line.startswith('#')]
    original_hashes, recorded_hashes = frame_hashes(source), frame_hashes(output)
    assert len(original_hashes) == len(recorded_hashes) == 150, ('TS EOF lost decoded frames', len(original_hashes), len(recorded_hashes))
    assert recorded_hashes == original_hashes, 'TS remux changed decoded frame pixels or presentation order'

    success = True
    print('PASS Matroska B-frame TS: 150 pixel-identical frames and packets, strictly increasing DTS, PTS >= DTS, matching HLS/audio-only clocks and preserved A/V alignment', flush=True)
finally:
    f.close(failed=not success)
