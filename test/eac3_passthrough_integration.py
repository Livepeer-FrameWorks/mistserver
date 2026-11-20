#!/usr/bin/env python3
"""Exercise canonical EAC-3 input through real MP4 and HLS-TS writers."""
import json
import os
from pathlib import Path
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import urllib.parse
import urllib.request
from process_av_rotation_integration import run, wait_for

if os.environ.get('MIST_RUN_MEDIA_TESTS')!='1':sys.exit(77)
build,ffmpeg,ffprobe,mode=Path(sys.argv[1]).resolve(),sys.argv[2],sys.argv[3],sys.argv[4]
work=Path(tempfile.mkdtemp(prefix='mist-eac3-passthrough-'))
env=dict(os.environ,TMP=str(work/'ipc'),MIST_CONTROL='1',UDP_API='udp://127.0.0.1:0',PATH=str(build)+os.pathsep+os.environ['PATH'])
(work/'ipc').mkdir()
controller=None
success=False
try:
    suffix='mp4' if mode=='mp4' else 'ts'
    audio_codec='ac3' if mode.endswith('-ac3') else 'aac' if mode.endswith('-aac') else 'eac3'
    source=work/('source.'+suffix)
    run([ffmpeg,'-v','error','-y','-f','lavfi','-i','testsrc2=size=320x180:rate=10','-f','lavfi','-i','anullsrc=channel_layout=5.1:sample_rate=48000','-t','12','-c:v','libx264','-threads','2','-preset','ultrafast','-g','10','-bf','0','-c:a',audio_codec,'-b:a','192k',source])
    with socket.socket() as api, socket.socket() as http:
        api.bind(('127.0.0.1',0));http.bind(('127.0.0.1',0))
        api_port,http_port=api.getsockname()[1],http.getsockname()[1]
    stream='eacpass'+str(os.getpid())
    config={'config':{'debug':4,'device_discovery':False,'protocols':[{'connector':'HTTP','interface':'127.0.0.1','port':http_port},{'connector':'HLS'},{'connector':'MP4'},{'connector':'EBML'}],'controller':{'interface':'127.0.0.1','port':api_port,'username':None},'sessionInputMode':15,'sessionOutputMode':15,'sessionStreamInfoMode':1,'sessionUnspecifiedMode':0,'sessionViewerMode':14,'tknMode':15,'triggers':{}},'streams':{stream:{'name':stream,'source':str(source)}}}
    (work/'config.json').write_text(json.dumps(config))
    log=work/'controller.log'
    controller=subprocess.Popen([str(build/'MistController'),'-c',str(work/'config.json'),'-C','r','-L',str(log)],env=env)
    wait_for(lambda:log.exists() and 'Controller started' in log.read_text(errors='replace'))
    if mode=='mp4':
        output=work/'recording.mp4'
        with (work/'output.log').open('wb') as outlog:
            run([build/'MistOutMP4','-s',stream,str(output)+'?video=all&audio=all&subtitle=none&meta=none'],env=env,stdout=outlog,stderr=subprocess.STDOUT,timeout=50)
    elif mode.startswith('progressive-') or mode.startswith('mkv-'):
        extension='mp4' if mode.startswith('progressive-') else 'mkv'
        output=work/('playback.'+extension)
        def request_ready():
            try:
                with urllib.request.urlopen(f'http://127.0.0.1:{http_port}/{stream}.{extension}',timeout=20) as response:
                    output.write_bytes(response.read())
                return True
            except urllib.error.URLError:return False
        wait_for(request_ready,deadline=35)
    else:
        url=f'http://127.0.0.1:{http_port}/hls/{stream}/index.m3u8'
        def fetch(url):
            with urllib.request.urlopen(url,timeout=20) as response:return response.read()
        manifests={}
        def playlist_ready():
            try:
                body=fetch(url).decode()
                if '#EXTM3U' not in body:return False
                manifests['body']=body
                return True
            except (OSError,urllib.error.URLError):return False
        wait_for(playlist_ready,deadline=35)
        body=manifests['body']
        for _ in range(3):
            links=[line.strip() for line in body.splitlines() if line.strip() and not line.startswith('#')]
            assert links,body
            target=urllib.parse.urljoin(url,links[0])
            data=fetch(target)
            if data.startswith(b'#EXTM3U'):
                url,body=target,data.decode();continue
            output=work/'segment.ts';output.write_bytes(data);break
        else:raise AssertionError('no HLS media segment')
    info=json.loads(run([ffprobe,'-v','error','-show_streams','-of','json',output],stdout=subprocess.PIPE).stdout)
    audio=[track for track in info['streams'] if track['codec_type']=='audio']
    assert len(audio)==1 and audio[0]['codec_name']==audio_codec and audio[0]['channels']==6,info
    if mode=='mp4':assert audio[0]['codec_tag_string']=='ec-3',audio
    decoded=subprocess.run([str(v) for v in [ffmpeg,'-v','error','-xerror','-i',output,'-map','0:v:0','-map','0:a:0','-f','null','-']],capture_output=True,text=True,timeout=40)
    assert decoded.returncode==0 and not decoded.stderr.strip(), 'container payload is truncated or corrupt: '+decoded.stderr
    if mode.startswith(('progressive-', 'mkv-')):
        def packets(path):
            return json.loads(run([ffprobe,'-v','error','-count_frames','-show_entries','stream=codec_type,nb_read_frames','-of','json',path],stdout=subprocess.PIPE).stdout)['streams']
        expected={row['codec_type']:int(row['nb_read_frames']) for row in packets(source)}
        actual={row['codec_type']:int(row['nb_read_frames']) for row in packets(output)}
        assert actual==expected, 'container lost decoded frames: '+repr((actual,expected))
    kind='mp4' if mode=='mp4' else 'ts'
    metadata=json.loads(run([build/'test'/('uploadmetaprobe_'+kind),source],stdout=subprocess.PIPE).stdout)
    codecs=[track['codec'] for track in metadata['tracks'].values() if track['type']=='audio']
    assert codecs==[{'eac3':'EAC-3','ac3':'AC3','aac':'AAC'}[audio_codec]],metadata
    success=True
    print(f'PASS {mode}: {audio_codec} source -> decodable 5.1 output with complete frames',flush=True)
finally:
    if controller:
        subprocess.run([str(build/'MistUtilNuke'),stream],env=env,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,timeout=10)
        if controller.poll() is None:
            controller.send_signal(signal.SIGINT)
            try:controller.wait(timeout=10)
            except subprocess.TimeoutExpired:controller.kill();controller.wait()
    if success and os.environ.get('MIST_KEEP_TEST_ARTIFACTS')!='1':shutil.rmtree(work)
    else:print('EAC-3 fixture artifacts: '+str(work),file=sys.stderr)
