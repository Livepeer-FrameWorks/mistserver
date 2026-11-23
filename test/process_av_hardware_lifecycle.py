#!/usr/bin/env python3
"""Decode every frame across repeated real-device pool lifetimes in one process."""
import os
from pathlib import Path
import sys
import tempfile
from process_av_rotation_integration import run


def main():
    backend, build, ffmpeg = sys.argv[1], Path(sys.argv[2]), sys.argv[3]
    with tempfile.TemporaryDirectory(prefix="mist-hw-lifecycle-") as directory:
        for codec, encoder in (("H264", ["libx264", "-preset", "ultrafast"]),
                               ("HEVC", ["libx265", "-preset", "ultrafast", "-x265-params", "log-level=error:pools=1"]),
                               ("AV1", ["libaom-av1", "-cpu-used", "8", "-threads", "2", "-usage", "realtime"])):
            source = Path(directory) / (codec + ".mp4")
            run([ffmpeg, "-v", "error", "-y", "-f", "lavfi", "-i", "testsrc2=size=320x180:rate=10:duration=4",
                 "-c:v", *encoder, "-g", "10", "-bf", "0", "-pix_fmt", "yuv420p", source])
            run([build / "test" / "avhardwarelifecycletest", backend, source], timeout=150)
    return 0


if __name__ == "__main__":
    sys.exit(main())
