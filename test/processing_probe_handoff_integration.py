#!/usr/bin/env python3
"""Admission publishes a ladder without reopening its already-probed source."""
from pathlib import Path
import http.server
import json
import os
import subprocess
import sys
import threading
import urllib.request
from controller_capacity_lifecycle_integration import Fixture, until

build, ffmpeg = Path(sys.argv[1]).resolve(), sys.argv[2]
admitted = threading.Event()
calls = []
class Trigger(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        self.rfile.read(int(self.headers.get('Content-Length', '0')))
        calls.append(admitted.is_set())
        policy = [{'process':'AV', 'codec':'H264', 'track_select':'video=none&audio=none&subtitle=none&meta=none',
                   'track_inhibit':'video=all&audio=all', 'x-LSP-probe':True}]
        if admitted.is_set():
            policy = [{'process':'AV','codec':'H264','resolution':'160x90','track_select':'video=maxbps&audio=none&subtitle=none&meta=none',
                       'source_mask':4,'target_mask':3,'restart_type':'disabled','encode_threads':1,'decode_threads':1,'accel':'sw'}]
        body = json.dumps(policy).encode()
        self.send_response(200)
        self.send_header('Content-Length',str(len(body)))
        self.end_headers()
        self.wfile.write(body)
    def log_message(self,*_): pass

f = Fixture(build / 'MistController', ffmpeg)
f.env['UDP_API']='udp://127.0.0.1:0'
success=False
with http.server.ThreadingHTTPServer(('127.0.0.1',0), Trigger) as server:
    worker=threading.Thread(target=server.serve_forever,daemon=True);worker.start()
    try:
        source=f.root/'probe.mkv'
        subprocess.run([ffmpeg,'-v','error','-y','-f','lavfi','-i','testsrc2=size=320x180:rate=10:duration=12',
                        '-c:v','libx264','-threads','1','-preset','ultrafast','-g','10','-bf','0',str(source)],check=True,timeout=30)
        f.start(streams_override={f.stream:{'source':str(source),'process_controlled_realtime':True,'realtime':True}},
                triggers_override={'STREAM_PROCESS':[{'handler':'http://127.0.0.1:%d/'%server.server_port,'sync':True,'streams':[f.stream]}]})
        with urllib.request.urlopen('http://127.0.0.1:%d/json_%s.js'%(f.http,f.stream),timeout=20) as response:
            response.read()
        until(lambda:calls,10,'metadata probe never resolved its process policy')
        def snapshot():
            active=f.request({'active_streams':{'longform':True,'fields':['health','sourcepids']}}).get('active_streams') or {}
            return active.get(f.stream,{})
        original=until(lambda:snapshot().get('sourcepids'),10,'paused source has no owner')
        assert len(original)==1,('probe has more than one source input',original)
        admitted.set()
        observed={}
        def started():
            observed.clear();observed.update(snapshot())
            return any(isinstance(track,dict) and track.get('codec')=='H264' and track.get('width')==160
                       and track.get('height')==90 and track.get('buffer',0)>0
                       for track in (observed.get('health') or {}).values())
        until(started,10,'admitted ladder never produced video on the existing probe input')
        assert set(original).issubset(observed.get('sourcepids') or []) and len(observed['sourcepids'])==2, (
            'admission replaced or duplicated the source input instead of adding its one encoder',original,observed)
        assert any(calls) and not all(calls),calls
        success=True
        print('PASS metadata probe -> admitted AV ladder with one source input and no restart',flush=True)
    finally:
        f.close(failed=not success)
        server.shutdown();worker.join()
