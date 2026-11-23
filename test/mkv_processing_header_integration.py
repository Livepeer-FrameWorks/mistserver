#!/usr/bin/env python3
"""A Matroska upload's processing recording declares every rendition in its header.

MistInEBML registers a Matroska source's tracks as their first frames arrive, so the processing
buffer can resolve its graph while the video track is new and its rendition decisions are still
waiting for the track's bitrate. The recording must not write its header before the graph
resolved for every original track it records: a rendition that appears after the header aborts
the recording (PROCESS_TRACKS_CHANGED, "was not declared in the recording header").
"""
import json
import os
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


def retain_selector(short, bitrate):
    window = max(32, short // 20)
    low_bytes = (bitrate + 15) // 16
    high_bytes = bitrate // 4
    return (f"video=h264,hevc,|source,|>0x{short - window - 1},|<0x{short + window + 1},"
            f"|>{(low_bytes - 1) // 128}kbps,|<{high_bytes // 128 + 1}kbps")


def exercise(build, ffmpeg, ffprobe):
    work = Path(tempfile.mkdtemp(prefix="mist-mkv-header-"))
    env = dict(os.environ, TMP=str(work / "ipc"), MIST_CONTROL="1", PATH=str(build) + os.pathsep + os.environ["PATH"],
               UDP_API="udp://127.0.0.1:0")
    Path(env["TMP"]).mkdir()
    stream = "mkvheader" + str(os.getpid())
    processes = []
    controller = None
    success = False
    try:
        cue = work / "cue.srt"
        cue.write_text("1\n00:00:00,500 --> 00:00:02,000\nstack subtitle cue\n")
        mp4 = work / "h264-surround.mp4"
        run([ffmpeg, "-v", "error", "-y", "-f", "lavfi", "-i", "testsrc2=size=960x540:rate=10:duration=6",
             "-f", "lavfi", "-i", "anullsrc=channel_layout=5.1:sample_rate=48000", "-i", cue, "-t", "6",
             "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p", "-g", "10", "-bf", "0",
             "-map", "0:v", "-map", "1:a", "-map", "2:s", "-c:a", "aac", "-b:a", "384k", "-c:s", "mov_text", mp4])
        source = work / "surround.mkv"
        run([ffmpeg, "-v", "error", "-y", "-i", mp4, "-c", "copy", "-c:s", "srt", source])
        # The local ladder FrameWorks composes for an upload: H264 rungs that an in-band original
        # could retain, an AAC encode for the surround audio and thumbnails.
        ladder = []
        for short, bitrate in ((360, 900000), (480, 1600000)):
            ladder.append({"process": "AV", "codec": "H264", "resolution": f"x{short}", "bitrate": bitrate,
                           "track_select": "video=maxbps&audio=none&subtitle=none&meta=none",
                           "track_inhibit": f"video=<0x{short}", "track_retain": retain_selector(short, bitrate),
                           "mask_replaced_originals": True, "source_mask": 4, "target_mask": 3, "accel": "sw",
                           "preset": "ultrafast", "gopsize": 10, "restart_type": "disabled", "x-LSP-name": f"Local {short}p"})
        ladder.append({"process": "AV", "codec": "AAC", "track_select": "audio=all&video=none&subtitle=none&meta=none",
                       "target_mask": 3, "restart_type": "disabled"})
        ladder.append({"process": "Thumbs", "track_select": "video=maxbps", "thumb_width": 80, "thumb_height": 80,
                       "grid_cols": 2, "grid_rows": 2, "interval": 1000, "inconsequential": True, "target_mask": 3,
                       "restart_type": "disabled"})
        with socket.socket() as port:
            port.bind(("127.0.0.1", 0))
            api_port = port.getsockname()[1]
        config = {"account": {"test": {"password": "098f6bcd4621d373cade4e832627b4f6"}},
                  "config": {"debug": 4, "device_discovery": False, "protocols": [],
                             "controller": {"interface": "127.0.0.1", "port": api_port, "username": None},
                             "sessionInputMode": 15, "sessionOutputMode": 15, "sessionStreamInfoMode": 1,
                             "sessionUnspecifiedMode": 0, "sessionViewerMode": 14, "tknMode": 15, "triggers": {}},
                  "streams": {stream: {"name": stream, "source": str(source), "process_controlled_realtime": True,
                                       "realtime_speed": 1, "processes": ladder}}}
        (work / "config.json").write_text(json.dumps(config))
        log = work / "controller.log"
        controller = subprocess.Popen([str(build / "MistController"), "-c", str(work / "config.json"), "-C", "r", "-L", str(log)],
                                      env=env)
        wait_for(lambda: log.exists() and "Controller started" in log.read_text(errors="replace"))
        with (work / "input.log").open("wb") as input_log:
            source_process = subprocess.Popen([str(build / "MistInEBML"), "-r", "-s", stream, str(source)], env=env,
                                              stdout=input_log, stderr=subprocess.STDOUT)
            processes.append(source_process)
            wait_for(lambda: "Input started" in (work / "input.log").read_text(errors="replace"))
            with (work / "output.log").open("wb") as output_log:
                run([build / "MistOutEBML", "-s", stream,
                     str(work / "recording.mkv") + "?video=all&audio=all&subtitle=all&meta=none&duration=6"],
                    env=env, stdout=output_log, stderr=subprocess.STDOUT, timeout=90)
        output = (work / "output.log").read_text(errors="replace")
        assert "was not declared in the recording header" not in output, output[-4000:]
        probe = json.loads(run([ffprobe, "-v", "error", "-show_streams", "-of", "json", work / "recording.mkv"],
                               stdout=subprocess.PIPE).stdout)
        heights = sorted(s["height"] for s in probe["streams"] if s["codec_type"] == "video" and s["codec_name"] == "h264")
        assert heights == [360, 480], probe
        assert any(s["codec_type"] == "audio" for s in probe["streams"]), probe
        success = True
        print("PASS Matroska upload recording declares the ladder renditions in its header", flush=True)
    finally:
        if controller:
            subprocess.run([str(build / "MistUtilNuke"), stream], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                           timeout=10)
        for process in processes:
            if process.poll() is None:
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
            shutil.rmtree(work)
        else:
            print(f"Integration artifacts: {work}", file=sys.stderr, flush=True)


def main():
    if os.environ.get("MIST_RUN_MEDIA_TESTS") != "1":
        print("Set MIST_RUN_MEDIA_TESTS=1 for isolated media integration", file=sys.stderr)
        return 77
    build, ffmpeg, ffprobe = Path(sys.argv[1]).resolve(), sys.argv[2], sys.argv[3]
    for _ in range(int(os.environ.get("MIST_MEDIA_REPEAT", "1"))):
        exercise(build, ffmpeg, ffprobe)
    return 0


if __name__ == "__main__":
    sys.exit(main())
