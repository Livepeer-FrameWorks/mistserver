#!/usr/bin/env python3
"""Formatting may edit changed lines but cannot cross an untouched boundary."""
import importlib.util
from pathlib import Path
import sys

spec = importlib.util.spec_from_file_location('changed_format', sys.argv[1])
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
source = '// é\nlegacy( )\n{\nreturn   4 ;\n}\n'.encode()
start = source.index(b'return')
changes = [(source.index(b'( )'), 3, b'()'),
           (source.index(b'\n{'), 2, b' {'),
           (start + 6, 3, b' '),
           (source.index(b' ;'), 1, b'')]
actual = module.scoped_replacements(source, changes, {4})
assert actual == '// é\nlegacy( )\n{\nreturn 4;\n}\n'.encode(), actual
assert module.scoped_replacements(b'a\nb\n', [(1, 1, b' ')], {1}) == b'a\nb\n'
assert module.scoped_replacements(b'a\nb\n', [(1, 1, b' ')], {1, 2}) == b'a b\n'
assert module.scoped_replacements(b'int a=1;\n', [(5, 0, b' '), (6, 0, b' ')], {1}) == b'int a = 1;\n'
print('PASS changed-line formatting preserves UTF8 bytes and untouched line boundaries')
