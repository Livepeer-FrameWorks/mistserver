#!/usr/bin/env python3
"""A real indexed source with only corrupt video packets must stop its decoder."""
import json
import os
from pathlib import Path
import subprocess
import sys
import time
from controller_capacity_lifecycle_integration import Fixture

if os.environ.get('MIST_RUN_MEDIA_TESTS') != '1':
    raise SystemExit(77)
build, ffmpeg, ffprobe = Path(sys.argv[1]).resolve(), sys.argv[2], sys.argv[3]
f = Fixture(build / 'MistController', ffmpeg)
success = False
f.env['UDP_API'] = 'udp://127.0.0.1:0'
try:
    source = f.root / 'corrupt.mp4'
    subprocess.run([ffmpeg,'-v','error','-y','-f','lavfi','-i','testsrc2=size=320x180:rate=30:duration=12','-c:v','libx264','-threads','1','-preset','ultrafast','-g','30','-bf','0','-movflags','+faststart',str(source)],check=True,timeout=30)
    packets = json.loads(subprocess.check_output([ffprobe,'-v','error','-select_streams','v','-show_packets','-show_entries','packet=pos,size','-of','json',str(source)],text=True))['packets']
    assert len(packets) >= 300, len(packets)
    data = bytearray(source.read_bytes())
    for packet in packets:
        offset,size = int(packet['pos']),int(packet['size'])
        data[offset:offset+size] = bytes(size)
    source.write_bytes(data)
    policy = [{'process':'AV','codec':'H264','accel':'sw','resolution':'x90','track_select':'video=H264&audio=none&meta=none&subtitle=none','restart_type':'disabled','decode_threads':1,'encode_threads':1}]
    f.start(streams_override={f.stream:{'source':str(source),'process_controlled_realtime':True,'processes':policy}})
    log = (f.root / 'input.log').open('w')
    child = subprocess.Popen([str(build/'MistInMP4'),'-r','-s',f.stream,str(source)],env=f.env,stdout=log,stderr=subprocess.STDOUT)
    f.children.append(child)
    until = time.monotonic()+15
    text = ''
    while time.monotonic() < until:
        text = '\n'.join(p.read_text(errors='replace') for p in f.root.glob('*.log'))
        if 'Decoder rejected 128 consecutive packets without a decoded frame' in text:
            success = True
            print('PASS corrupt source decoder stops after 128 invalid packets instead of stalling',flush=True)
            break
        time.sleep(.05)
    assert success, 'decoder never reported bounded invalid-data failure:\n'+text[-7000:]
finally:
    f.close(failed=not success)
