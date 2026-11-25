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
import subprocess
import sys
import time
import urllib.request

from controller_capacity_lifecycle_integration import Fixture, until
from test_ports import free_port


def main():
    if os.environ.get("MIST_RUN_MEDIA_TESTS") != "1":
        return 77
    build, ffmpeg = Path(sys.argv[1]).resolve(), sys.argv[2]
    fixture = Fixture(build / "MistController", ffmpeg)
    success = False
    try:
        origin = "origin" + str(os.getpid())
        long_origin = "longorigin" + str(os.getpid())
        sources = {}
        # Keep the short input-timeout case at 2.5 s GOPs. The separate 5 s GOP
        # case cannot fill five fragments inside the 14 s first-viewer bound.
        for name, gop in ((origin, 25), (long_origin, 50)):
            source = fixture.root / (name + ".mkv")
            sources[name] = source
            subprocess.run([ffmpeg, "-v", "error", "-y",
                        "-f", "lavfi", "-i", "testsrc2=size=160x90:rate=0.1:duration=60",
                        "-f", "lavfi", "-i", "testsrc2=size=320x180:rate=10:duration=60",
                        *(["-map", "0:v", "-map", "1:v", "-c:v:0", "mjpeg", "-c:v:1", "libx264"]
                          if name == origin else ["-map", "1:v", "-c:v", "libx264"]), "-threads", "1",
                        "-preset", "ultrafast", "-g", str(gop), "-keyint_min", str(gop), "-sc_threshold", "0", "-bf", "0",
                        str(source)], env=fixture.tool_env, check=True, timeout=60)
        dtsc = free_port()
        # The short replica's viewer allows HLS 10 s to fill its
        # playlist window, past the replica's 6 s input timeout: only a buffer that counts the
        # waiting viewer is still up to serve it. The second is the default replica.
        replicas = {"longreplica" + str(os.getpid()): ({"inputtimeout": 60}, "", long_origin),
                    "shortreplica" + str(os.getpid()): ({"inputtimeout": 6}, "?maxwaittrackms=10000", origin),
                    "replica" + str(os.getpid()): ({}, "", origin)}
        streams = {name: {"source": str(source)} for name, source in sources.items()}
        for name, (options, _, source_origin) in replicas.items():
            streams[name] = dict(options, source="dtsc://127.0.0.1:%d/%s" % (dtsc, source_origin))
        fixture.start(streams_override=streams, extra_protocols=[{"connector": "DTSC", "interface": "127.0.0.1", "port": dtsc}])
        fixture.streams.update(streams)
        for name, source in sources.items():
            feeder_log = (fixture.root / (name + ".log")).open("wb")
            fixture.children.append(subprocess.Popen([str(build / "MistInEBML"), "-r", "-s", name, str(source)],
                                                 env=fixture.env, stdout=feeder_log, stderr=subprocess.STDOUT))
        # The origin must be the realtime feeder's live stream before a replica pulls it: a pull that
        # arrives first boots the origin from its file as VOD instead (a slow, instrumented feeder
        # loses that race), and the replica then receives the whole file at once.
        def origin_live():
            info = fixture.request({"active_streams": {"longform": True, "fields": ["status", "tracks", "lastms"]}})
            active = info.get("active_streams") or {}
            return all((active.get(name) or {}).get("status") == "Online" and
                       (active.get(name) or {}).get("tracks", 0) >= (2 if name == origin else 1) and
                       (active.get(name) or {}).get("lastms", 0) > 0 for name in sources)
        until(lambda: all(b"Input started" in (fixture.root / (name + ".log")).read_bytes() for name in sources) and origin_live(), 60,
              "the realtime origin did not come online")
        for replica, (_, query, _) in replicas.items():
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
