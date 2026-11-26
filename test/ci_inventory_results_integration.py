#!/usr/bin/env python3
"""Prove CI refuses a skipped regression even when it remains registered."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

build = Path(sys.argv[1]).resolve()
inventory = subprocess.check_output(['meson', 'introspect', str(build), '--tests'], text=True)
rows = json.loads(inventory)
# Model the Linux inventory when this guard's fixture runs on another host.
for name in ('Controller capacity lifecycle whep',
             'Controller capacity lifecycle webrtc-idle',
             'Controller capacity lifecycle webrtc-reload',
             'Controller capacity lifecycle whep-idle-timer',
             'Detached Mist children fail the process audit',
             'Process audit reaps adopted zombies while the test runs',
             'Process audit bounds children that ignore stop signals',
             'WebRTC signal handler owns the post-dup2 listener descriptor',
             'A session that boots across a second boundary stays up'):
    if not any(row['name'] == name for row in rows):
        rows.append({'name': name})
# Focused hardware builds can omit the WebRTC connector. The synthetic Linux
# inventory still needs its script identity to exercise the result guard itself.
whip = 'WHIP offers get an answer or a logged refusal, and a passive offerer can publish'
if not any(row['name'] == whip for row in rows):
    rows.append({'name': whip, 'cmd': [str(Path(__file__).with_name('webrtc_whip_setup_integration.sh'))]})
orientation = 'H264 display orientation rotates processing and thumbnails'
if not any(row['name'] == orientation for row in rows):
    rows.append({'name': orientation, 'cmd': [str(Path(__file__).with_name('process_av_rotation_integration.py'))]})
# AV unit targets are absent from a non-AV build; model their identities for
# the full-profile results fixture without requiring their compiled binaries.
for name in (
    'AAC channel layouts survive Opus processing at full duration',
    'AAC mono stays mono through Opus processing',
    'AV codec mapping and surround channel layouts',
    'AV rotation scales display geometry and rotates planar and packed formats',
    'Actual ProcAV resampler keeps changing EAC3 block sizes current before EOF',
    'Admitted 4K software encode retains threads lookahead and mbtree',
    'All eight orientations produce upright ladder and thumbnail pixels',
    'Audio resampler detects rate and equal-count speaker layout changes',
    'CPU 4K downscale retains fine detail above 40 dB',
    'CUDA downloads preserve the actual software pixel format',
    'Default Matroska output does not select subtitles',
    'Late subtitles survive an AV ladder and thumbnail processing',
    'Live zerolatency ignores admitted VOD lookahead',
    'MP4 tx3g cue text and timing survive Matroska recording',
    'Matroska recordings retain subtitle cues while WebM excludes them',
    'Sparse subtitles declare recording headers before a three minute first cue',
    'Stored Matroska subtitles serve complete WebVTT over HTTP',
    'Upload containers preserve video codecs, surround audio and subtitle durations',
):
    if not any(row['name'] == name for row in rows):
        rows.append({'name': name})
root = Path(__file__).resolve().parents[1]
# This results fixture models a complete media profile even when the current
# build registers its AV-gated cases through the non-AV skip driver.
rows.append({'name': 'Fixture full-profile media script coverage',
             'cmd': [str(path) for path in sorted((root / 'test').iterdir())
                     if path.suffix in ('.py', '.sh')]})
inventory = json.dumps(rows)
with tempfile.TemporaryDirectory(prefix='mist-inventory-results-') as directory:
    fixture = Path(directory)
    (fixture / 'meson-logs').mkdir()
    (fixture / 'bin').mkdir()
    (fixture / 'inventory.json').write_text(inventory)
    fake = fixture / 'bin' / 'meson'
    fake.write_text('#!' + sys.executable + '\nfrom pathlib import Path\nprint(Path(' + repr(str(fixture / 'inventory.json')) + ').read_text())\n')
    fake.chmod(0o755)
    for victim in ('Canonical EAC-3 survives actual mp4 passthrough',
                   'WHIP offers get an answer or a logged refusal, and a passive offerer can publish',
                   'H264 display orientation rotates processing and thumbnails'):
        assert any(row['name'] == victim for row in rows), 'fixture regression is not registered: ' + victim
        results = [{'name': row['name'], 'result': 'SKIP' if row['name'] == victim else 'OK'} for row in rows]
        (fixture / 'meson-logs' / 'testlog.json').write_text(''.join(json.dumps(row)+'\n' for row in results))
        env = dict(os.environ, PATH=str(fixture / 'bin') + os.pathsep + os.environ['PATH'])
        linux_runner = "import sys,runpy;sys.platform='linux';sys.argv=sys.argv[1:];runpy.run_path(sys.argv[0],run_name='__main__')"
        result = subprocess.run([sys.executable, '-c', linux_runner,
                                 str(root / 'scripts/ci/check_test_inventory.py'), str(fixture), '--results'],
                                env=env, capture_output=True, text=True)
        assert result.returncode != 0 and 'required media regressions silently skipped' in result.stderr, 'CI accepted a required media SKIP for ' + victim + ': ' + result.stdout + result.stderr
print('PASS CI rejects a registered media regression that skipped')
