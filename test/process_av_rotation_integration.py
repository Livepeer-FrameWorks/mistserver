#!/usr/bin/env python3
"""Run actual portrait uploads through an isolated Mist buffer and local ladder."""
import json
import os
import re
from pathlib import Path
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time


def run(command, **kwargs):
    return subprocess.run([str(v) for v in command], check=True, timeout=kwargs.pop("timeout", 60), **kwargs)


def wait_for(predicate, deadline=20):
    until = time.monotonic() + deadline
    while time.monotonic() < until:
        if predicate():
            return
        time.sleep(0.05)
    raise AssertionError("timed out waiting for fixture readiness")


def verify_corner(ffmpeg, path, stream, width, height, angle, mirror=False):
    pixels = run([ffmpeg, "-v", "error", "-noautorotate", "-i", path, "-map", f"0:v:{stream}",
                  "-frames:v", "1", "-f", "rawvideo", "-pix_fmt", "rgb24", "-"], stdout=subprocess.PIPE).stdout
    assert len(pixels) == width * height * 3
    corners = [(width // 10, height // 10), (width * 9 // 10, height // 10),
               (width * 9 // 10, height * 9 // 10), (width // 10, height * 9 // 10)]
    expected = angle // 90
    if mirror:
        expected = [1, 0, 3, 2][expected]
    for index, (x, y) in enumerate(corners):
        red, green, blue = pixels[(y * width + x) * 3:(y * width + x) * 3 + 3]
        assert (red > 160 and red > blue + 80 and red > green + 80) == (index == expected), (path, index, (red, green, blue), angle)


def exercise(build, ffmpeg, ffprobe, codec, angle, mirror=False):
    work = Path(tempfile.mkdtemp(prefix="mist-av-rotation-"))
    env = dict(os.environ, TMP=str(work / "ipc"), MIST_CONTROL="1", PATH=str(build) + os.pathsep + os.environ["PATH"], UDP_API="udp://127.0.0.1:0")
    Path(env["TMP"]).mkdir()
    stream = "rotation" + str(os.getpid())
    container = os.environ.get("MIST_ROTATION_CONTAINER", "mp4")
    output_codec = os.environ.get("MIST_ROTATION_OUTPUT", "H264")
    processes = []
    controller = None
    success = False
    try:
        base = work / "base.mp4"
        assert codec in ("H264", "HEVC", "MPEG2", "MPEG4"), codec
        encoder = ["-c:v", "libx265", "-preset", "ultrafast", "-x265-params", "log-level=error:pools=1"] if codec == "HEVC" else ["-c:v", "mpeg2video"] if codec == "MPEG2" else ["-c:v", "mpeg4"] if codec == "MPEG4" else ["-c:v", "libx264", "-preset", "ultrafast"]
        bframes = os.environ.get("MIST_ROTATION_BFRAMES", "2" if codec in ("HEVC", "H264") and container not in ("av", "mkv") else "0")
        run([ffmpeg, "-v", "error", "-y", "-f", "lavfi", "-i", "color=blue:size=320x180:rate=10:duration=4",
             "-vf", "drawbox=x=0:y=0:w=80:h=80:color=red:t=fill", *encoder, "-pix_fmt", "yuv420p", "-g", "10", "-bf", bframes, base])
        source = work / "source.mp4"
        if angle or mirror:
            run([ffmpeg, "-v", "error", "-y", "-display_rotation", str(-angle), *(["-display_hflip"] if mirror else []), "-i", base, "-c", "copy", source])
        else:
            shutil.copyfile(base, source)
        if container == "mkv":
            converted = work / "source.mkv"
            run([ffmpeg, "-v", "error", "-y", "-i", source, "-c", "copy", converted])
            source = converted
        probe_kind = "ebml" if container == "mkv" else "av" if container == "av" else "mp4"
        source_metadata = json.loads(run([build / "test" / ("uploadmetaprobe_" + probe_kind), source], stdout=subprocess.PIPE).stdout)
        original = next(v for v in source_metadata["tracks"].values() if v["type"] == "video")
        assert original.get("rotation", 0) == angle, original
        assert bool(original.get("mirror", False)) == mirror, original
        for short in (90, 180):
            processes.append({"process": "AV", "x-LSP-kind": "video", "codec": output_codec, "resolution": f"0x{short}",
                              "accel": os.environ.get("MIST_ROTATION_ACCEL", "sw"), "preset": "ultrafast", "tune": "zerolatency", "bitrate": 300000, "gopsize": 10,
                              "track_select": f"video={codec}&audio=none&meta=none&subtitle=none", "source_mask": 4,
                              "target_mask": 3, "restart_type": "fixed"})
        if os.environ.get("MIST_ROTATION_PROCESSOR") == "FFMPEG":
            processes = [{"process": "FFMPEG", "x-LSP-kind": "video", "codec": "H264", "source_track": codec,
                          "preset": "ultrafast", "flags": "-tune zerolatency -g 10 -bf 0", "bitrate": 300000,
                          "source_mask": 4, "target_mask": 3, "restart_type": "fixed"}]
        processes.append({"process": "Thumbs", "track_select": f"video={codec}", "thumb_width": 80, "thumb_height": 80,
                          "grid_cols": 2, "grid_rows": 2, "interval": 1000, "target_mask": 3, "restart_type": "fixed"})
        with socket.socket() as port:
            port.bind(("127.0.0.1", 0))
            api_port = port.getsockname()[1]
        config = {"account": {"test": {"password": "098f6bcd4621d373cade4e832627b4f6"}},
                  "config": {"debug": 4, "device_discovery": False, "protocols": [],
                             "controller": {"interface": "127.0.0.1", "port": api_port, "username": None},
                             "sessionInputMode": 15, "sessionOutputMode": 15, "sessionStreamInfoMode": 1,
                             "sessionUnspecifiedMode": 0, "sessionViewerMode": 14, "tknMode": 15, "triggers": {}},
                  "streams": {stream: {"name": stream, "source": str(source), "process_controlled_realtime": True,
                                       "realtime_speed": 1, "processes": processes}}}
        (work / "config.json").write_text(json.dumps(config))
        log = work / "controller.log"
        controller = subprocess.Popen([str(build / "MistController"), "-c", str(work / "config.json"), "-C", "r", "-L", str(log)], env=env)
        wait_for(lambda: log.exists() and "Controller started" in log.read_text(errors="replace"))
        with (work / "input.log").open("wb") as input_log:
            source_process = subprocess.Popen([str(build / ("MistInEBML" if container == "mkv" else "MistInAV" if container == "av" else "MistInMP4")), "-r", "-s", stream, str(source)], env=env, stdout=input_log, stderr=subprocess.STDOUT)
            processes[:] = [source_process]
            wait_for(lambda: "Input started" in (work / "input.log").read_text(errors="replace"))
            with (work / "output.log").open("wb") as output_log:
                run([build / "MistOutEBML", "-s", stream, str(work / "upright.mkv") + f"?video={output_codec}&audio=none&subtitle=none&meta=none&duration=4"],
                    env=env, stdout=output_log, stderr=subprocess.STDOUT, timeout=50)
        probe = json.loads(run([ffprobe, "-v", "error", "-show_streams", "-show_packets", "-of", "json", work / "upright.mkv"], stdout=subprocess.PIPE).stdout)
        videos = [s for s in probe["streams"] if s["codec_type"] == "video"]
        expected = [(90, 160), (180, 320)] if angle % 180 else [(160, 90), (320, 180)]
        if os.environ.get("MIST_ROTATION_PROCESSOR") == "FFMPEG":
            expected = [(180, 320)] if angle % 180 else [(320, 180)]
        assert sorted((s["width"], s["height"]) for s in videos) == sorted(expected), probe
        for index, video in enumerate(videos):
            packets = [p for p in probe["packets"] if p["stream_index"] == video["index"]]
            assert packets and float(packets[0]["pts_time"]) <= 0.15, packets[:1]
            assert float(packets[-1]["pts_time"]) >= 3.9, packets[-1:]
            assert not any(side.get("rotation", 0) for side in video.get("side_data_list", [])), video
            verify_corner(ffmpeg, work / "upright.mkv", index, video["width"], video["height"], angle, mirror)
        poster = Path("/tmp/mist_thumbs") / stream / "poster.jpg"
        wait_for(lambda: poster.exists() and poster.stat().st_size > 0, deadline=15)
        image = json.loads(run([ffprobe, "-v", "error", "-show_streams", "-of", "json", poster], stdout=subprocess.PIPE).stdout)["streams"][0]
        assert (image["width"], image["height"]) == ((44, 80) if angle % 180 else (80, 44)), image
        verify_corner(ffmpeg, poster, 0, image["width"], image["height"], angle, mirror)
        if os.environ.get("MIST_ROTATION_ACCEL") == "hw" and sys.platform == "darwin":
            diagnostics = (work / "input.log").read_text(errors="replace")
            assert "Video decoder backend=videotoolbox" in diagnostics, diagnostics
            assert "Video encoder backend=videotoolbox" in diagnostics, diagnostics
            assert "Video transform backend=vt" in diagnostics, diagnostics
        if os.environ.get("MIST_ROTATION_ACCEL") == "auto" and sys.platform == "darwin":
            diagnostics = (work / "input.log").read_text(errors="replace")
            assert "Video encoder backend=software" in diagnostics, diagnostics
            assert "Video encoder backend=videotoolbox" not in diagnostics, diagnostics
        success = True
        print(f"PASS {codec} -> {output_codec} rotation={angle} mirror={mirror}: local ladder and thumbnail pixels", flush=True)
    finally:
        if controller:
            subprocess.run([str(build / "MistUtilNuke"), stream], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=10)
        for process in processes:
            if isinstance(process, subprocess.Popen) and process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
        if controller and controller.poll() is None:
            controller.send_signal(signal.SIGINT)
            try:
                controller.wait(timeout=10)
            except subprocess.TimeoutExpired:
                controller.kill()
                controller.wait()
        shutil.rmtree(Path("/tmp/mist_thumbs") / stream, ignore_errors=True)
        if success and os.environ.get("MIST_KEEP_TEST_ARTIFACTS") != "1":
            shutil.rmtree(Path("/tmp/mist_thumbs") / stream, ignore_errors=True)
            shutil.rmtree(work)
        else:
            print(f"Integration artifacts: {work}", file=sys.stderr, flush=True)


def main():
    if os.environ.get("MIST_RUN_MEDIA_TESTS") != "1":
        print("Set MIST_RUN_MEDIA_TESTS=1 for isolated media integration", file=sys.stderr)
        return 77
    build, ffmpeg, ffprobe = Path(sys.argv[1]).resolve(), sys.argv[2], sys.argv[3]
    cases = [("H264", 90), ("H264", 180), ("H264", 270)]
    if os.environ.get("MIST_ORIENTATION_ALL") == "1":
        cases = [("H264", angle, mirror) for mirror in (False, True) for angle in (0, 90, 180, 270)]
    if os.environ.get("MIST_ROTATION_CASE"):
        values = os.environ["MIST_ROTATION_CASE"].split(":")
        cases = [(values[0], int(values[1]), bool(int(values[2])) if len(values) > 2 else False)]
    for _ in range(int(os.environ.get("MIST_MEDIA_REPEAT", "1"))):
        for case in cases:
            exercise(build, ffmpeg, ffprobe, *case)
    return 0


if __name__ == "__main__":
    sys.exit(main())
