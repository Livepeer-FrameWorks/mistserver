#!/usr/bin/env python3
"""Require real hardware decode, scaling and encode through Mist's local ladder."""
import os
from pathlib import Path
import sys
from process_av_rotation_integration import exercise


def main():
    backend = os.environ.get("MIST_TEST_HW_DEVICE")
    if backend not in ("qsv", "cuda", "vaapi"):
        print("Set MIST_TEST_HW_DEVICE on a machine with the real device", file=sys.stderr)
        return 77
    os.environ["MIST_ROTATION_ACCEL"] = "hw"
    os.environ["MIST_EXPECT_HW_BACKEND"] = backend
    os.environ["MIST_EXPECT_THUMB_ENCODER"] = "qsv" if backend == "qsv" else "software" if backend == "cuda" else "vaapi"
    os.environ["MIST_THUMBS_ACCEL"] = "hw" if backend != "cuda" else "auto"
    build, ffmpeg, ffprobe = Path(sys.argv[1]).resolve(), sys.argv[2], sys.argv[3]
    codecs = ("H264", "HEVC", "AV1", "VP9", "MPEG2") if backend == "qsv" else ("H264", "HEVC", "AV1")
    for codec in codecs:
        os.environ["MIST_ROTATION_CONTAINER"] = "mkv" if codec == "MPEG2" else "av"
        exercise(build, ffmpeg, ffprobe, codec, 0)
    os.environ["MIST_ROTATION_CONTAINER"] = "mp4"
    os.environ["MIST_ROTATION_OUTPUT"] = "AV1"
    exercise(build, ffmpeg, ffprobe, "H264", 0)
    os.environ["MIST_ROTATION_OUTPUT"] = "H264"
    os.environ["MIST_ROTATION_ACCEL"] = "auto"
    for mirror in (False, True):
        for angle in (0, 90, 180, 270):
            exercise(build, ffmpeg, ffprobe, "H264", angle, mirror)
    return 0


if __name__ == "__main__":
    sys.exit(main())
