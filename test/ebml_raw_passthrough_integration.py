#!/usr/bin/env python3
"""All supported planar raw tracks survive a real Matroska VOD response."""
from pathlib import Path
import struct
import sys
import urllib.request

from controller_capacity_lifecycle_integration import Fixture


def element(identifier, payload):
    size = len(payload)
    width = next(n for n in range(1, 9) if size < (1 << (7 * n)) - 1)
    return identifier.to_bytes((identifier.bit_length() + 7) // 8, 'big') + ((1 << (7 * width)) | size).to_bytes(width, 'big') + payload


def uint(identifier, value):
    return element(identifier, value.to_bytes(max(1, (value.bit_length() + 7) // 8), 'big'))


def elements(data):
    offset = 0
    while offset < len(data):
        start = offset
        width = 1
        while not data[offset] & (1 << (8 - width)):
            width += 1
        identifier = int.from_bytes(data[offset:offset + width], 'big')
        offset += width
        width = 1
        while not data[offset] & (1 << (8 - width)):
            width += 1
        size = int.from_bytes(data[offset:offset + width], 'big') & ((1 << (7 * width)) - 1)
        offset += width
        end = len(data) if size == (1 << (7 * width)) - 1 else offset + size
        assert end <= len(data) and end > start, 'truncated EBML element'
        yield identifier, data[offset:end]
        offset = end


def unpack(data):
    tracks = {}
    blocks = []
    for identifier, payload in elements(data):
        if identifier == 0x18538067:
            return unpack(payload)
        if identifier == 0x1654AE6B:
            for entry_id, entry in elements(payload):
                if entry_id != 0xAE:
                    continue
                fields = dict(elements(entry))
                video = dict(elements(fields[0xE0]))
                assert fields[0x86] == b'V_UNCOMPRESSED', fields
                tracks[int.from_bytes(fields[0xD7], 'big')] = video[0x2EB524].decode()
        if identifier == 0x1F43B675:
            base = 0
            for child, value in elements(payload):
                if child == 0xE7:
                    base = int.from_bytes(value, 'big')
                if child == 0xA3:
                    assert value[0] & 0x80 and not value[3] & 0x06, 'unexpected track number or lacing'
                    blocks.append((value[0] & 0x7f, base + int.from_bytes(value[1:3], 'big', signed=True), value[4:]))
    return tracks, blocks


build, ffmpeg = Path(sys.argv[1]).resolve(), sys.argv[2]
f = Fixture(build / 'MistController', ffmpeg)
f.env['UDP_API'] = 'udp://127.0.0.1:0'
success = False
try:
    codecs = ('I420', 'I422', 'I444', 'NV12')
    sizes = (384, 512, 768, 384)
    entries = b''
    expected = {codec: [] for codec in codecs}
    blocks = b''
    for index, codec in enumerate(codecs, 1):
        video = uint(0xB0, 16) + uint(0xBA, 16) + element(0x2EB524, codec.encode())
        entries += element(0xAE, uint(0xD7, index) + uint(0x73C5, index) + uint(0x83, 1) + uint(0x23E383, 100000000) + element(0x86, b'V_UNCOMPRESSED') + element(0xE0, video))
    for frame in range(10):
        for index, (codec, size) in enumerate(zip(codecs, sizes), 1):
            pixels = bytes([index * 30 + frame]) * size
            expected[codec].append((frame * 100, pixels))
            blocks += element(0xA3, bytes([0x80 | index]) + struct.pack('>h', frame * 100) + b'\x80' + pixels)
    header = uint(0x4286, 1) + uint(0x42F7, 1) + uint(0x42F2, 4) + uint(0x42F3, 8) + element(0x4282, b'matroska') + uint(0x4287, 4) + uint(0x4285, 1)
    info = uint(0x2AD7B1, 1000000) + element(0x4489, struct.pack('>d', 1000))
    source = f.root / 'planar.mkv'
    source.write_bytes(element(0x1A45DFA3, header) + element(0x18538067, element(0x1549A966, info) + element(0x1654AE6B, entries) + element(0x1F43B675, uint(0xE7, 0) + blocks)))
    f.start(streams_override={f.stream: {'source': str(source)}})
    url = 'http://127.0.0.1:%d/%s.mkv?video=all&audio=none&subtitle=none&meta=none' % (f.http, f.stream)
    with urllib.request.urlopen(url, timeout=20) as response:
        recorded = response.read()
    (f.root / 'recorded.mkv').write_bytes(recorded)
    tracks, packets = unpack(recorded)
    assert set(tracks.values()) == set(codecs), ('raw VOD tracks disappeared', tracks)
    actual = {codec: [] for codec in codecs}
    for track, timestamp, pixels in packets:
        actual[tracks[track]].append((timestamp, pixels))
    assert actual == expected, 'raw VOD changed frame count, timing or pixels'
    success = True
    print('PASS actual Matroska VOD retains I420/I422/I444/NV12, all 40 frames, timestamps and pixels')
finally:
    f.close(failed=not success)
