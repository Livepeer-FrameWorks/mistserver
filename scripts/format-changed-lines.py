#!/usr/bin/env python3
"""Apply clang-format replacements only inside lines with semantic Git changes."""
import argparse
import bisect
import difflib
from pathlib import Path
import re
import subprocess
import sys
import xml.etree.ElementTree as ET


def scoped_replacements(source, replacements, changed):
    starts = [0] + [match.end() for match in re.finditer(b'\n', source)]
    accepted = []
    for offset, length, value in replacements:
        first = bisect.bisect_right(starts, offset)
        end = offset + max(0, length - 1)
        if b'\n' in source[offset:offset + length]:
            # Joining a newline also changes the following original line.
            end = offset + length
        last = bisect.bisect_right(starts, end)
        if all(line in changed for line in range(first, last + 1)):
            accepted.append((offset, length, value))
    for offset, length, value in reversed(accepted):
        source = source[:offset] + value + source[offset + length:]
    return source


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', required=True)
    parser.add_argument('--staged', action='store_true')
    parser.add_argument('--apply', action='store_true')
    parser.add_argument('base', nargs='?')
    parser.add_argument('target', nargs='?', default='HEAD')
    options = parser.parse_args()
    if options.apply and not options.staged:
        parser.error('--apply requires --staged')
    if not options.staged and not options.base:
        parser.error('provide a base commit or --staged')
    root = Path(subprocess.check_output(['git', 'rev-parse', '--show-toplevel'], text=True).strip())
    def git(*arguments):
        return subprocess.check_output(['git', *arguments], cwd=root)
    revision = ['--cached'] if options.staged else [options.base, options.target]
    names = git('diff', '--name-only', '-z', '--diff-filter=ACMR', *revision).decode().split('\0')
    failed = False
    for name in names:
        if (not name.endswith(('.cpp', '.h', '.hpp')) or name.endswith(('.js.h', '.css.h')) or
                name.startswith(('lsp/', 'embed/', 'subprojects/', 'lib/ndi/'))):
            continue
        diff = git('diff', '--no-ext-diff', '--no-color', '--ignore-all-space', '--ignore-blank-lines',
                   '--unified=0', *revision, '--', name).decode()
        changed = set()
        ranges = []
        for start, count in re.findall(r'^@@ -\d+(?:,\d+)? \+(\d+)(?:,(\d+))? @@', diff, re.M):
            start, count = int(start), int(count or 1)
            if count:
                ranges.append((start, start + count - 1))
                changed.update(range(start, start + count))
        if not changed:
            continue
        source = git('show', (':' if options.staged else options.target + ':') + name)
        command = [options.binary, '--output-replacements-xml', '--assume-filename=' + str(root / name)]
        command.extend('--lines=%d:%d' % limits for limits in ranges)
        formatted = subprocess.run(command, input=source, capture_output=True, check=True)
        document = ET.fromstring(formatted.stdout)
        if document.get('incomplete_format') == 'true':
            raise RuntimeError('clang-format could not parse ' + name)
        replacements = [(int(item.attrib['offset']), int(item.attrib['length']), (item.text or '').encode())
                        for item in document.findall('replacement')]
        result = scoped_replacements(source, replacements, changed)
        if result == source:
            continue
        if options.apply:
            path = root / name
            if path.read_bytes() != source:
                raise RuntimeError('preserving unstaged edits: stage ' + name + ' before formatting it')
            path.write_bytes(result)
            print('Formatted semantic changes in ' + name)
        else:
            failed = True
            print(''.join(difflib.unified_diff(source.decode().splitlines(True), result.decode().splitlines(True),
                                             fromfile='a/' + name, tofile='b/' + name)), end='')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
