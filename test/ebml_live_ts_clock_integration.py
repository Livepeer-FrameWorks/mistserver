#!/usr/bin/env python3
"""Keep one presentation clock as live Matroska reorder depth and windows change."""
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import json
import subprocess
import sys
import time
import urllib.parse
import urllib.request
from controller_capacity_lifecycle_integration import Fixture, until


def stamp(data):
    return ((data[0] & 14) << 29) | (data[1] << 22) | ((data[2] & 254) << 14) | (data[3] << 7) | (data[4] >> 1)


def timestamps(data):
    result = {'video': [], 'audio': []}
    assert len(data) % 188 == 0, 'incomplete TS packet'
    for start in range(0, len(data), 188):
        packet = data[start:start + 188]
        assert packet[0] == 0x47, 'TS sync lost'
        if not packet[1] & 64 or not packet[3] & 16:
            continue
        offset = 4 + (1 + packet[4] if packet[3] & 32 else 0)
        pes = packet[offset:]
        if len(pes) < 14 or pes[:3] != b'\x00\x00\x01' or not 0xc0 <= pes[3] <= 0xef:
            continue
        pts = stamp(pes[9:14])
        dts = stamp(pes[14:19]) if pes[7] >> 6 == 3 else pts
        result['video' if pes[3] >= 0xe0 else 'audio'].append((pts, dts))
    return result


def main():
    build, ffmpeg, ffprobe = Path(sys.argv[1]).resolve(), sys.argv[2], sys.argv[3]
    codec = sys.argv[4] if len(sys.argv) > 4 else 'H264'
    encoder = ['libx264', '-b_strategy', '2'] if codec == 'H264' else ['libx265', '-x265-params', 'b-adapt=2:pools=1:frame-threads=1:log-level=error']
    f = Fixture(build / 'MistController', ffmpeg)
    f.env['UDP_API'] = 'udp://127.0.0.1:0'
    success = False
    try:
        source = f.root / 'adaptive.mkv'
        subprocess.run([ffmpeg, '-v', 'error', '-y', '-filter_threads', '1', '-filter_complex_threads', '1',
                        '-f', 'lavfi', '-i', 'testsrc2=size=320x180:rate=25:duration=20',
                        '-f', 'lavfi', '-i', 'sine=sample_rate=48000:duration=20', '-c:v', *encoder, '-threads', '1',
                        '-preset', 'veryfast' if codec == 'HEVC' else 'medium', '-bf', '3', '-g', '20', '-keyint_min', '1', '-sc_threshold', '0',
                        '-force_key_frames', 'expr:if(lt(t,4.2),1,if(lt(t,8),eq(mod(n,20),0),1))',
                        '-c:a', 'aac', str(source)], env=f.tool_env, check=True, timeout=30)
        original = json.loads(subprocess.check_output([ffprobe, '-v', 'error', '-select_streams', 'v:0',
                              '-show_entries', 'packet=pts_time', '-of', 'json', str(source)], env=f.tool_env))['packets']
        pts = [round(float(p['pts_time']) * 1000) for p in original]
        offsets = [a - b for a, b in zip(pts, sorted(pts))]
        if codec == 'H264':
            assert min(offsets[:100]) == 0 and min(offsets[125:200]) == -80 and min(offsets[325:425]) == 0, ('fixture did not vary B-frame depth', offsets)
        else:
            assert min(offsets) == -80 and -40 in offsets, ('HEVC fixture did not reorder frames', offsets)
        # Keep the source window after publisher EOF so the reader can verify its
        # complete tail without non-resumable stream shutdown removing the tracks.
        f.start(streams_override={f.stream: {'source': 'push://', 'DVR': 4000, 'segmentsize': 200, 'resume': 1}}, extra_protocols=[{'connector': 'HTTPTS'}])
        publisher = subprocess.Popen([ffmpeg, '-v', 'error', '-re', '-i', str(source), '-map', '0', '-c', 'copy',
                                      '-f', 'matroska', '-live', '1', '-cluster_time_limit', '250', '-method', 'PUT',
                                      'http://127.0.0.1:%d/%s.mkv' % (f.http, f.stream)], env=f.tool_env,
                                     stdout=f.log, stderr=subprocess.STDOUT)
        f.children.append(publisher)
        def source_ready():
            try:
                with urllib.request.urlopen('http://127.0.0.1:%d/json_%s.js' % (f.http, f.stream), timeout=2) as response:
                    meta = json.load(response).get('meta', {})
                return meta if meta.get('live') and len(meta.get('tracks', {})) >= 2 else False
            except (OSError, ValueError):
                return False
        initial_meta = until(source_ready, 10, 'MKV publisher did not expose live video/audio metadata')
        first_video = min(t['firstms'] for t in initial_meta['tracks'].values() if t.get('type') == 'video')
        def connection():
            chunks = []
            with urllib.request.urlopen('http://127.0.0.1:%d/%s.ts?start=0' % (f.http, f.stream), timeout=3) as response:
                while True:
                    try:
                        chunk = response.read(188)
                    except TimeoutError:
                        assert publisher.poll() == 0, 'live read stalled before the publisher completed'
                        break
                    if not chunk:
                        break
                    chunks.append(chunk)
            return timestamps(b''.join(chunks))
        seen = {}
        segments = []
        playlist = 'http://127.0.0.1:%d/hls/%s/index.m3u8' % (f.http, f.stream)
        if codec == 'HEVC':
            # Browser defaults exclude HEVC; address this fixture's video/audio variant explicitly.
            playlist = 'http://127.0.0.1:%d/hls/%s/0_1/index.m3u8' % (f.http, f.stream)
        started = time.monotonic()
        last_meta = None
        with ThreadPoolExecutor(max_workers=1) as pool:
            reader = pool.submit(connection)
            while time.monotonic() - started < 20:
                url = playlist
                try:
                    for _ in range(3):
                        with urllib.request.urlopen(url, timeout=5) as response:
                            uris = [line.strip() for line in response.read().decode().splitlines() if line.strip() and not line.startswith('#')]
                        if not uris or not urllib.parse.urlparse(uris[0]).path.endswith('.m3u8'):
                            break
                        url = urllib.parse.urljoin(url, uris[0])
                    for uri in uris:
                        segment = urllib.parse.urljoin(url, uri)
                        if segment in seen:
                            continue
                        with urllib.request.urlopen(segment, timeout=5) as response:
                            row = timestamps(response.read())
                        seen[segment] = row
                        segments.append(row)
                except OSError:
                    pass
                if last_meta is None and time.monotonic() - started > 17:
                    last_meta = source_ready() or None
                time.sleep(0.35)
            actual = reader.result(timeout=15)
        assert last_meta is not None, 'did not observe the advanced live window'
        last_first = min(t['firstms'] for t in last_meta['tracks'].values() if t.get('type') == 'video')
        assert last_first - first_video > 8000, ('live window retained the earlier deep-B-frame interval', first_video, last_first)
        video = actual['video']
        ingested = json.loads(subprocess.check_output([sys.argv[5], f.stream], env=f.env, timeout=5))
        ingest_video = [track for track in ingested.values() if track['type'] == 'video']
        assert len(ingest_video) == 1 and ingest_video[0]['parts_end'] == len(original), ('publisher EOF lost buffered source frames', ingested, len(original))
        # An ordinary resumable live reader holds its final packet until the next
        # arrives. Check that prefix separately from the complete source counter.
        assert len(original) - 1 <= len(video) <= len(original), ('live TS lost more than its held final packet', len(video), len(original))
        print('publisher EOF: complete source metadata=%s; continuous TS frames=%d/%d' % (json.dumps(ingest_video), len(video), len(original)), flush=True)
        assert all(b[1] > a[1] for a, b in zip(video, video[1:])), 'live MKV DTS moved backwards'
        assert all(pts >= dts for pts, dts in video), 'live MKV PTS precedes DTS'
        sorted_pts = sorted(pts for pts, _ in video)
        gaps = [b - a for a, b in zip(sorted_pts, sorted_pts[1:])]
        assert all(abs(gap - 3600) <= 90 for gap in gaps), ('live TS presentation clock jumped as reorder depth changed', sorted(set(gaps)))
        audio = [pts for pts, _ in actual['audio']]
        assert len(audio) > 500, ('too little live audio', len(audio))
        gaps = [b - a for a, b in zip(audio, audio[1:])]
        assert all(abs(gap - 1920) <= 90 for gap in gaps), ('live audio acquired a presentation gap', sorted(set(gaps)))
        reference = {dts: pts for pts, dts in video}
        compared = 0
        hls_clock = {}
        assert len(segments) >= 4, ('too few HLS segment requests', len(segments))
        for row in segments:
            for pts, dts in row['video']:
                assert dts not in hls_clock or hls_clock[dts] == pts, ('overlapping HLS requests disagreed on PTS', dts, pts, hls_clock.get(dts))
                hls_clock[dts] = pts
                if dts in reference:
                    compared += 1
                    assert pts == reference[dts], ('live HLS segment used a different presentation clock', dts, pts, reference[dts])
        assert compared > 200, ('HLS did not overlap the continuous connection', compared)
        hls_pts = sorted({pts for dts, pts in hls_clock.items() if dts in reference})
        assert all(abs(b - a - 3600) <= 90 for a, b in zip(hls_pts, hls_pts[1:])), 'HLS segment boundaries introduced a presentation gap'
        success = True
        print('PASS live %s MKV: continuous video/audio PTS and matching clocks across %d HLS segments (%d packets)' % (codec, len(segments), compared), flush=True)
    finally:
        f.close(failed=not success)


if __name__ == '__main__':
    main()
