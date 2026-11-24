#!/usr/bin/env python3
"""Run real source/TS/FFmpeg-gateway/TS/sink media with reordered frames."""
from pathlib import Path
import http.server
import json
import subprocess
import sys
import threading
from controller_capacity_lifecycle_integration import Fixture, until


def main():
    build, ffmpeg, ffprobe = Path(sys.argv[1]).resolve(), sys.argv[2], sys.argv[3]
    bframes = int(sys.argv[4])
    f = Fixture(build / 'MistController', ffmpeg)
    f.env['UDP_API'] = 'udp://127.0.0.1:0'
    uploads = []
    class Gateway(http.server.BaseHTTPRequestHandler):
        def do_POST(self):
            media = self.rfile.read(int(self.headers['Content-Length']))
            if self.path.endswith('/session'):
                self.send_response(202)
                self.send_header('Content-Length', '0')
                self.end_headers()
                return
            result = subprocess.run([ffmpeg, '-v', 'error', '-copyts', '-i', 'pipe:0', '-map', '0:v:0', '-an',
                                     '-c:v', 'libx264', '-threads', '1', '-preset', 'fast', '-bf', str(bframes),
                                     '-b_strategy', '0', '-g', '25', '-mpegts_copyts', '1', '-muxdelay', '0',
                                     '-f', 'mpegts', 'pipe:1'], input=media, env=f.tool_env, capture_output=True, timeout=20)
            uploads.append((self.path, result.returncode, result.stderr.decode(errors='replace')))
            if result.returncode:
                self.send_error(500)
                return
            body = b'--clock-check\r\nContent-Type: video/mp2t\r\nRendition-Name: audit\r\n\r\n' + result.stdout + b'\r\n--clock-check--\r\n'
            self.send_response(200)
            self.send_header('Content-Type', 'multipart/mixed; boundary=clock-check')
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
        source, recording = f.root / 'bframes.mkv', f.root / 'roundtrip.mkv'
        subprocess.run([ffmpeg, '-v', 'error', '-y', '-f', 'lavfi', '-i', 'testsrc2=size=320x180:rate=25:duration=8',
                        '-c:v', 'libx264', '-threads', '1', '-preset', 'medium', '-bf', '3', '-b_strategy', '2',
                        '-g', '25', '-sc_threshold', '0', '-an', str(source)], env=f.tool_env, check=True, timeout=30)
        f.start(streams_override={f.stream: {'source': str(source), 'process_controlled_realtime': True,
                'realtime_speed': 4, 'processes': [{'process': 'Livepeer', 'x-LSP-kind': 'video',
                'hardcoded_broadcasters': 'http://127.0.0.1:%d' % server.server_port,
                'target_profiles': [{'name': 'audit', 'bitrate': 500000, 'width': 320, 'height': 180, 'fps': 25, 'gop': '1.0'}],
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
        assert len(uploads) >= 4 and all(code == 0 for _, code, _ in uploads), ('real gateway transcodes failed', uploads)
        probe = json.loads(subprocess.check_output([ffprobe, '-v', 'error', '-show_streams', '-show_packets', '-of', 'json', str(recording)], env=f.tool_env, timeout=30))
        streams = [s for s in probe['streams'] if s['codec_type'] == 'video']
        assert len(streams) == 2, ('round trip did not publish source and rendition', streams)
        timelines = []
        for stream in streams:
            times = sorted(round(float(p['pts_time']) * 1000) for p in probe['packets'] if p['stream_index'] == stream['index'])
            assert len(times) >= 175, ('rendition lost too much media', stream['index'], len(times))
            timelines.append(times)
        source_times, returned_times = timelines
        shift = returned_times[0] - source_times[0]
        assert abs(shift) <= 2, ('Livepeer rendition presentation differs from source', bframes, shift, source_times[:5], returned_times[:5])
        assert len(returned_times) == len(source_times), ('Livepeer round trip lost frames', bframes, len(source_times), len(returned_times))
        common = len(source_times)
        assert all(abs(a - b) <= 2 for a, b in zip(source_times[:common], returned_times[:common])), ('Livepeer round trip changed its presentation clock across segments', bframes, len(source_times), len(returned_times), [(i, a, b) for i, (a, b) in enumerate(zip(source_times, returned_times)) if abs(a - b) > 2][:12])
        success = True
        print('PASS Livepeer real FFmpeg round trip: return bf=%d, source/rendition PTS agree across %d segments (%d frames)' % (bframes, len(uploads), common), flush=True)
    finally:
        if not success:
            print('Gateway transcodes:', uploads, file=sys.stderr)
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
