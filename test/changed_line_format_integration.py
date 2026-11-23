#!/usr/bin/env python3
"""A semantic-line formatter must leave neighboring legacy code byte-identical."""
from pathlib import Path
import subprocess
import sys
import tempfile

legacy = sys.argv[1] == '--legacy'
script, binary = (None, sys.argv[2]) if legacy else (Path(sys.argv[1]).resolve(), sys.argv[2])
with tempfile.TemporaryDirectory(prefix='mist-format-scope-') as directory:
    root = Path(directory)
    def git(*args):
        return subprocess.run(['git', *args], cwd=root, check=True, capture_output=True, text=True)
    git('init', '-q', '--initial-branch=master')
    (root / '.clang-format').write_text('BasedOnStyle: LLVM\nIndentWidth: 2\nColumnLimit: 80\n')
    original = 'extern "C" {\n  #include "a.h"\n}\n\nint legacy( ){return 1;}\n\nint body( )\n{\nreturn   3 ;\n}\n'
    source = root / 'source.cpp'
    source.write_text(original)
    git('add', '.')
    git('-c', 'user.name=Formatter test', '-c', 'user.email=formatter@example.invalid', 'commit', '-qm', 'baseline')
    source.write_text(original.replace('  #include "a.h"\n', '  #include "a.h"\n  #include "b.h"\n').replace('return   3 ;', 'return   4 ;') + '\nint added( ){return 2;}\n')
    git('add', 'source.cpp')
    if legacy:
        result = subprocess.run(['git', 'clang-format', '--binary', binary, '--force'], cwd=root, capture_output=True, text=True)
        assert result.returncode in (0, 1), (result.returncode, result.stderr)
    else:
        subprocess.run([sys.executable, str(script), '--binary', binary, '--staged', '--apply'], cwd=root, check=True)
    formatted = source.read_text()
    assert '  #include "a.h"\n' in formatted, 'formatter reflowed an untouched include next to a changed line'
    assert 'int legacy( ){return 1;}\n' in formatted, 'formatter reflowed an untouched legacy function'
    assert 'int body( )\n{\n' in formatted, 'formatter reflowed an untouched function boundary around a changed body line'
    assert 'return 4;' in formatted, 'formatter did not format the changed body line'
    assert 'int added( ){return 2;}' not in formatted, 'formatter did not format the changed function'
    assert '#include "b.h"' in formatted, 'formatter removed the changed include'
    print('PASS formatting changes only semantic lines and preserves neighboring legacy bytes')
