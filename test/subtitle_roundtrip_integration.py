#!/usr/bin/env python3
import json
import os
from pathlib import Path
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
from process_av_rotation_integration import run, wait_for


def main():
    if os.environ.get("MIST_RUN_MEDIA_TESTS") != "1":
        return 77
    build, ffmpeg, ffprobe = Path(sys.argv[1]).resolve(), sys.argv[2], sys.argv[3]
    work = Path(tempfile.mkdtemp(prefix="mist-subtitle-roundtrip-"))
    env = dict(os.environ, TMP=str(work / "ipc"), MIST_CONTROL="1", UDP_API="udp://127.0.0.1:0")
    Path(env["TMP"]).mkdir()
    stream = "subtitles" + str(os.getpid())
    controller = source_process = None
    success = False
    try:
        cue = work / "cue.srt"
        late = os.environ.get("MIST_SUBTITLE_LATE") == "1"
        sparse = os.environ.get("MIST_SUBTITLE_SPARSE") == "1"
        length = 184 if sparse else 4
        start, duration = (180.0, 0.75) if sparse else (3.0, 0.75) if late else (0.5, 1.5)
        end = "00:03:00,750" if sparse else "00:00:03,750" if late else "00:00:02,000"
        cue.write_text(f"1\n{'00:03:00,000' if sparse else '00:00:03,000' if late else '00:00:00,500'} --> {end}\nhello roundtrip\n")
        container = os.environ.get("MIST_SUBTITLE_CONTAINER", "mkv")
        default_selection = os.environ.get("MIST_SUBTITLE_DEFAULT") == "1"
        source = work / ("source." + container)
        encoder = ["-c:v", "libx264", "-preset", "ultrafast", "-c:s", "mov_text"] if container == "mp4" else ["-c:v", "libvpx-vp9", "-deadline", "realtime", "-cpu-used", "8", "-c:s", "srt"]
        processed = os.environ.get("MIST_SUBTITLE_PROCESSED") == "1"
        if processed:
            encoder = ["-c:v", "mpeg4", "-q:v", "3", "-c:s", "srt"]
        extensions = ("mkv",) if container == "mp4" or processed or default_selection else ("mkv", "webm")
        processors = []
        if processed:
            processors = [
                {"process": "AV", "codec": "H264", "resolution": "128x72", "accel": "sw", "preset": "ultrafast",
                 "tune": "zerolatency", "bitrate": 100000, "gopsize": 10, "source_mask": 4, "target_mask": 3,
                 "track_select": "video=MPEG4&audio=none&meta=none&subtitle=none", "restart_type": "fixed"},
                {"process": "Thumbs", "track_select": "video=MPEG4", "thumb_width": 80, "thumb_height": 80,
                 "grid_cols": 2, "grid_rows": 2, "interval": 1000, "target_mask": 3, "restart_type": "fixed"}]

        run([ffmpeg, "-v", "error", "-y", "-f", "lavfi", "-i", f"color=blue:size=160x90:rate=10:duration={length}",
             "-i", cue, *encoder, "-g", "10", source])
        with socket.socket() as port:
            port.bind(("127.0.0.1", 0))
            api_port = port.getsockname()[1]
        config = {"config": {"debug": 4, "device_discovery": False, "protocols": [],
                             "controller": {"interface": "127.0.0.1", "port": api_port, "username": None},
                             "sessionInputMode": 15, "sessionOutputMode": 15, "sessionStreamInfoMode": 1,
                             "sessionUnspecifiedMode": 0, "sessionViewerMode": 14, "tknMode": 15, "triggers": {}},
                  "streams": {stream: {"name": stream, "source": str(source), "process_controlled_realtime": True,
                                       "realtime_speed": 20 if sparse else 1, "processes": processors}}}
        (work / "config.json").write_text(json.dumps(config))
        log = work / "controller.log"
        controller = subprocess.Popen([str(build / "MistController"), "-c", str(work / "config.json"), "-C", "r", "-L", str(log)], env=env)
        wait_for(lambda: log.exists() and "Controller started" in log.read_text(errors="replace"))
        with (work / "input.log").open("wb") as input_log:
            source_process = subprocess.Popen([str(build / ("MistInMP4" if container == "mp4" else "MistInEBML")), "--debug", "4", "-r", "-s", stream, str(source)], env=env, stdout=input_log, stderr=subprocess.STDOUT)
            wait_for(lambda: "Input started" in (work / "input.log").read_text(errors="replace"))
            recordings = []
            for extension in extensions:
                output = work / ("recording." + extension)
                output_log = (work / ("output-" + extension + ".log")).open("wb")
                process = subprocess.Popen([str(build / "MistOutEBML"), "-s", stream, str(output) + (f"?video=all&audio=none&duration={length}" if default_selection else f"?video=all&audio=none&subtitle=all&meta=all&duration={length}")], env=env, stdout=output_log, stderr=subprocess.STDOUT)
                recordings.append((process, output_log))
            if sparse:
                # The file header must be written before the first cue arrives at 9s wall time.
                # Waiting for subtitle packets lets the bounded live buffer discard the start.
                wait_for(lambda: all((work / ("recording." + ext)).exists() and (work / ("recording." + ext)).stat().st_size > 0 for ext in extensions), deadline=4)
            for process, output_log in recordings:
                assert process.wait(timeout=65) == 0
                output_log.close()
        for extension in extensions:
            output = work / ("recording." + extension)
            probe = json.loads(run([ffprobe, "-v", "error", "-show_streams", "-show_packets", "-of", "json", output], stdout=subprocess.PIPE).stdout)
            for video in (s for s in probe["streams"] if s["codec_type"] == "video" and s["codec_name"] != "mjpeg"):
                packets = [p for p in probe["packets"] if p["stream_index"] == video["index"]]
                assert packets and float(packets[0]["pts_time"]) < 0.2, packets[:1]
                assert float(packets[-1]["pts_time"]) >= length - 0.3, packets[-1:]
            subtitles = [s for s in probe["streams"] if s["codec_type"] == "subtitle"]
            assert len(subtitles) == (1 if extension == "mkv" and not default_selection else 0), probe
            if subtitles:
                index = subtitles[0]["index"]
                packet = next(p for p in probe["packets"] if p["stream_index"] == index)
                assert abs(float(packet["pts_time"]) - start) < 0.02 and abs(float(packet["duration_time"]) - duration) < 0.02, packet
                text = run([ffmpeg, "-v", "error", "-i", output, "-map", "0:s:0", "-f", "srt", "-"], stdout=subprocess.PIPE).stdout
                assert b"hello roundtrip" in text and end.encode() in text, text
        if os.environ.get("MIST_SUBTITLE_HTTP") == "1":
            import urllib.request
            from controller_capacity_lifecycle_integration import Fixture
            playback = Fixture(build / "MistController", ffmpeg)
            played = False
            try:
                playback.start(streams_override={playback.stream: {"source": str(work / "recording.mkv")}},
                               extra_protocols=[{"connector": "SubRip"}])
                url = f"http://127.0.0.1:{playback.http}/{playback.stream}.vtt"
                with urllib.request.urlopen(url, timeout=30) as response:
                    assert response.headers.get_content_type() == "text/vtt", response.headers
                    body = response.read().decode()
                assert body.startswith("WEBVTT") and "hello roundtrip" in body, body
                assert "00:00:00.500 --> 00:00:02.000" in body, body
                played = True
                print("PASS stored MKV serves subtitle text and duration through WebVTT", flush=True)
            finally:
                playback.close(failed=not played)
        success = True
        print(f"PASS {container} subtitle cue text/timing survives recording ({extensions})", flush=True)
        return 0
    finally:
        if controller:
            subprocess.run([str(build / "MistUtilNuke"), stream], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=10)
        for process in (source_process, controller):
            if process and process.poll() is None:
                process.send_signal(signal.SIGINT)
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
        if success and os.environ.get("MIST_KEEP_TEST_ARTIFACTS") != "1":
            shutil.rmtree(work)
        else:
            print(f"Subtitle integration artifacts: {work}", file=sys.stderr)


if __name__ == "__main__":
    status = 0
    for _iteration in range(int(os.environ.get("MIST_MEDIA_REPEAT", "1"))):
        status = main()
        if status:
            break
    sys.exit(status)
