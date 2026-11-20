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


for program, expected in [(sys.argv[1], {"H264", "AV1", "JPEG", "YUYV", "UYVY", "NV12", "I420", "PCM", "opus", "AAC"}), (sys.argv[2], {"H264", "AV1", "JPEG"})]:
    result = subprocess.run([program, "--json"], capture_output=True, text=True, timeout=10)
    assert result.returncode in (0, 255), result.stderr
    capability = json.loads(result.stdout)
    supported = set(strings(capability["codecs"]))
    assert expected == supported, f"{program} codec contract differs: missing={sorted(expected - supported)}, unexpected={sorted(supported - expected)}"
