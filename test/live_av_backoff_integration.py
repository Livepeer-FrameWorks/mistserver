#!/usr/bin/env python3
"""Check actual InputBuffer restart spacing with a failing private AV processor."""
from pathlib import Path
import json
import os
import shutil
import subprocess
import sys
import time

if os.environ.get("MIST_RUN_MEDIA_TESTS") != "1":
    print("set MIST_RUN_MEDIA_TESTS=1 to run live AV restart supervision")
    sys.exit(77)

build = Path(sys.argv[1]).resolve()
helper_root = Path(__file__).resolve().parent
if not (helper_root / 'controller_capacity_lifecycle_integration.py').exists():
    helper_root = build.parent / 'test'
sys.path.insert(0, str(helper_root))
from controller_capacity_lifecycle_integration import Fixture, free_port, until

fixture = Fixture(build / 'MistController', sys.argv[2])
success = False
try:
    private = fixture.root / 'bin'
    private.mkdir()
    (private / "lib").symlink_to(build / "lib", target_is_directory=True)
    (private / "subprojects").symlink_to(build / "subprojects", target_is_directory=True)
    for executable in build.iterdir():
        if executable.is_file() and executable.name.startswith('Mist'):
            try:
                os.link(executable, private / executable.name)
            except OSError:
                shutil.copy2(executable, private / executable.name)
    for name in ('MistController', 'MistInBuffer'):
        (private / name).unlink()
        source = Path(os.environ.get('MIST_AV_BACKOFF_BUFFER', str(build / name))) if name == 'MistInBuffer' else build / name
        shutil.copy2(source, private / name)
    stable = os.environ.get("MIST_AV_BACKOFF_STABLE") == "1"
    marker = fixture.root / 'av-starts.jsonl'
    (private / 'MistProcAV').unlink()
    fake = private / 'MistProcAV'
    fake.write_text('#!' + sys.executable + '\nimport os,sys,time,json\n'
                    'if "-j" in sys.argv or "--describe-outputs" in sys.argv: os.execv(' + repr(str(build / 'MistProcAV')) + ',sys.argv)\n'
                    'with open(' + repr(str(marker)) + ',"a") as log: log.write(json.dumps(time.monotonic())+"\\n")\n'
                    + ('if sum(1 for _ in open(' + repr(str(marker)) + ')) == 4: time.sleep(61)\n' if stable else '')
                    + 'sys.exit(1)\n')
    fake.chmod(0o755)
    fixture.controller = str(private / 'MistController')
    rtmp = free_port()
    fixture.start(streams_override={fixture.stream: {'source': 'push://', 'processes': [
        {'process': 'AV', 'codec': 'H264', 'track_select': 'video=H264&audio=none', 'resolution': '160x90'}]}},
        extra_protocols=[{'connector': 'RTMP', 'interface': '127.0.0.1', 'port': rtmp}])
    publisher = subprocess.Popen([sys.argv[2], '-v', 'error', '-re', '-f', 'lavfi', '-i',
        'testsrc2=size=320x180:rate=10', '-t', '110' if stable else '35', '-c:v', 'libx264', '-threads', '1', '-preset',
        'ultrafast', '-tune', 'zerolatency', '-g', '10', '-an', '-f', 'flv',
        'rtmp://127.0.0.1:%d/live/%s' % (rtmp, fixture.stream)], env=fixture.env,
        stdout=fixture.log, stderr=subprocess.STDOUT)
    fixture.children.append(publisher)
    def starts():
        return [float(line) for line in marker.read_text().splitlines()] if marker.exists() else []
    until(lambda: len(starts()) >= 4, 30, 'live AV did not restart four times')
    times = starts()[:4]
    gaps = [times[i+1] - times[i] for i in range(3)]
    assert times[-1] - times[0] >= 6.5, ('live AV respawned without exponential backoff', gaps)
    assert gaps[-1] >= gaps[0] + 1.5, ('live AV backoff did not grow', gaps)
    print('PASS actual live AV restart gaps', gaps, flush=True)
    if stable:
        until(lambda: len(starts()) >= 5, 85, 'live AV did not restart after a stable run')
        stable_gap = starts()[4] - starts()[3]
        assert 61 <= stable_gap <= 65, ('stable AV run retained accumulated failure backoff', stable_gap)
        print('PASS stable AV run reset restart delay', stable_gap, flush=True)
    success = True
finally:
    if fixture.process and fixture.group is None:
        fixture.group = fixture.process.pid
    fixture.close(failed=not success)
