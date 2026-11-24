#!/usr/bin/env python3
"""The first HLS viewer of a cold pulled replica gets a playlist.

A replica pulls a live stream over DTSC when its first viewer arrives. Its track index 0 can be a
thumbnail sprite (a JPEG track with a keyframe every few seconds), and its video can carry long
GOPs, so neither has the fragments HLS waits for when the viewer asks. The viewer must still get a
playlist: HLS judges readiness on the stream's continuous video, stops waiting for a full window
after a bounded time, and the buffer stays up while a viewer waits for it, instead of shutting
down after its input timeout because no viewer had selected a track yet.
"""
import os
from pathlib import Path
import socket
import subprocess
import sys
import time
import urllib.request

from controller_capacity_lifecycle_integration import Fixture


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def main():
    if os.environ.get("MIST_RUN_MEDIA_TESTS") != "1":
        return 77
    build, ffmpeg = Path(sys.argv[1]).resolve(), sys.argv[2]
    fixture = Fixture(build / "MistController", ffmpeg)
    success = False
    try:
        source = fixture.root / "sprite-first.mkv"
        # Track 0: a JPEG sprite with a frame every 10 s. Track 1: H264 with 2.5 s GOPs.
        subprocess.run([ffmpeg, "-v", "error", "-y",
                        "-f", "lavfi", "-i", "testsrc2=size=160x90:rate=0.1:duration=60",
                        "-f", "lavfi", "-i", "testsrc2=size=320x180:rate=10:duration=60",
                        "-map", "0:v", "-map", "1:v", "-c:v:0", "mjpeg", "-c:v:1", "libx264", "-threads", "1",
                        "-preset", "ultrafast", "-g", "25", "-keyint_min", "25", "-sc_threshold", "0", "-bf", "0",
                        str(source)], env=fixture.tool_env, check=True, timeout=60)
        dtsc = free_port()
        origin = "origin" + str(os.getpid())
        # The first replica's viewer arrives while the origin starts and allows HLS 10 s to fill its
        # playlist window, past the replica's 6 s input timeout: only a buffer that counts the
        # waiting viewer is still up to serve it. The second is the default replica.
        replicas = {"shortreplica" + str(os.getpid()): ({"inputtimeout": 6}, "?maxwaittrackms=10000"),
                    "replica" + str(os.getpid()): ({}, "")}
        streams = {origin: {"source": str(source)}}
        for name, (options, _) in replicas.items():
            streams[name] = dict(options, source="dtsc://127.0.0.1:%d/%s" % (dtsc, origin))
        fixture.start(streams_override=streams, extra_protocols=[{"connector": "DTSC", "interface": "127.0.0.1", "port": dtsc}])
        fixture.streams.update(streams)
        feeder_log = (fixture.root / "feeder.log").open("wb")
        fixture.children.append(subprocess.Popen([str(build / "MistInEBML"), "-r", "-s", origin, str(source)],
                                                 env=fixture.env, stdout=feeder_log, stderr=subprocess.STDOUT))
        time.sleep(1)
        for replica, (_, query) in replicas.items():
            started = time.monotonic()
            url = "http://127.0.0.1:%d/hls/%s/index.m3u8%s" % (fixture.http, replica, query)
            with urllib.request.urlopen(url, timeout=60) as response:
                playlist = response.read().decode(errors="replace")
            waited = time.monotonic() - started
            assert response.status == 200 and playlist.startswith("#EXTM3U") and ".m3u8" in playlist, (replica, waited, playlist)
            assert waited < 14, ("the first viewer waited too long", replica, waited)
            print("PASS the first HLS viewer of cold replica %s got its playlist after %.1f s" % (replica, waited), flush=True)
        success = True
        return 0
    finally:
        fixture.close(failed=not success)


if __name__ == "__main__":
    sys.exit(main())
