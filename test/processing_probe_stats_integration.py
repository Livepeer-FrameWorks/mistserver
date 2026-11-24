#!/usr/bin/env python3
"""A single boot exposes source metadata before admission, without reading media."""
from pathlib import Path
import http.server
import json
import subprocess
import sys
import threading
import time
import urllib.request
from controller_capacity_lifecycle_integration import Fixture, until

build, ffmpeg = Path(sys.argv[1]).resolve(), sys.argv[2]
admitted = threading.Event()
calls = []
class Trigger(http.server.BaseHTTPRequestHandler):
    protocol_version='HTTP/1.1'
    def do_HEAD(self):
        file=f.root/self.path.lstrip('/')
        if not file.is_file():self.send_error(404);return
        self.send_response(200);self.send_header('Content-Length',str(file.stat().st_size));self.send_header('Accept-Ranges','bytes');self.end_headers()
    def do_GET(self):
        file=f.root/self.path.lstrip('/')
        if not file.is_file():
            self.send_error(404);return
        data=file.read_bytes();start=0;end=len(data)-1
        ranged=self.headers.get('Range')
        if ranged:
            left,right=ranged.removeprefix('bytes=').split('-')
            start=int(left);end=min(end,int(right)) if right else end
        self.send_response(206 if ranged else 200)
        self.send_header('Content-Length',str(end-start+1))
        if ranged:self.send_header('Content-Range','bytes %d-%d/%d'%(start,end,len(data)))
        self.end_headers();self.wfile.write(data[start:end+1])
    def do_PUT(self):
        if self.headers.get('Transfer-Encoding','').lower()=='chunked':
            chunks=[]
            while True:
                size=int(self.rfile.readline().strip().split(b';')[0],16)
                if not size:
                    self.rfile.readline();break
                chunks.append(self.rfile.read(size));self.rfile.read(2)
            data=b''.join(chunks)
        else:data=self.rfile.read(int(self.headers.get('Content-Length','0')))
        (f.root/self.path.lstrip('/')).write_bytes(data)
        self.send_response(200);self.send_header('Content-Length','0');self.end_headers()

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
        source=f.root/'probe.mp4'
        subprocess.run([ffmpeg,'-v','error','-y','-f','lavfi','-i','testsrc2=size=320x180:rate=10:duration=12',
                        '-f','lavfi','-i','anullsrc=channel_layout=5.1:sample_rate=48000','-t','12','-c:a','aac','-c:v','libx264','-threads','1','-preset','ultrafast','-g','10','-bf','0',str(source)],env=f.tool_env,check=True,timeout=30)
        f.start(streams_override={f.stream:{'source':'http://127.0.0.1:%d/probe.mp4'%server.server_port,'process_controlled_realtime':True,'realtime':True}},
                triggers_override={'STREAM_PROCESS':[{'handler':'http://127.0.0.1:%d/'%server.server_port,'sync':True,'streams':[f.stream]}]})
        try:
            with urllib.request.urlopen('http://127.0.0.1:%d/json_%s.js?metaeverywhere=1&inclzero=1'%(f.http,f.stream),timeout=5) as response:
                metadata=json.loads(response.read())
            print('SINGLE_BOOT_METADATA_KEYS',list(metadata),flush=True)
        except (OSError,TimeoutError) as error:
            print('SINGLE_BOOT_TIMEOUT',str(error),flush=True)
        found=False
        for _sample in range(30):
            snapshot=f.request({'active_streams':{'longform':True,'fields':['tracks','status','firstms','lastms','health','sourcepids','pid']}})
            active=snapshot.get('active_streams') or {}
            data=active.get(f.stream,{})
            print('SINGLE_BOOT_ACTIVE',json.dumps(data),flush=True)
            if data.get('health'):
                found=True;break
            time.sleep(.5)
        assert found, 'single metadata boot never appears in controller health before admission'
        assert data['lastms']==0, 'probe advanced before memory admission'
        assert any(isinstance(t,dict) and t.get('codec')=='AAC' and t.get('channels')==6 and t.get('rate')==48000 for t in data['health'].values()), 'paused source lost its 5.1 audio metadata: %s'%data
        assert any(isinstance(t,dict) and t.get('codec')=='H264' and t.get('width')==320 and t.get('height')==180 for t in data['health'].values()), data
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
        def rendition_available():
            active=f.request({'active_streams':{'longform':True,'fields':['health','lastms']}}).get('active_streams') or {}
            health=active.get(f.stream,{}).get('health') or {}
            return any(isinstance(t,dict) and t.get('codec')=='H264' and t.get('width')==160 and t.get('height')==90 and t.get('buffer',0)>0 for t in health.values())
        until(rendition_available,20,'admitted source never produced its video rendition')
        success=True
        print('PASS metadata probe -> admitted AV ladder with one source input and no restart',flush=True)
    finally:
        f.close(failed=not success)
        server.shutdown();worker.join()
