#!/usr/bin/env python3
"""Check rotation metadata on actual MP4, Matroska and CMAF passthrough outputs."""
import json
import os
from pathlib import Path
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import urllib.request

from process_av_rotation_integration import run, wait_for
from test_ports import free_port


def main():
    if os.environ.get("MIST_RUN_MEDIA_TESTS") != "1":
        print("Set MIST_RUN_MEDIA_TESTS=1 for isolated media integration", file=sys.stderr)
        return 77
    build, ffmpeg, ffprobe = Path(sys.argv[1]).resolve(), sys.argv[2], sys.argv[3]
    work = Path(tempfile.mkdtemp(prefix="mist-passthrough-rotation-"))
    env = dict(os.environ, TMP=str(work / "ipc"), MIST_CONTROL="1", UDP_API="udp://127.0.0.1:0",
               PATH=str(build) + os.pathsep + os.environ["PATH"])
    Path(env["TMP"]).mkdir()
    stream = "passthrough" + str(os.getpid())
    api_port, http_port = free_port(), free_port()
    angle = int(os.environ.get("MIST_PASSTHROUGH_ANGLE", "90"))
    mirror = os.environ.get("MIST_PASSTHROUGH_MIRROR") == "1"
    controller = None
    success = False
    try:
        run([ffmpeg, "-v", "error", "-y", "-f", "lavfi", "-i", "color=blue:size=320x180:rate=10:duration=4",
             "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p", "-g", "10", "-bf", "2", work / "base.mp4"])
        run([ffmpeg, "-v", "error", "-y", "-display_rotation", str(-angle), *(["-display_hflip"] if mirror else []), "-i", work / "base.mp4", "-c", "copy", work / "source.mp4"])
        config = {
            "account": {"test": {"password": "098f6bcd4621d373cade4e832627b4f6"}},
            "config": {"device_discovery": False, "debug": 4,
                       "controller": {"interface": "127.0.0.1", "port": api_port, "username": None},
                       "protocols": [{"connector": "HTTP", "interface": "127.0.0.1", "port": http_port}, {"connector": "CMAF"}],
                       "sessionInputMode": 15, "sessionOutputMode": 15, "sessionStreamInfoMode": 1,
                       "sessionUnspecifiedMode": 0, "sessionViewerMode": 14, "tknMode": 15, "triggers": {}},
            "streams": {stream: {"name": stream, "source": str(work / "source.mp4")}}}
        (work / "config.json").write_text(json.dumps(config))
        log = work / "controller.log"
        controller = subprocess.Popen([str(build / "MistController"), "-c", str(work / "config.json"), "-C", "r", "-L", str(log)], env=env)
        wait_for(lambda: log.exists() and "Controller started" in log.read_text(errors="replace"))
        for binary, suffix in (("MistOutMP4", "mp4"), ("MistOutEBML", "mkv")):
            target = str(work / ("out." + suffix)) + "?video=H264&audio=none&subtitle=none&meta=none"
            with (work / (suffix + ".log")).open("wb") as output:
                run([build / binary, "-s", stream, target], env=env, stdout=output, stderr=subprocess.STDOUT, timeout=35)

        def http_ready():
            try:
                with socket.create_connection(("127.0.0.1", http_port), timeout=0.5):
                    return True
            except OSError:
                return False

        wait_for(http_ready)
        with urllib.request.urlopen(f"http://127.0.0.1:{http_port}/cmaf/{stream}/0/init.m4s", timeout=20) as response:
            (work / "init.m4s").write_bytes(response.read())
        for name in ("out.mp4", "out.mkv", "init.m4s"):
            info = json.loads(run([ffprobe, "-v", "error", "-show_streams", "-of", "json", work / name], stdout=subprocess.PIPE).stdout)
            video = next(s for s in info["streams"] if s["codec_type"] == "video")
            rotations = [s["rotation"] for s in video.get("side_data_list", []) if "rotation" in s]
            if not mirror or name == "init.m4s":
                assert (rotations or angle == 0) and all(round(v) % 360 == -angle % 360 for v in rotations), (name, video)
            matrix = next((s["displaymatrix"] for s in video.get("side_data_list", []) if "displaymatrix" in s), "0: 65536 0 0\n1: 0 65536 0\n2: 0 0 1073741824")
            rows = [[int(v) for v in row.split(":", 1)[1].split()] for row in matrix.strip().splitlines()]
            determinant = rows[0][0] * rows[1][1] - rows[0][1] * rows[1][0]
            assert (determinant < 0) == (mirror and name != "init.m4s"), (name, matrix)
            if name != "init.m4s":
                kind = "mp4" if name.endswith("mp4") else "ebml"
                tracks = json.loads(run([build / "test" / ("uploadmetaprobe_" + kind), work / name], stdout=subprocess.PIPE).stdout)
                track = next(t for t in tracks["tracks"].values() if t["type"] == "video")
                assert track.get("rotation", 0) == angle and bool(track.get("mirror")) == mirror, (name, track)
            assert (video["width"], video["height"]) == (320, 180), (name, video)
            reflected = mirror and name != "init.m4s"
            print(f"PASS actual passthrough {name}: coded 320x180, clockwise {angle} mirror={reflected} display matrix", flush=True)
        success = True
    finally:
        if controller:
            subprocess.run([str(build / "MistUtilNuke"), stream], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=10)
            if controller.poll() is None:
                controller.send_signal(signal.SIGINT)
                try:
                    controller.wait(timeout=15)
                except subprocess.TimeoutExpired:
                    controller.kill()
                    controller.wait()
        if success and os.environ.get("MIST_KEEP_TEST_ARTIFACTS") != "1":
            shutil.rmtree(work)
        else:
            print(f"Integration artifacts: {work}", file=sys.stderr, flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
