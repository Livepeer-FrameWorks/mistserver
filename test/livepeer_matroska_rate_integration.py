#!/usr/bin/env python3
"""Livepeer segments of a B-frame Matroska file keep the source frame cadence from the first frame.

The gateway reports the r_frame_rate FFmpeg infers from each segment as the source
rate, and the transcode is refused when it disagrees with the rate Mist requests.
"""
from pathlib import Path
import http.server
import json
import subprocess
import sys
import threading
from controller_capacity_lifecycle_integration import Fixture, until


def main():
    build, ffmpeg, ffprobe, fps = Path(sys.argv[1]).resolve(), sys.argv[2], sys.argv[3], int(sys.argv[4])
    f = Fixture(build / 'MistController', ffmpeg)
    f.env['UDP_API'] = 'udp://127.0.0.1:0'
    segments = []

    class Gateway(http.server.BaseHTTPRequestHandler):
        def do_POST(self):
            media = self.rfile.read(int(self.headers['Content-Length']))
            if self.path.endswith('/session'):
                self.send_response(202)
                self.send_header('Content-Length', '0')
                self.end_headers()
                return
            if not any(path == self.path for path, _ in segments):
                segments.append((self.path, media))
            result = subprocess.run([ffmpeg, '-v', 'error', '-copyts', '-i', 'pipe:0', '-map', '0:v:0', '-an',
                                     '-c:v', 'libx264', '-threads', '1', '-preset', 'ultrafast', '-s', '320x180',
                                     '-mpegts_copyts', '1', '-muxdelay', '0', '-f', 'mpegts', 'pipe:1'],
                                    input=media, env=f.tool_env, capture_output=True, timeout=20)
            if result.returncode:
                self.send_error(500)
                return
            body = b'--rate-check\r\nContent-Type: video/mp2t\r\nRendition-Name: audit\r\n\r\n' + result.stdout + b'\r\n--rate-check--\r\n'
            self.send_response(200)
            self.send_header('Content-Type', 'multipart/mixed; boundary=rate-check')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, *_):
            pass

    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Gateway)
    worker = threading.Thread(target=server.serve_forever, daemon=True)
    worker.start()
    success = False
    try:
        mp4, source, recording = f.root / 'source.mp4', f.root / 'source.mkv', f.root / 'roundtrip.mkv'
        # The upload fixture the platform stack uses: x264 B-frames, AAC, remuxed to Matroska without DTS.
        subprocess.run([ffmpeg, '-v', 'error', '-y', '-f', 'lavfi', '-i', 'testsrc2=size=640x360:rate=%d' % fps,
                        '-f', 'lavfi', '-i', 'sine=frequency=440:sample_rate=48000', '-t', '8', '-c:v', 'libx264',
                        '-preset', 'veryfast', '-g', str(2 * fps), '-pix_fmt', 'yuv420p', '-c:a', 'aac', str(mp4)],
                       env=f.tool_env, check=True, timeout=30)
        subprocess.run([ffmpeg, '-v', 'error', '-y', '-i', str(mp4), '-c', 'copy', str(source)],
                       env=f.tool_env, check=True, timeout=30)
        f.start(streams_override={f.stream: {'source': str(source), 'process_controlled_realtime': True,
                'realtime_speed': 4, 'processes': [{'process': 'Livepeer', 'x-LSP-kind': 'video',
                'hardcoded_broadcasters': 'http://127.0.0.1:%d' % server.server_port,
                'target_profiles': [{'name': 'audit', 'bitrate': 300000, 'width': 320, 'height': 180, 'fps': 0}],
                'source_mask': 7, 'target_mask': 3, 'workload': 'vod', 'deadline_ms': 20000, 'restart_type': 'disabled'}]}})
        input_log = (f.root / 'input.log').open('wb')
        canonical = subprocess.Popen([str(build / 'MistInEBML'), '--debug', '4', '-r', '-s', f.stream, str(source)],
                                     env=f.env, stdout=input_log, stderr=subprocess.STDOUT)
        f.children.append(canonical)
        until(lambda: (f.root / 'input.log').exists() and 'Input started' in (f.root / 'input.log').read_text(errors='replace'),
              15, 'canonical real-time file input did not start')
        with (f.root / 'recording.log').open('wb') as log:
            output = subprocess.Popen([str(build / 'MistOutEBML'), '-s', f.stream,
                str(recording) + '?video=all&audio=none&subtitle=none&meta=none&waittrackcount=2&maxwaittrackms=30000'],
                env=f.env, stdout=log, stderr=subprocess.STDOUT)
            f.children.append(output)
            assert output.wait(timeout=65) == 0, 'Livepeer round-trip recording failed'
        assert len(segments) >= 3, ('too few Livepeer segments', [path for path, _ in segments])
        frame_ticks = 90000 / fps
        for index, (path, media) in enumerate(segments):
            segment = f.root / ('segment%d.ts' % index)
            segment.write_bytes(media)
            probe = json.loads(subprocess.check_output([ffprobe, '-v', 'error', '-select_streams', 'v:0', '-show_streams',
                                                        '-show_packets', '-of', 'json', str(segment)],
                                                       env=f.tool_env, text=True, timeout=30))
            decode = [int(p['dts']) for p in probe['packets']]
            if len(decode) < fps:
                continue
            steps = [after - before for before, after in zip(decode, decode[1:])]
            uneven = [(i, step) for i, step in enumerate(steps) if abs(step - frame_ticks) > 90]
            assert not uneven, ('segment decode clock left the %d fps cadence' % fps, path, uneven[:6], decode[:8])
            rate = probe['streams'][0]['r_frame_rate']
            assert rate == '%d/1' % fps, ('gateway probe infers another source rate', path, rate, decode[:8])
        success = True
        print('PASS Livepeer %d fps Matroska segments: decode clock keeps the frame cadence and probes as %d/1 across %d segments'
              % (fps, fps, len(segments)), flush=True)
    finally:
        if not success:
            for name in ('input.log', 'recording.log'):
                if (f.root / name).exists():
                    print((f.root / name).read_text(errors='replace')[-12000:], file=sys.stderr)
        try:
            f.close(failed=not success)
        finally:
            server.shutdown()
            server.server_close()
            worker.join()


if __name__ == '__main__':
    main()
