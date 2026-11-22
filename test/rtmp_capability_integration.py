#!/usr/bin/env python3
"""Check the actual capability source used by wildcard RTMP negotiation."""
import json
import subprocess
import sys

result = subprocess.run([sys.argv[1], "-j"], stdout=subprocess.PIPE, text=True, timeout=10)
assert result.returncode in (0, 255), result
capabilities = json.loads(result.stdout)
for configuration in capabilities["codecs"]:
    for track in configuration:
        assert "EAC-3" not in track and "EAC3" not in track, ("RTMP wildcard advertises unwritable E-AC3", track)
assert any("AC3" in track for configuration in capabilities["codecs"] for track in configuration), capabilities
print("PASS actual RTMP wildcard capabilities omit E-AC3 and retain AC3")
