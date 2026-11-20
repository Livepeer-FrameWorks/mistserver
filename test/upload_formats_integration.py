#!/usr/bin/env python3
import json
import http.server
import re
import threading
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def run(args):
    result = subprocess.run([str(v) for v in args], stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=40)
    if result.returncode:
        raise AssertionError(f"{args}: {result.stderr.decode(errors='replace')}")
    return result.stdout


def tracks(build, kind, path):
    metadata = json.loads(run([build / "test" / ("uploadmetaprobe_" + kind), path]))
    return list(metadata["tracks"].values())


def main():
    if os.environ.get("MIST_RUN_MEDIA_TESTS") != "1":
        return 77
    build, ffmpeg = Path(sys.argv[1]).resolve(), sys.argv[2]
    work = Path(tempfile.mkdtemp(prefix="mist-upload-formats-"))
    success = False
    try:
        for codec in ("ac3", "eac3"):
            for container, kind in [("mp4", "mp4"), ("mkv", "ebml"), ("ts", "ts")]:
                path = work / (codec + "." + container)
                run([ffmpeg, "-v", "error", "-y", "-f", "lavfi", "-i", "anullsrc=channel_layout=5.1:sample_rate=48000",
                     "-t", "2", "-c:a", codec, "-b:a", "192k", path])
                audio = tracks(build, kind, path)
                assert len(audio) == 1 and audio[0]["codec"] == ("EAC-3" if codec == "eac3" else codec.upper()), audio
                assert audio[0]["channels"] == 6 and audio[0]["rate"] == 48000, audio
                if codec == "eac3":
                    assert audio[0]["init"] and audio[0]["codecstring"] == "ec-3", audio
                if codec == "eac3" and container == "mkv":
                    av_audio = tracks(build, "av", path)
                    assert av_audio[0]["init"] and av_audio[0]["codecstring"] == "ec-3", av_audio
                print(f"PASS {codec.upper()} {container} 5.1 metadata", flush=True)
        source = work / "h264.ts"
        run([ffmpeg, "-v", "error", "-y", "-f", "lavfi", "-i", "testsrc2=size=320x180:rate=10:duration=2",
             "-c:v", "libx264", "-preset", "ultrafast", "-g", "10", "-bf", "0", source])
        raw = source.read_bytes()
        assert len(raw) % 188 == 0
        m2ts = work / "h264.m2ts"
        m2ts.write_bytes(b"".join(b"\x47\x10\x00\x10" + raw[start:start + 188] for start in range(0, len(raw), 188)))
        plain, prefixed = tracks(build, "ts", source), tracks(build, "ts", m2ts)
        for metadata in (plain, prefixed):
            assert len(metadata) == 1 and metadata[0]["codec"] == "H264" and metadata[0]["width"] == 320, metadata
        for key in ("codec", "width", "height", "firstms", "lastms", "init"):
            assert plain[0][key] == prefixed[0][key], (key, plain, prefixed)
        print("PASS M2TS packet framing and complete index", flush=True)
        range_requests = []
        class RangeServer(http.server.BaseHTTPRequestHandler):
            def log_message(self, *_):
                pass
            def do_HEAD(self):
                self.serve(False)
            def do_GET(self):
                self.serve(True)
            def serve(self, body):
                first, last = 0, len(m2ts.read_bytes()) - 1
                request = self.headers.get("Range")
                if request:
                    match = re.fullmatch(r"bytes=(\d+)-(\d*)", request)
                    assert match, request
                    first = int(match[1])
                    if match[2]:
                        last = min(last, int(match[2]))
                    range_requests.append((first, last))
                data = m2ts.read_bytes()
                self.send_response(206 if request else 200)
                self.send_header("Content-Length", str(last - first + 1))
                self.send_header("Accept-Ranges", "bytes")
                if request:
                    self.send_header("Content-Range", f"bytes {first}-{last}/{len(data)}")
                self.end_headers()
                if body:
                    try:
                        self.wfile.write(data[first:last + 1])
                    except (BrokenPipeError, ConnectionResetError):
                        pass
        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), RangeServer)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            metadata = json.loads(run([build / "test" / "uploadmetaprobe_ts", f"http://127.0.0.1:{server.server_port}/source.m2ts"]))
            assert metadata["video_seek_time"] >= plain[0]["firstms"] + 900, metadata
            assert range_requests, "M2TS seek made no range request"
        finally:
            server.shutdown()
            server.server_close()
            thread.join()
        print("PASS HTTP M2TS indexes and seeks over byte ranges", flush=True)

        text = work / "tx3g.srt"
        text.write_text("1\n00:00:00,200 --> 00:00:01,700\nhello\n\n2\n00:00:01,800 --> 00:00:02,500\nlast cue\n")
        for mode in ("progressive", "moof", "absolute"):
            fragmented = mode != "progressive"
            target = work / ("tx3g-" + mode + ".mp4")
            run([ffmpeg, "-v", "error", "-y", "-i", text, "-c:s", "mov_text", *(["-movflags", "+frag_keyframe+empty_moov" + ("+default_base_moof" if mode == "moof" else "")] if fragmented else []), target])
            metadata = json.loads(run([build / "test" / "uploadmetaprobe_mp4", target]))
            assert metadata["first_subtitle"]["data"] == "hello", metadata
            assert metadata["first_subtitle"]["duration"] == 1500, metadata
            assert list(metadata["tracks"].values())[0]["type"] == "meta", metadata
            assert metadata["last_subtitle"]["data"] == "last cue", metadata
            assert metadata["last_subtitle"]["duration"] == 700, metadata
            print(f"PASS MP4 tx3g cue text and duration, mode={mode}", flush=True)
        success = True
        return 0
    finally:
        if success and os.environ.get("MIST_KEEP_TEST_ARTIFACTS") != "1":
            shutil.rmtree(work)
        else:
            print(f"Upload integration artifacts: {work}", file=sys.stderr)


if __name__ == "__main__":
    status = 0
    for _iteration in range(int(os.environ.get("MIST_MEDIA_REPEAT", "1"))):
        status = main()
        if status:
            break
    sys.exit(status)
