#!/usr/bin/env python3
import json
import array
import math
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
    work = Path(tempfile.mkdtemp(prefix="mist-av-surround-"))
    env = dict(os.environ, TMP=str(work / "ipc"), MIST_CONTROL="1", UDP_API="udp://127.0.0.1:0")
    Path(env["TMP"]).mkdir()
    stream = "surround" + str(os.getpid())
    controller, source_process, success = None, None, False
    try:
        channels = 1 if os.environ.get("MIST_AUDIO_MONO") == "1" else 6
        maximum = int(os.environ.get("MIST_AUDIO_MAX_CHANNELS", "0"))
        output_channels = min(channels, maximum) if maximum else channels
        changing = os.environ.get("MIST_AUDIO_FORMAT_CHANGE") == "1"
        seconds = 8 if changing else 4
        source = work / "surround.mp4"
        fixture_signal = "sine=frequency=440:sample_rate=48000:duration=4" if channels == 1 else "aevalsrc=sin(2*PI*440*t)|sin(2*PI*550*t)|sin(2*PI*660*t)|sin(2*PI*80*t)|sin(2*PI*770*t)|sin(2*PI*880*t):s=48000:d=4:channel_layout=5.1"
        run([ffmpeg, "-v", "error", "-y", "-f", "lavfi", "-i", fixture_signal, "-c:a", "aac", "-b:a", "384k", source])
        if changing:
            segments = []
            for index, rate in enumerate((48000, 44100)):
                segment = work / ("part%d.ts" % index)
                run([ffmpeg, "-v", "error", "-y", "-f", "lavfi", "-i", "testsrc2=size=320x180:rate=10:duration=4",
                     "-f", "lavfi", "-i", "aevalsrc=sin(2*PI*440*t):s=%d:d=4" % rate,
                     "-c:v", "libx264", "-threads", "1", "-preset", "ultrafast", "-g", "10", "-bf", "0",
                     "-c:a", "ac3", "-ac", "6" if index == 0 else "2", "-ar", str(rate), "-b:a", "384k",
                     "-output_ts_offset", str(index * 4), segment])
                segments.append(segment.read_bytes())
            source = work / "changing.ts"
            source.write_bytes(b"".join(segments))
        processes = [{"process": "AV", "x-LSP-kind": "audio", "codec": codec,
                      "track_select": "audio=%s&video=none&subtitle=none&meta=none" % ("AC3" if changing else "AAC"), "source_mask": 4,
                      "max_channels": maximum, "target_mask": 3, "restart_type": "fixed", "bitrate": 384000 if codec == "AAC" else 128000}
                     for codec in (("AAC",) if changing else ("opus",))]
        with socket.socket() as port:
            port.bind(("127.0.0.1", 0))
            api_port = port.getsockname()[1]
        config = {"config": {"debug": 4, "device_discovery": False, "protocols": [],
                             "controller": {"interface": "127.0.0.1", "port": api_port, "username": None},
                             "sessionInputMode": 15, "sessionOutputMode": 15, "sessionStreamInfoMode": 1,
                             "sessionUnspecifiedMode": 0, "sessionViewerMode": 14, "tknMode": 15, "triggers": {}},
                  "streams": {stream: {"name": stream, "source": str(source), "process_controlled_realtime": True,
                                       "realtime_speed": 1, "processes": processes}}}
        (work / "config.json").write_text(json.dumps(config))
        log = work / "controller.log"
        controller = subprocess.Popen([str(build / "MistController"), "-c", str(work / "config.json"), "-C", "r", "-L", str(log)], env=env)
        wait_for(lambda: log.exists() and "Controller started" in log.read_text(errors="replace"))
        with (work / "input.log").open("wb") as input_log, (work / "output.log").open("wb") as output_log:
            source_process = subprocess.Popen([str(build / ("MistInTS" if changing else "MistInMP4")), "-r", "-s", stream, str(source)], env=env, stdout=input_log, stderr=subprocess.STDOUT)
            wait_for(lambda: "Input started" in (work / "input.log").read_text(errors="replace"))
            recording = work / "surround.mkv"
            run([build / "MistOutEBML", "-s", stream, str(recording) + f"?video=none&audio=all&subtitle=none&meta=none&duration={seconds}"],
                env=env, stdout=output_log, stderr=subprocess.STDOUT, timeout=50)
        probe = json.loads(run([ffprobe, "-v", "error", "-show_streams", "-show_packets", "-of", "json", recording], stdout=subprocess.PIPE).stdout)
        audio = probe["streams"]
        assert {(s["codec_name"], s["channels"]) for s in audio} == {("aac" if changing else "opus", output_channels)}, audio
        for index, channel_count in enumerate(s["channels"] for s in audio):
            packets = [p for p in probe["packets"] if p["stream_index"] == index]
            assert packets, audio
            assert float(packets[-1]["pts_time"]) >= seconds - 0.2, packets[-1]
            samples = run([ffmpeg, "-v", "error", "-i", recording, "-map", f"0:{index}", "-f", "s16le", "-"], stdout=subprocess.PIPE).stdout
            assert len(samples) >= 48000 * channel_count * 2 * (seconds - 0.2), len(samples)
            if channel_count == 6 and not changing:
                pcm = array.array("h", samples)
                if sys.byteorder != "little":
                    pcm.byteswap()
                frequencies = (440, 550, 660, 80, 770, 880)
                for channel, expected in enumerate(frequencies):
                    sample_start = 0.5
                    values = pcm[int(48000 * 6 * sample_start) + channel:int(48000 * 6 * (sample_start + 0.5)):6]
                    powers = []
                    for frequency in frequencies:
                        coefficient = 2 * math.cos(2 * math.pi * frequency / 48000)
                        previous = before = 0.0
                        for value in values:
                            current = value + coefficient * previous - before
                            before, previous = previous, current
                        powers.append(previous * previous + before * before - coefficient * previous * before)
                    assert frequencies[max(range(6), key=lambda i: powers[i])] == expected, (channel, expected, powers)

        if changing:
            log_text = (work / "input.log").read_text(errors="replace")
            assert "Decoded audio format changed; draining and rebuilding the resampler" in log_text, log_text[-3000:]
            assert all(track["sample_rate"] == "48000" for track in audio), audio
            assert not any(word in log_text for word in ("Could not open resample", "Could not flush audio", "Error while")), log_text[-3000:]
        success = True
        print(f"PASS {'changing AC3' if changing else 'AAC'} {channels} channels -> {'AAC' if changing else 'Opus'} {output_channels} channels; full-duration decoding", flush=True)
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
            print(f"Surround integration artifacts: {work}", file=sys.stderr)


if __name__ == "__main__":
    status = 0
    for _iteration in range(int(os.environ.get("MIST_MEDIA_REPEAT", "1"))):
        status = main()
        if status:
            break
    sys.exit(status)
