#!/usr/bin/env python3
"""Prove disabling ProcAV leaves all remaining test dependencies well-defined."""
from pathlib import Path
import subprocess
import sys
import tempfile

with tempfile.TemporaryDirectory(prefix="mist-noav-configure-") as directory:
    result = subprocess.run([
        "meson", "setup", directory, str(Path(sys.argv[1]).resolve()),
        "--wrap-mode=nodownload", "-DWITH_AV=false", "-DONNX=disabled",
        "-DNOSSL=true", "-DNORIST=true", "-DNOSRT=true", "-DNOUSRSCTP=true",
    ], text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    print(result.stdout)
    assert result.returncode == 0, "non-AV Meson setup failed before tests could register"
print("PASS non-AV Meson setup registers remaining media fixtures")
