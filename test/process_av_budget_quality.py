#!/usr/bin/env python3
"""Run the real software encoder with an admitted multi-thread quality profile."""
import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from process_av_rotation_integration import run, wait_for
from test_ports import free_port

if os.environ.get('MIST_RUN_MEDIA_TESTS') != '1':
    sys.exit(77)
build, ffmpeg, ffprobe = Path(sys.argv[1]).resolve(), sys.argv[2], sys.argv[3]
work = Path(tempfile.mkdtemp(prefix='mist-budget-quality-'))
env = dict(os.environ, TMP=str(work/'ipc'), UDP_API='udp://127.0.0.1:0', MIST_CONTROL='1')
(work/'ipc').mkdir()
controller = source_process = None
success = False
try:
    source = work/'source.mp4'
    run([ffmpeg,'-v','error','-y','-f','lavfi','-i','testsrc2=size=3840x2160:rate=12','-t','2','-c:v','libx264','-threads','2','-preset','ultrafast','-g','12','-bf','0','-pix_fmt','yuv420p',source])
    port = free_port()
    stream = 'budgetquality'+str(os.getpid())
    processor = {'process':'AV','codec':'H264','accel':'sw','bitrate':8000000,'gopsize':12,'preset':'faster','source_mask':4,'target_mask':3,'track_select':'video=H264&audio=none&subtitle=none&meta=none','restart_type':'disabled','decode_threads':4,'encode_threads':8,'lookahead':20}
    zero_latency = os.environ.get('MIST_ENCODER_ZERO_LATENCY') == '1'
    if zero_latency: processor['tune'] = 'zerolatency'
    config = {'config':{'debug':4,'device_discovery':False,'protocols':[],'controller':{'interface':'127.0.0.1','port':port,'username':None},'sessionInputMode':15,'sessionOutputMode':15,'sessionStreamInfoMode':1,'sessionUnspecifiedMode':0,'sessionViewerMode':14,'tknMode':15,'triggers':{}},'streams':{stream:{'name':stream,'source':str(source),'process_controlled_realtime':True,'realtime_speed':1,'processes':[processor]}}}
    (work/'config.json').write_text(json.dumps(config))
    log = work/'controller.log'
    controller = subprocess.Popen([str(build/'MistController'),'-c',str(work/'config.json'),'-C','r','-L',str(log)],env=env)
    wait_for(lambda:log.exists() and 'Controller started' in log.read_text(errors='replace'))
    started = time.monotonic()
    with (work/'input.log').open('wb') as input_log, (work/'output.log').open('wb') as output_log:
        source_process = subprocess.Popen([str(build/'MistInMP4'),'--debug','4','-r','-s',stream,str(source)],env=env,stdout=input_log,stderr=subprocess.STDOUT)
        wait_for(lambda:'Input started' in (work/'input.log').read_text(errors='replace'))
        output = work/'output.mkv'
        run([build/'MistOutEBML','-s',stream,str(output)+'?video=H264&audio=none&subtitle=none&meta=none'],env=env,stdout=output_log,stderr=subprocess.STDOUT,timeout=90)
    elapsed = time.monotonic()-started
    encoded = run([ffmpeg,'-v','error','-i',output,'-map','0:v:0','-c:v','copy','-f','h264','pipe:1'],stdout=subprocess.PIPE).stdout
    parameters = encoded.decode('utf-8', errors='ignore')
    assert re.search(r'\bthreads=8\b',parameters), 'admitted encoder threads were overridden: '+repr(parameters[:1500])
    if zero_latency:
        assert not re.search(r'\brc_lookahead=[1-9][0-9]*\b',parameters) and re.search(r'\bmbtree=0\b',parameters), 'explicit lookahead overrode zerolatency: '+repr(parameters[:1500])
    else:
        assert re.search(r'\brc_lookahead=20\b',parameters) and re.search(r'\bmbtree=1\b',parameters), 'admitted lookahead/mb-tree was disabled: '+repr(parameters[:1500])
    info = json.loads(run([ffprobe,'-v','error','-select_streams','v','-count_frames','-show_entries','stream=nb_read_frames,width,height','-of','json',output],stdout=subprocess.PIPE).stdout)['streams'][0]
    assert info['width']==3840 and info['height']==2160 and int(info['nb_read_frames'])==24, info
    success = True
    profile = 'zerolatency, mbtree=0' if zero_latency else 'lookahead=20, mbtree=1'
    print(f'PASS 4K software H264: 8 threads, {profile}; all 24 frames, wall={elapsed:.2f}s',flush=True)
finally:
    if controller:
        subprocess.run([str(build/'MistUtilNuke'),stream],env=env,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,timeout=10)
    for process in (source_process,controller):
        if process and process.poll() is None:
            process.send_signal(signal.SIGINT)
            try:process.wait(timeout=10)
            except subprocess.TimeoutExpired:process.kill();process.wait()
    if success and os.environ.get('MIST_KEEP_TEST_ARTIFACTS')!='1':shutil.rmtree(work)
    else:print('Quality fixture artifacts: '+str(work),file=sys.stderr)
