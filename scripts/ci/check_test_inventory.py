#!/usr/bin/env python3
"""Fail CI when required compiled regressions disappear from Meson's inventory."""
import json
from pathlib import Path
import shutil
import subprocess
import sys

build = Path(sys.argv[1])
for program in ('ffmpeg', 'ffprobe'):
    assert shutil.which(program), 'required media fixture executable is missing: ' + program
inventory = json.loads(subprocess.check_output(['meson', 'introspect', str(build), '--tests'], text=True))
names = {row['name'] for row in inventory}
modes = {'bandwidth', 'progressive-mp4', 'fragmented-mp4', 'fragmented-mp4-moof', 'record-mp4-no-b', 'record-mp4-av', 'record-mkv',
         'webrtc-idle', 'webrtc-reload', 'whep-idle-timer'}
if sys.platform.startswith('linux'):
    modes.add('whep')
required = {'Controller capacity lifecycle ' + mode for mode in modes}
required.update({
    'Upload containers preserve video codecs, surround audio and subtitle durations',
    'All eight orientations produce upright ladder and thumbnail pixels',
    'AAC channel layouts survive Opus processing at full duration',
    'Closed WHEP HTTP setup preserves the media inactivity reason',
    'Input buffer producer EOF lifecycle',
    'Processor output declarations leave no runtime heartbeat pages',
    'CPU 4K downscale retains fine detail above 40 dB',
    'Admitted 4K software encode retains threads lookahead and mbtree',
    'Canonical EAC-3 survives actual mp4 passthrough',
    'Canonical EAC-3 survives actual hls passthrough',
    'Live subtitles start without producer key flags',
    'Live zerolatency ignores admitted VOD lookahead',
    'Hardware decoder device and session failures remain retryable',
    'Premature MP4 sample order exhaustion fails loudly',
    'DTSC header length matches valid and partless track selection',
    'AAC mono stays mono through Opus processing',
    'TS container playback progressive-ac3',
    'TS container playback progressive-eac3',
    'TS container playback mkv-aac',
    'TS container playback mkv-ac3',
    'TS container playback mkv-eac3',
})
if sys.platform.startswith('linux'):
    required.add('WebRTC signal handler owns the post-dup2 listener descriptor')
missing = sorted(required - names)
assert not missing, 'required regression tests are absent: ' + ', '.join(missing)
output = build / 'meson-logs' / 'ingest-capacity-test-inventory.json'
output.write_text(json.dumps(inventory, indent=2) + '\n')
print('CI inventory: %d total tests, %d controller capacity modes, all %d required regressions present' %
      (len(inventory), len(modes), len(required)))

# A preloaded clock shim must run before libc; ASan requires its runtime first.
exceptions = {'A session that boots across a second boundary stays up':
              'LD_PRELOAD clock interposition conflicts with ASan runtime ordering; covered by the normal Linux job.'}
if '--results' in sys.argv:
    results = [json.loads(line) for line in (build / 'meson-logs' / 'testlog.json').read_text().splitlines() if line.strip()]
    # Meson result names include the project and suite before the configured test name.
    skipped = [row['name'] for row in results if row.get('result') == 'SKIP' and
               any(row['name'].endswith(name) for name in required)]
    assert not skipped, 'required media regressions silently skipped: ' + ', '.join(skipped)
    missing_results = [name for name in required if not any(row['name'].endswith(name) for row in results)]
    assert not missing_results, 'required regressions have no result: ' + ', '.join(missing_results)
    print('Required regression results contain no skipped or missing tests')
(build / 'meson-logs' / 'sanitizer-exceptions.json').write_text(json.dumps(exceptions, indent=2) + '\n')
