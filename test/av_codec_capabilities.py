#!/usr/bin/env python3
import json
import subprocess
import sys


def strings(value):
    if isinstance(value, str):
        yield value
    elif isinstance(value, list):
        for entry in value:
            yield from strings(entry)


for program, expected in [(sys.argv[1], {"H264", "HEVC", "AV1", "VP8", "VP9", "MPEG2", "JPEG", "theora", "YUYV", "UYVY", "NV12", "I420", "PCM", "opus", "AAC", "AC3", "EAC-3", "MP3", "MP2", "FLAC", "vorbis"}), (sys.argv[2], {"H264", "HEVC", "AV1", "VP8", "VP9", "MPEG2", "JPEG", "theora"})]:
    result = subprocess.run([program, "--json"], capture_output=True, text=True, timeout=10)
    assert result.returncode in (0, 255), result.stderr
    capability = json.loads(result.stdout)
    supported = set(strings(capability["codecs"]))
    assert expected == supported, f"{program} codec contract differs: missing={sorted(expected - supported)}, unexpected={sorted(supported - expected)}"
