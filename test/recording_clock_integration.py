#!/usr/bin/env python3
"""Accept millisecond VFR media and reject real duplicate packet timestamps."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

from recording_clock import recording_time_base


def main():
    if os.environ.get('MIST_RUN_MEDIA_TESTS') != '1':
        return 77
    ffmpeg, ffprobe = sys.argv[1:]
    with tempfile.TemporaryDirectory(prefix='mist-recording-clock-') as temporary:
        root = Path(temporary)
        valid, invalid = root / 'valid.mkv', root / 'invalid.mkv'
        subprocess.run([ffmpeg, '-v', 'error', '-y', '-filter_threads', '1',
                        '-filter_complex_threads', '1', '-f', 'lavfi', '-i',
                        'testsrc2=size=64x64:rate=10:duration=3', '-vf',
                        "settb=1/1000,setpts='if(eq(N,1),1,N*100)'", '-c:v', 'libx264',
                        '-threads', '1', '-preset', 'ultrafast', '-bf', '0',
                        '-fps_mode', 'passthrough', '-enc_time_base', '1/1000', str(valid)],
                       check=True, timeout=15)
        time_base = recording_time_base(ffprobe, valid)
        assert time_base == '1/1000', time_base
        decoded = subprocess.run([ffmpeg, '-v', 'error', '-i', str(valid), '-fps_mode',
                                  'passthrough', '-enc_time_base', time_base, '-f', 'null', '-'],
                                 capture_output=True, check=True, timeout=15)
        assert not decoded.stderr, decoded.stderr.decode(errors='replace')
        subprocess.run([ffmpeg, '-v', 'error', '-y', '-i', str(valid), '-c:v', 'copy',
                        '-bsf:v', 'setts=pts=0:dts=0', str(invalid)], check=True, timeout=15)
        try:
            recording_time_base(ffprobe, invalid)
        except ValueError as error:
            assert 'not strictly increasing' in str(error), error
        else:
            raise AssertionError('duplicate recorded timestamps were accepted')
    print('PASS millisecond VFR clock and duplicate-timestamp rejection')
    return 0


if __name__ == '__main__':
    sys.exit(main())
