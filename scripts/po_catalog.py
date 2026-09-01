#!/usr/bin/env python3
"""Compile and validate the checked-in JSON form of gettext PO catalogs."""

import json
import pathlib
import re
import sys


QUOTED = re.compile(r'"((?:[^"\\]|\\.)*)"')
PLACEHOLDER = re.compile(r'%(?:(?:\d+)\$)?[a-zA-Z%]')
HTML_TAG = re.compile(r'</?[A-Za-z][^>]*>')
CAPABILITY_FIELDS = 'friendly|desc|hrn|source_help|name|help|when|payload|argument|response_action'
CAPABILITY_ASSIGNMENT = re.compile(
    r'\[\s*"(?:' + CAPABILITY_FIELDS + r')"\s*\]\s*=\s*(.*?);',
    re.DOTALL,
)
CPP_STRING = re.compile(r'"((?:\\.|[^"\\])*)"')


def decode(value):
    return json.loads('"' + value + '"')


def quoted(line):
    return ''.join(decode(match.group(1)) for match in QUOTED.finditer(line))


def parse(path):
    def fresh():
        return {'msgid': None, 'plural': None, 'msgstr': {}, 'fuzzy': False}

    entry = fresh()
    target = None
    with open(path, encoding='utf-8') as source:
        for raw in source:
            line = raw.strip()
            if not line:
                if entry['msgid'] is not None:
                    yield entry
                entry = fresh()
                target = None
                continue
            if line.startswith('#'):
                if line.startswith('#,') and 'fuzzy' in line:
                    entry['fuzzy'] = True
                continue
            if line.startswith('msgid_plural'):
                entry['plural'] = quoted(line)
                target = ('plural', 0)
            elif line.startswith('msgid'):
                entry['msgid'] = quoted(line)
                target = ('msgid', 0)
            elif line.startswith('msgstr['):
                index = int(line[7:line.index(']')])
                entry['msgstr'][index] = quoted(line)
                target = ('msgstr', index)
            elif line.startswith('msgstr'):
                entry['msgstr'][0] = quoted(line)
                target = ('msgstr', 0)
            elif line.startswith('"') and target:
                value = quoted(line)
                if target[0] == 'msgid':
                    entry['msgid'] += value
                elif target[0] == 'plural':
                    entry['plural'] += value
                else:
                    entry['msgstr'][target[1]] += value
    if entry['msgid'] is not None:
        yield entry


def catalog(entries):
    result = {}
    for entry in entries:
        if not entry['msgid'] or entry['fuzzy']:
            continue
        if entry['plural'] is not None:
            values = [entry['msgstr'].get(i, '') for i in range(max(entry['msgstr'], default=-1) + 1)]
            if any(values):
                result[entry['msgid']] = values
        else:
            value = entry['msgstr'].get(0, '')
            if value:
                result[entry['msgid']] = value
    return result


def encoded_catalog(path):
    return json.dumps(catalog(parse(path)), ensure_ascii=False, separators=(',', ':'), sort_keys=True) + '\n'


def compile_catalog(source, target):
    pathlib.Path(target).write_text(encoded_catalog(source), encoding='utf-8')


def boundary_whitespace(value):
    return re.match(r'^\s*', value).group(0), re.search(r'\s*$', value).group(0)


def validate(path):
    failed = False
    for entry in parse(path):
        if not entry['msgid'] or entry['fuzzy']:
            continue
        sources = [entry['msgid']]
        if entry['plural'] is not None:
            sources.append(entry['plural'])
        for index, source in enumerate(sources):
            translated = entry['msgstr'].get(index, '')
            if not translated:
                continue
            if sorted(PLACEHOLDER.findall(source)) != sorted(PLACEHOLDER.findall(translated)):
                print('%s: placeholder mismatch for %r' % (path, source), file=sys.stderr)
                failed = True
            if HTML_TAG.findall(source) != HTML_TAG.findall(translated):
                print('%s: HTML tag mismatch for %r' % (path, source), file=sys.stderr)
                failed = True
            if boundary_whitespace(source) != boundary_whitespace(translated):
                print('%s: boundary whitespace mismatch for %r' % (path, source), file=sys.stderr)
                failed = True

    json_path = pathlib.Path(path).with_suffix('.json')
    expected = encoded_catalog(path)
    if not json_path.exists() or json_path.read_text(encoding='utf-8') != expected:
        print('%s is missing or stale; run scripts/update-translations.sh' % json_path, file=sys.stderr)
        failed = True
    return not failed


def capability_literals(path):
    with open(path, encoding='utf-8', errors='replace') as source:
        text = source.read()
    for match in CAPABILITY_ASSIGNMENT.finditer(text):
        expression = match.group(1)
        tokens = CPP_STRING.findall(expression)
        if not tokens:
            continue
        remainder = CPP_STRING.sub('', expression)
        if re.search(r'[^\s+()]', remainder):
            continue
        try:
            value = ''.join(json.loads('"' + token + '"') for token in tokens)
        except (ValueError, json.JSONDecodeError):
            continue
        if value and re.search(r'[A-Za-z]', value):
            yield value


def extract_backend(paths):
    messages = set()
    for path in paths:
        messages.update(capability_literals(path))
    print('#include <mist/tr.h>')
    print('void capability_translation_markers() {')
    for message in sorted(messages, key=str.casefold):
        print('  tr(' + json.dumps(message, ensure_ascii=False) + ');')
    print('}')


def main():
    if len(sys.argv) < 2:
        raise SystemExit('usage: po_catalog.py compile INPUT.po OUTPUT.json | check CATALOG.po [...]')
    command = sys.argv[1]
    if command == 'compile' and len(sys.argv) == 4:
        compile_catalog(sys.argv[2], sys.argv[3])
        return 0
    if command == 'check' and len(sys.argv) > 2:
        results = [validate(path) for path in sys.argv[2:]]
        return 0 if all(results) else 1
    if command == 'extract-backend' and len(sys.argv) > 2:
        extract_backend(sys.argv[2:])
        return 0
    raise SystemExit('invalid po_catalog.py arguments')


if __name__ == '__main__':
    raise SystemExit(main())
