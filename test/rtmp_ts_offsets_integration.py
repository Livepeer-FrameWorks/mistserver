#!/usr/bin/env python3
"""Match actual RTMP source slices to TS packets without adding a mux delay."""
from pathlib import Path
import hashlib
import json
import re
import subprocess
import sys
import urllib.request
from controller_capacity_lifecycle_integration import Fixture, free_port, until


def stamp(data):
    return ((data[0] & 14) << 29) | (data[1] << 22) | ((data[2] & 254) << 14) | (data[3] << 7) | (data[4] >> 1)


def slice_hash(nals):
    slices = [nal.rstrip(b'\x00') for nal in nals if nal and 1 <= nal[0] & 31 <= 5]
    return hashlib.sha256(b''.join(slices)).hexdigest() if slices else None


def avcc_hash(data):
    nals = []
    cursor = 0
    while cursor + 4 <= len(data):
        length = int.from_bytes(data[cursor:cursor + 4], 'big')
        cursor += 4
        nals.append(data[cursor:cursor + length])
        cursor += length
    return slice_hash(nals)


def source_packets(ffprobe, path, env):
    rows = json.loads(subprocess.check_output([ffprobe, '-v', 'error', '-select_streams', 'v:0', '-show_packets',
        '-show_data', '-of', 'json', str(path)], env=env, timeout=30))['packets']
    result = {}
    for row in rows:
        data = bytes.fromhex(''.join(line.split(':', 1)[1].split('  ', 1)[0].strip()
                                    for line in row['data'].splitlines() if ':' in line))
        key = avcc_hash(data)
        assert key and key not in result, 'source slice identities are not unique'
        pts, dts = round(float(row['pts_time']) * 90000), round(float(row['dts_time']) * 90000)
        assert pts >= dts, ('RTMP source contains a negative offset', pts, dts)
        result[key] = (pts, dts)
    return result


def ts_packets(data):
    completed = []
    pending = {}
    for start in range(0, len(data), 188):
        packet = data[start:start + 188]
        assert len(packet) == 188 and packet[0] == 0x47, 'invalid TS packet'
        if not packet[3] & 16:
            continue
        pid = ((packet[1] & 31) << 8) | packet[2]
        offset = 4 + (packet[4] + 1 if packet[3] & 32 else 0)
        if packet[1] & 64:
            if pid in pending:
                completed.append(bytes(pending[pid]))
            pending[pid] = bytearray()
        if pid in pending:
            pending[pid].extend(packet[offset:])
    completed.extend(bytes(p) for p in pending.values())
    for pes in completed:
        if len(pes) < 14 or pes[:3] != b'\x00\x00\x01' or not 0xe0 <= pes[3] <= 0xef:
            continue
        length = int.from_bytes(pes[4:6], 'big')
        if length:
            pes = pes[:length + 6]
        pts = stamp(pes[9:14])
        dts = stamp(pes[14:19]) if pes[7] >> 6 == 3 else pts
        key = slice_hash(re.split(b'\x00\x00\x01', pes[9 + pes[8]:]))
        if key:
            yield key, pts, dts


def main():
    build, ffmpeg, ffprobe = Path(sys.argv[1]).resolve(), sys.argv[2], sys.argv[3]
    fps, mode = int(sys.argv[4]), sys.argv[5]
    f = Fixture(build / 'MistController', ffmpeg)
    f.env['UDP_API'] = 'udp://127.0.0.1:0'
    success = False
    try:
        source = f.root / 'source.flv'
        video = ['-f', 'lavfi', '-i', 'testsrc2=size=320x180:rate=%d:duration=8' % fps]
        encoder = ['-c:v', 'libx264', '-threads', '1', '-preset', 'medium', '-bf', '3', '-g', str(fps * 2)]
        if mode == 'no-restriction':
            raw, stripped = f.root / 'raw.h264', f.root / 'stripped.h264'
            subprocess.run([ffmpeg, '-v', 'error', '-y', *video, '-an', '-c:v', 'libx264', '-threads', '1',
                '-bf', '0', '-g', str(fps * 2), '-f', 'h264', str(raw)], env=f.tool_env, check=True, timeout=30)
            subprocess.run([sys.executable, str(Path(__file__).with_name('h264_strip_restriction.py')), str(raw), str(stripped)],
                           env=f.tool_env, check=True, timeout=10)
            trace = subprocess.run([ffmpeg, '-v', 'trace', '-i', str(stripped), '-c', 'copy', '-bsf:v', 'trace_headers',
                                    '-f', 'null', '-'], env=f.tool_env, capture_output=True, text=True, timeout=15)
            assert re.search(r'bitstream_restriction_flag.*= 0', trace.stderr), 'fixture SPS still has a reorder restriction'
            video, encoder = ['-r', str(fps), '-i', str(stripped)], ['-c:v', 'copy']
        subprocess.run([ffmpeg, '-v', 'error', '-y', *video, '-f', 'lavfi', '-i', 'sine=sample_rate=48000:duration=8',
                        *encoder, '-c:a', 'aac', '-t', '8', str(source)], env=f.tool_env, check=True, timeout=30)
        expected = source_packets(ffprobe, source, f.tool_env)
        port = free_port()
        f.start(streams_override={f.stream: {'source': 'push://', 'resume': 1}},
                extra_protocols=[{'connector': 'HTTPTS'}, {'connector': 'RTMP', 'interface': '127.0.0.1', 'port': port}])
        publisher = subprocess.Popen([ffmpeg, '-v', 'error', '-re', '-i', str(source), '-map', '0', '-c', 'copy', '-f', 'flv',
            'rtmp://127.0.0.1:%d/live/%s' % (port, f.stream)], env=f.tool_env, stdout=f.log, stderr=subprocess.STDOUT)
        f.children.append(publisher)
        def ready():
            try:
                with urllib.request.urlopen('http://127.0.0.1:%d/json_%s.js' % (f.http, f.stream), timeout=2) as response:
                    meta = json.load(response).get('meta', {})
                return meta.get('live') and {'video', 'audio'}.issubset({t.get('type') for t in meta.get('tracks', {}).values()})
            except (OSError, ValueError):
                return False
        until(ready, 10, 'RTMP publisher did not expose its source tracks')
        with urllib.request.urlopen('http://127.0.0.1:%d/%s.ts?start=0&duration=4' % (f.http, f.stream), timeout=15) as response:
            actual = list(ts_packets(response.read()))
        assert len(actual) >= fps * 3, ('too few matched TS frames', len(actual))
        added = []
        for key, pts, dts in actual:
            assert key in expected, 'TS writer changed source slice bytes'
            source_pts, source_dts = expected[key]
            added.append((pts - dts) - (source_pts - source_dts))
        assert all(value == 0 for value in added), ('TS added presentation delay to nonnegative RTMP offsets', fps, mode, sorted(set(added)))
        success = True
        print('PASS RTMP %d fps %s: %d identical source frames retain PTS == DTS + source offset, added delay=0' % (fps, mode, len(actual)), flush=True)
    finally:
        f.close(failed=not success)


if __name__ == '__main__':
    main()
