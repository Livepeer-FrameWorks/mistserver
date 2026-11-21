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
# Every invocation of a media-gated script must be present and produce a result,
# including modes beyond the named defect regressions above.
source = Path(__file__).resolve().parents[2]
gated_scripts = {path.name for path in (source / 'test').iterdir()
                 if path.suffix in ('.py', '.sh') and 'MIST_RUN_MEDIA_TESTS' in path.read_text(errors='replace')}
represented = set()
for row in inventory:
    scripts = {Path(arg).name for arg in row.get('cmd', [])
               if isinstance(arg, str) and arg.endswith(('.py', '.sh'))}
    gated = scripts & gated_scripts
    if gated:
        required.add(row['name'])
        represented.update(gated)
optional_scripts = set()
if not (build / 'MistProcONNX').exists():
    optional_scripts.update({'onnx_vod_recording_integration.sh', 'processing_graph_integration.sh'})
missing_scripts = sorted(gated_scripts - represented - optional_scripts)
assert not missing_scripts, 'media scripts are absent from the test inventory: ' + ', '.join(missing_scripts)
required.update({
    'AC3 header parsing does not copy remaining PES payload',
    'Matroska UTF8 subtitles retain the upstream input mapping',
    'RTMP enhanced audio advertises only codecs with packet writers',
    'Livepeer refuses transformed and unsupported sources before gateway contact',
    'Unknown private PIDs do not delay H264 AAC transport readiness',
    'Non-AV build registers media fixtures without missing dependencies',
    'Transport streams retain packet boundaries across 188 and 192 byte chunks',
    'AV codec mapping and surround channel layouts',
    'CUDA downloads preserve the actual software pixel format',
    'AV rotation scales display geometry and rotates planar and packed formats',
    'EAC-3 access units aggregate dependent substream channels',
})
if sys.platform.startswith('linux'):
    required.add('WebRTC signal handler owns the post-dup2 listener descriptor')
    required.add('A session that boots across a second boundary stays up')
    required.add('WHIP offers get an answer or a logged refusal, and a passive offerer can publish')
missing = sorted(required - names)
assert not missing, 'required regression tests are absent: ' + ', '.join(missing)
output = build / 'meson-logs' / 'ingest-capacity-test-inventory.json'
output.write_text(json.dumps(inventory, indent=2) + '\n')
print('CI inventory: %d total tests, %d controller capacity modes, all %d required regressions present' %
      (len(inventory), len(modes), len(required)))

exceptions = {
    'AV hardware orientation vt keeps all eight transforms on device':
        'Requires a macOS VideoToolbox hardware device; not registered on Linux.',
    'Hardware upload preserves pixel buffers retained by an encoder':
        'Requires a macOS VideoToolbox hardware frame pool; not registered on Linux.',
}
if '--results' in sys.argv:
    results = [json.loads(line) for line in (build / 'meson-logs' / 'testlog.json').read_text().splitlines() if line.strip()]
    # Meson result names include the project and suite before the configured test name.
    skipped = [row['name'] for row in results if row.get('result') == 'SKIP' and
               (sys.platform.startswith('linux') or any(row['name'].endswith(name) for name in required)) and
               not any(row['name'].endswith(name) for name in exceptions)]
    assert not skipped, 'required media regressions silently skipped: ' + ', '.join(skipped)
    missing_results = [name for name in required if not any(row['name'].endswith(name) for row in results)]
    assert not missing_results, 'required regressions have no result: ' + ', '.join(missing_results)
    print('Required regression results contain no skipped or missing tests')
(build / 'meson-logs' / 'sanitizer-exceptions.json').write_text(json.dumps(exceptions, indent=2) + '\n')
