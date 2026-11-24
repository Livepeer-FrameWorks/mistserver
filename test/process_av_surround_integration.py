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
import time
import urllib.parse
import urllib.request
from process_av_rotation_integration import run, wait_for



def record_rate_change(command, env, output_log, seconds, work, api_port, stream, packetization_ms):
    samples = []
    output = subprocess.Popen([str(value) for value in command], env=env, stdout=output_log, stderr=subprocess.STDOUT)
    deadline = time.monotonic() + seconds + 46
    try:
        while output.poll() is None:
            assert time.monotonic() < deadline, "rate-change recording did not finish"
            time.sleep(1)
            query = urllib.parse.urlencode({"command": json.dumps({"proc_list": stream})})
            with urllib.request.urlopen("http://127.0.0.1:%d/api2?%s" % (api_port, query), timeout=3) as response:
                updates = json.load(response).get("proc_list") or {}
            active = [(int(pid), update) for pid, update in updates.items()
                      if update.get("process") == "AV" and not update.get("terminated")]
            if not active:
                continue
            assert len(active) == 1, ("unexpected audio processors", active)
            pid, update = active[0]
            timing = update.get("ainfo", {})
            if not all(key in timing for key in ("sourceTime", "sinkTime")):
                continue
            memory = -1
            try:
                if sys.platform.startswith("linux"):
                    status = Path("/proc/%d/status" % pid).read_text()
                    memory = int(next(line for line in status.splitlines() if line.startswith("RssAnon:")).split()[1])
                else:
                    memory = int(subprocess.check_output(["ps", "-p", str(pid), "-o", "rss="], text=True))
            except (OSError, ValueError, StopIteration, subprocess.CalledProcessError):
                pass
            row = {"pid": pid, "source_ms": timing["sourceTime"], "sink_ms": timing["sinkTime"], "resident_kib": memory}
            row["lag_ms"] = row["source_ms"] - row["sink_ms"]
            samples.append(row)
            if len(samples) % 5 == 0:
                print("RESAMPLER " + json.dumps(row), flush=True)
        assert output.returncode == 0, ("rate-change recording failed", output.returncode)
    finally:
        (work / "resampler-samples.json").write_text(json.dumps(samples, indent=2) + "\n")
        if output.poll() is None:
            output.terminate()
            try:
                output.wait(timeout=5)
            except subprocess.TimeoutExpired:
                output.kill()
                output.wait()
    steady = [row for row in samples if 8000 <= row["source_ms"] < (seconds - 2) * 1000]
    assert len(steady) >= 10, ("too few while-running rate-change observations", samples)
    assert len({row["pid"] for row in steady}) == 1, ("resampler restarted", steady)
    assert max(row["source_ms"] for row in steady) >= (seconds - 5) * 1000, ("resampler stopped progressing", steady[-1])
    # Source time marks the start of a PES; the sink can already contain its later audio frames.
    assert all(-packetization_ms <= row["lag_ms"] <= 100 for row in steady), ("while-running resampler exceeds the 100 ms lag or input PES lead", packetization_ms, steady)
    resident = [row["resident_kib"] for row in steady[-20:]]
    assert len(resident) >= 10 and min(resident) > 0, ("missing while-running process memory", resident)
    # AddressSanitizer keeps freed blocks in its quarantine (256 MB by default), so resident memory
    # grows with every allocation the encoder frees; a leak is LeakSanitizer's to report at exit.
    if os.environ.get("MIST_ASAN") != "1":
        assert max(resident) - min(resident) <= 2048, ("resampler resident memory grew after the rate change", min(resident), max(resident))
    print("PASS while-running resampler: lag=%d..%d ms; resident=%d..%d KiB" %
          (min(row["lag_ms"] for row in steady), max(row["lag_ms"] for row in steady), min(resident), max(resident)), flush=True)


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
        rate_changing = os.environ.get("MIST_AUDIO_RATE_CHANGE") == "1"
        input_codec = "MP3" if rate_changing else os.environ.get("MIST_AUDIO_CODEC", "AAC")
        channels = 1 if os.environ.get("MIST_AUDIO_MONO") == "1" else 2 if input_codec in ("MP3", "MP2") else 6
        maximum = int(os.environ.get("MIST_AUDIO_MAX_CHANNELS", "0"))
        output_channels = min(channels, maximum) if maximum else channels
        changing = rate_changing or os.environ.get("MIST_AUDIO_FORMAT_CHANGE") == "1"
        if rate_changing:
            output_channels = 2
        second_phase = 60 if rate_changing else 4
        seconds = 4 + second_phase if changing else 4
        source = work / ("surround.mp4" if input_codec == "AAC" else "surround.mkv")
        fixture_signal = "sine=frequency=440:sample_rate=48000:duration=4" if channels == 1 else "aevalsrc=sin(2*PI*440*t)|sin(2*PI*550*t):s=48000:d=4:channel_layout=stereo" if channels == 2 else "aevalsrc=sin(2*PI*440*t)|sin(2*PI*550*t)|sin(2*PI*660*t)|sin(2*PI*80*t)|sin(2*PI*770*t)|sin(2*PI*880*t):s=48000:d=4:channel_layout=5.1"
        run([ffmpeg, "-v", "error", "-y", "-f", "lavfi", "-i", fixture_signal, "-c:a", {"AAC": "aac", "AC3": "ac3", "EAC-3": "eac3", "MP2": "mp2", "MP3": "libmp3lame"}[input_codec], "-b:a", "192k" if channels == 2 else "384k", source])
        if changing:
            segments = []
            packetization_ms = 0
            for index, rate in enumerate((44100, 8000) if rate_changing else (48000, 44100)):
                segment_seconds = 4 if index == 0 else second_phase
                segment = work / ("part%d.ts" % index)
                generation = [ffmpeg, "-v", "error", "-y", "-f", "lavfi", "-i", "testsrc2=size=320x180:rate=10:duration=%d" % segment_seconds,
                     "-f", "lavfi", "-i", "aevalsrc=sin(2*PI*440*t):s=%d:d=%d" % (rate, segment_seconds),
                     "-c:v", "libx264", "-threads", "1", "-preset", "ultrafast", "-g", "10", "-bf", "0",
                     "-c:a", "libmp3lame" if rate_changing else "ac3", "-ac", "2" if rate_changing else ("6" if index == 0 else "2"), "-ar", str(rate), "-b:a", "24k" if rate_changing else "384k",
                     "-output_ts_offset", str(index * 4), segment]
                run(generation)
                if rate_changing:
                    packets = json.loads(run([ffprobe, "-v", "error", "-select_streams", "a", "-show_packets", "-of", "json", segment], stdout=subprocess.PIPE).stdout)["packets"]
                    packet_seconds = 0
                    for packet in packets:
                        if packet.get("pos") is not None:
                            packet_seconds = 0
                        packet_seconds += float(packet["duration_time"])
                        packetization_ms = max(packetization_ms, math.ceil(packet_seconds * 1000))
                    if index:
                        # MP3 priming differs by rate; join the encoded packets without a timestamp overlap.
                        correction = previous_end - float(packets[0]["pts_time"])
                        generation[generation.index("-output_ts_offset") + 1] = "%.6f" % (index * 4 + correction)
                        run(generation)
                    else:
                        previous_end = float(packets[-1]["pts_time"]) + float(packets[-1]["duration_time"])
                segments.append(segment.read_bytes())
            source = work / "changing.ts"
            source.write_bytes(b"".join(segments))
        processes = [{"process": "AV", "x-LSP-kind": "audio", "codec": codec,
                      "track_select": "audio=%s&video=none&subtitle=none&meta=none" % ("MP3" if rate_changing else "AC3" if changing else input_codec), "source_mask": 4,
                      "max_channels": maximum, "target_mask": 3, "restart_type": "fixed", "bitrate": 384000 if codec == "AAC" else 128000}
                     for codec in (("AAC",) if changing and not rate_changing else ("opus",))]
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
            source_process = subprocess.Popen([str(build / ("MistInTS" if changing else "MistInMP4" if input_codec == "AAC" else "MistInEBML")), "-r", "-s", stream, str(source)], env=env, stdout=input_log, stderr=subprocess.STDOUT)
            wait_for(lambda: "Input started" in (work / "input.log").read_text(errors="replace"))
            recording = work / "surround.mkv"
            command = [build / "MistOutEBML", "-s", stream, str(recording) + f"?video=none&audio=all&subtitle=none&meta=none&duration={seconds}"]
            if rate_changing:
                record_rate_change(command, env, output_log, seconds, work, api_port, stream, packetization_ms)
            else:
                run(command, env=env, stdout=output_log, stderr=subprocess.STDOUT, timeout=50)
        probe = json.loads(run([ffprobe, "-v", "error", "-show_streams", "-show_packets", "-of", "json", recording], stdout=subprocess.PIPE).stdout)
        audio = probe["streams"]
        assert {(s["codec_name"], s["channels"]) for s in audio} == {("aac" if changing and not rate_changing else "opus", output_channels)}, audio
        for index, channel_count in enumerate(s["channels"] for s in audio):
            packets = [p for p in probe["packets"] if p["stream_index"] == index]
            assert packets, audio
            assert seconds - 0.2 <= float(packets[-1]["pts_time"]) <= seconds + 0.1, packets[-1]
            samples = run([ffmpeg, "-v", "error", "-i", recording, "-map", f"0:{index}", "-f", "s16le", "-"], stdout=subprocess.PIPE).stdout
            decoded_seconds = len(samples) / (48000 * channel_count * 2)
            assert seconds - 0.2 <= decoded_seconds <= seconds + 0.1, ("decoded audio duration", decoded_seconds, seconds)
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
        print(f"PASS {'changing MP3' if rate_changing else 'changing AC3' if changing else input_codec} {channels} channels -> {'AAC' if changing and not rate_changing else 'Opus'} {output_channels} channels; full-duration decoding", flush=True)
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
