#!/usr/bin/env python3
"""Distinguish MPEG-4 Visual from legacy H264 in real upload containers."""
from pathlib import Path
import os
import sys
import tempfile
build, ffmpeg = Path(sys.argv[1]).resolve(), sys.argv[2]
helper = Path(__file__).resolve().parent
if not (helper / 'upload_formats_integration.py').exists(): helper = build.parent / 'test'
sys.path.insert(0, str(helper))
from upload_formats_integration import run, tracks
if os.environ.get('MIST_RUN_MEDIA_TESTS') != '1': sys.exit(77)
with tempfile.TemporaryDirectory(prefix='mist-mpeg4-containers-') as directory:
    root = Path(directory)
    failures = []
    for container, kind in [('mp4','mp4'),('mov','mp4'),('mkv','ebml'),('ts','ts'),('avi','av')]:
        path = root / ('mpeg4.' + container)
        run([ffmpeg,'-v','error','-y','-f','lavfi','-i','testsrc2=size=320x180:rate=10:duration=2',
             '-c:v','mpeg4','-threads','1','-g','10','-bf','0',path])
        try:
            video = tracks(build,kind,path)
            assert len(video)==1 and video[0]['codec']=='MPEG4', video
            assert (video[0]['width'],video[0]['height'])==(320,180) and video[0]['init'],video
            print('PASS MPEG4',container,'metadata',flush=True)
        except (AssertionError, KeyError) as error:
            failures.append(container+': '+str(error))
    source = root / 'h264.mp4'
    run([ffmpeg,'-v','error','-y','-f','lavfi','-i','testsrc2=size=320x180:rate=10:duration=2',
         '-c:v','libx264','-threads','1','-preset','ultrafast','-g','10','-bf','0',source])
    raw = source.read_bytes()
    config = raw.index(b'avcC')
    entry = raw.rfind(b'avc1',0,config)
    assert entry>=0
    legacy = root / 'legacy.mp4'
    legacy.write_bytes(raw[:entry]+b'mp4v'+raw[entry+4:])
    video = tracks(build,'mp4',legacy)
    assert video[0]['codec']=='H264' and video[0]['init'],video
    print('PASS legacy mp4v with avcC remains H264',flush=True)
    assert not failures,'MPEG4 upload mappings failed:\n'+'\n'.join(failures)
