#!/usr/bin/env python3
"""Receive real SDP/RTP output from a B-frame Matroska file.

RTP timestamps start at an arbitrary origin; Mist sends its shared presentation timeline, which
may sit a constant lead after the file's own. Every frame must keep its exact presentation time
relative to the others, in presentation order on the 90 kHz clock.
"""
from pathlib import Path
import json
import socket
import subprocess
import sys
from controller_capacity_lifecycle_integration import Fixture


def main():
    build, ffmpeg, ffprobe = Path(sys.argv[1]).resolve(), sys.argv[2], sys.argv[3]
    f = Fixture(build / 'MistController', ffmpeg)
    f.env['UDP_API'] = 'udp://127.0.0.1:0'
    success = False
    try:
        source = f.root / 'bframes.mkv'
        subprocess.run([ffmpeg, '-v', 'error', '-y', '-f', 'lavfi', '-i', 'testsrc2=size=320x180:rate=25:duration=4',
                        '-c:v', 'libx264', '-threads', '1', '-preset', 'medium', '-bf', '3', '-g', '25', '-b_strategy', '0', '-an', str(source)], env=f.tool_env, check=True, timeout=30)
        expected = json.loads(subprocess.check_output([ffprobe, '-v', 'error', '-select_streams', 'v:0', '-show_entries', 'packet=pts_time', '-of', 'json', str(source)], env=f.tool_env))['packets']
        expected = [round(float(p['pts_time']) * 90000) & 0xffffffff for p in expected]
        f.start(streams_override={f.stream: {'source': str(source)}})
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as receiver:
            receiver.bind(('127.0.0.1', 0))
            receiver.settimeout(10)
            port = receiver.getsockname()[1]
            with (f.root / 'sdp.log').open('wb') as log:
                output = subprocess.Popen([str(build / 'MistOutSDP'), '-s', f.stream,
                    str(f.root / 'output.sdp') + '?targetIP=127.0.0.1&startPort=%d&audio=none&rate=1' % port],
                    env=f.env, stdout=log, stderr=subprocess.STDOUT)
                f.children.append(output)
                observed = []
                while len(observed) < 100:
                    data = receiver.recv(65536)
                    if len(data) >= 12 and data[1] & 0x80:
                        observed.append(int.from_bytes(data[4:8], 'big'))
                assert output.wait(timeout=10) == 0, 'SDP output failed'
        assert len(observed) == 100 and len(expected) >= 100, ('missing frames', len(observed), len(expected))
        shifts = {(o - e) & 0xffffffff for o, e in zip(observed, expected)}
        assert len(shifts) == 1, ('RTP presentation clock differs from MKV PTS', observed[:10], expected[:10])
        success = True
        print('PASS real B-frame MKV -> SDP/RTP: all 100 frame timestamps retain presentation order and 90 kHz clock', flush=True)
    finally:
        if not success and (f.root / 'sdp.log').exists():
            print((f.root / 'sdp.log').read_text(errors='replace')[-10000:], file=sys.stderr)
        f.close(failed=not success)


if __name__ == '__main__':
    main()
