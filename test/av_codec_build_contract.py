#!/usr/bin/env python3
"""Private FFmpeg mapping must not leak dependencies into the installed SDK."""
import json
from pathlib import Path
import sys
root = Path(sys.argv[1])
commands = json.loads((root / 'compile_commands.json').read_text())
compiled = [row for row in commands if Path(row['file']).name == 'av_codec_map.cpp']
assert len(compiled) == 1, 'FFmpeg codec map is compiled %d times, expected one shared private target' % len(compiled)
installed = json.loads((root / 'meson-info/intro-installed.json').read_text())
assert not any(Path(name).name == 'av_codec_map.h' for name in installed), 'private FFmpeg codec map header is installed publicly'
