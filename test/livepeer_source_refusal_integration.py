#!/usr/bin/env python3
"""Exercise startup refusals before the real processor contacts a gateway."""
import http.server
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading

from fixture_ipc import release_page

fixture, processor = sys.argv[1:]
requests = []
class API(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        requests.append(self.path)
        self.send_response(200)
        self.end_headers()
        self.wfile.write(b'[]')
    def log_message(self, *_):
        pass

with http.server.ThreadingHTTPServer(('127.0.0.1', 0), API) as server:
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        for kind in ('vod', 'live'):
            for codec, rotation, mirror, expected in (
                    ('H264', 90, 0, 'SOURCE_ROTATED'),
                    ('H264', 0, 1, 'SOURCE_ROTATED'),
                    ('MPEG2', 0, 0, 'SOURCE_CODEC_UNSUPPORTED'),
                    ('MPEG4', 0, 0, 'SOURCE_CODEC_UNSUPPORTED'),
                    ('VP9', 0, 0, 'SOURCE_CODEC_UNSUPPORTED'),
                    ('H264', 0, 0, 'FORMAT_SPECIFIC'),
                    ('H264', 360, 0, 'FORMAT_SPECIFIC'),
                    ('H264+RAW', 0, 0, 'FORMAT_SPECIFIC')):
                with tempfile.TemporaryDirectory(prefix='mist-source-refusal-') as directory:
                    env = dict(os.environ, TMP=directory, MIST_CONTROL='1')
                    stream = 'lprefusal' + str(os.getpid())
                    owner = subprocess.Popen([fixture, stream, codec, str(rotation), str(mirror), kind],
                                             stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                             text=True, env=env)
                    child = None
                    try:
                        assert owner.stdout.readline().strip() == 'ready', 'fixture metadata was not created'
                        requests.clear()
                        config = {'source': stream, 'custom_url': 'http://127.0.0.1:' + str(server.server_port),
                                  'target_profiles': [{'name': 'small', 'height': 90, 'bitrate': 100000}]}
                        child = subprocess.Popen([processor, json.dumps(config)], stdout=subprocess.PIPE,
                                                 stderr=subprocess.PIPE, text=True, env=env)
                        stdout, stderr = child.communicate(timeout=20)
                        reason = subprocess.check_output([fixture, 'reason', str(child.pid)], env=env, text=True).strip()
                        remote = expected == 'FORMAT_SPECIFIC'
                        assert reason == expected, (kind, codec, rotation, mirror, reason, expected, stderr)
                        assert bool(requests) == remote, ('unexpected gateway contact', kind, codec, rotation, mirror, requests)
                        assert child.returncode == 2, (child.returncode, stderr)
                        print('PASS', kind, codec, rotation, mirror, reason, 'gateway_requests=' + str(len(requests)), flush=True)
                    finally:
                        if child:
                            if child.poll() is None:
                                child.kill()
                                child.wait()
                            release_page('/MstProcState_' + str(child.pid), env)
                        owner.communicate('\n', timeout=5)
    finally:
        server.shutdown()
        thread.join()
