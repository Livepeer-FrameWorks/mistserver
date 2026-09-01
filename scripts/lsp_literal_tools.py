#!/usr/bin/env python3
"""Small JavaScript literal scanner used by the MI localization tooling."""

import ast
import argparse
import json
import os
import pathlib
import re
import sys
import warnings


HUMAN = re.compile(r'[A-Za-z][A-Za-z]')
DIRECT_PREFIXES = (
    re.compile(r'\.(?:textContent|innerHTML|title|placeholder)\s*=\s*$'),
    re.compile(r'\.(?:text|html|append|prepend)\(\s*$'),
    re.compile(r'(?:window\.)?(?:confirm|alert)\(\s*$'),
    re.compile(r'new\s+Option\(\s*$'),
    re.compile(r'\.setAttribute\(\s*[\'\"](?:title|aria-label|placeholder)[\'\"]\s*,\s*$'),
)
DESCRIPTOR_PREFIX = re.compile(
    r'\b(?:label|title|subtitle|help|text|desc|description|placeholder|intro|emptyMessage|'
    r'addLabel|advancedLabel|advancedLabelExpanded|searchPlaceholder|quickLabel|emptyLabel|'
    r'noMatchLabel|cancelLabel|confirmLabel|loadingText|promoText|disabledTitle)\s*:\s*$'
)
ALREADY_TRANSLATED = re.compile(r'\b(?:tr|trn|msg)\(\s*$')
PRESENTATION_ASSIGNMENT = re.compile(r'\.(?:textContent|innerHTML|title|placeholder)\s*=')
TRANSLATED_CALL_ARGUMENTS = {
    'backButton': {0},
    'headerButton': {1, 2},
    'infoRow': {0},
    'lineDataset': {0},
    'registerTab': {0},
    'releaseAction': {2},
    'setActionState': {2},
    'statCard': {2},
}


def decode_js_string(raw):
    # JavaScript's ordinary quoted strings overlap with Python string syntax for
    # the escape forms used in the MI sources. Unlike unicode_escape, this keeps
    # already-decoded non-ASCII source text intact.
    try:
        with warnings.catch_warnings():
            warnings.simplefilter('ignore', SyntaxWarning)
            return ast.literal_eval(raw)
    except (SyntaxError, ValueError):
        try:
            return json.loads(raw)
        except json.JSONDecodeError:
            return raw[1:-1]


def scan_strings(source):
    """Yield (start, end, raw, decoded) for non-template JS string literals."""
    index = 0
    length = len(source)
    previous_significant = ''
    while index < length:
        char = source[index]
        if char.isspace():
            index += 1
            continue
        if source.startswith('//', index):
            newline = source.find('\n', index + 2)
            index = length if newline < 0 else newline + 1
            continue
        if source.startswith('/*', index):
            end = source.find('*/', index + 2)
            index = length if end < 0 else end + 2
            continue
        if char == '`':
            index += 1
            while index < length:
                if source[index] == '\\':
                    index += 2
                elif source[index] == '`':
                    index += 1
                    break
                else:
                    index += 1
            previous_significant = '`'
            continue
        if char == '/' and previous_significant in ('', '=', '(', '[', '{', ',', ':', ';', '!', '?'):
            index += 1
            in_class = False
            while index < length:
                if source[index] == '\\':
                    index += 2
                elif source[index] == '[':
                    in_class = True
                    index += 1
                elif source[index] == ']':
                    in_class = False
                    index += 1
                elif source[index] == '/' and not in_class:
                    index += 1
                    while index < length and source[index].isalpha():
                        index += 1
                    break
                else:
                    index += 1
            previous_significant = '/'
            continue
        if char in ('\'', '"'):
            start = index
            quote = char
            index += 1
            while index < length:
                if source[index] == '\\':
                    index += 2
                elif source[index] == quote:
                    index += 1
                    break
                else:
                    index += 1
            raw = source[start:index]
            yield start, index, raw, decode_js_string(raw)
            previous_significant = quote
            continue
        previous_significant = char
        index += 1


def visible_text(value):
    without_markup = re.sub(r'<[^>]*>', '', value)
    return bool(HUMAN.search(without_markup))


_CONTEXT_SOURCE = None
_CALL_CONTEXTS = {}


def _build_call_contexts(source):
    """Map quoted-string offsets to their innermost containing call.

    Strings and comments are skipped so a nested call such as
    ``el('time', format.dateTime(value, 'long'))`` is attributed to dateTime,
    not el. This deliberately handles only the structural detail needed by the
    localization audit rather than attempting to parse all of JavaScript.
    """
    stack = []
    contexts = {}
    index = 0
    length = len(source)
    previous_significant = ''
    while index < length:
        char = source[index]
        if char.isspace():
            index += 1
            continue
        if source.startswith('//', index):
            newline = source.find('\n', index + 2)
            index = length if newline < 0 else newline + 1
            continue
        if source.startswith('/*', index):
            end = source.find('*/', index + 2)
            index = length if end < 0 else end + 2
            continue
        if char in ('\'', '"', '`'):
            quote = char
            if quote != '`':
                contexts[index] = tuple(stack[-1]) if stack else (None, 0)
            index += 1
            while index < length:
                if source[index] == '\\':
                    index += 2
                elif source[index] == quote:
                    index += 1
                    break
                else:
                    index += 1
            previous_significant = quote
            continue
        if char == '/' and previous_significant in ('', '=', '(', '[', '{', ',', ':', ';', '!', '?'):
            index += 1
            in_class = False
            while index < length:
                if source[index] == '\\':
                    index += 2
                elif source[index] == '[':
                    in_class = True
                    index += 1
                elif source[index] == ']':
                    in_class = False
                    index += 1
                elif source[index] == '/' and not in_class:
                    index += 1
                    while index < length and source[index].isalpha():
                        index += 1
                    break
                else:
                    index += 1
            previous_significant = '/'
            continue
        if char == '(':
            prefix = source[max(0, index - 100):index]
            match = re.search(r'([A-Za-z_$][\w$]*(?:\.[A-Za-z_$][\w$]*)*)\s*$', prefix)
            stack.append([match.group(1) if match else None, 0])
        elif char == ')' and stack:
            stack.pop()
        elif char == ',' and stack:
            stack[-1][1] += 1
        previous_significant = char
        index += 1
    return contexts


def enclosing_call_name(source, offset):
    global _CONTEXT_SOURCE, _CALL_CONTEXTS
    if source is not _CONTEXT_SOURCE:
        _CONTEXT_SOURCE = source
        _CALL_CONTEXTS = _build_call_contexts(source)
    return _CALL_CONTEXTS.get(offset, (None, 0))


def literal_role(source, start, end, value):
    if not visible_text(value):
        return None
    if value in {'default', 'string', 'true'} or 'data-dependent-' in value or '{ display:' in value:
        return None
    before = source[max(0, start - 700):start]
    after = source[end:min(len(source), end + 80)]
    call_name, argument_index = enclosing_call_name(source, start)
    if ALREADY_TRANSLATED.search(before):
        return None
    if value.startswith(('[', ']')) and re.search(r'\.innerHTML\s*=\s*$', before):
        return None
    if any(pattern.search(before) for pattern in DIRECT_PREFIXES):
        return 'direct'
    statement_start = max(
        source.rfind(';', max(0, start - 1200), start),
        source.rfind('{', max(0, start - 1200), start),
        source.rfind('}', max(0, start - 1200), start),
    )
    if (PRESENTATION_ASSIGNMENT.search(source[statement_start + 1:start])
            and call_name is None
            and not re.match(r'\s+in\b', after)
            and not re.fullmatch(r'&[A-Za-z]+;', value)):
        return 'direct'
    if DESCRIPTOR_PREFIX.search(before):
        return 'descriptor'
    short_name = call_name.rsplit('.', 1)[-1] if call_name else None
    if argument_index in TRANSLATED_CALL_ARGUMENTS.get(short_name, set()):
        return 'descriptor'
    if call_name == 'el' and argument_index >= 1 and re.match(r'\s*\)', after):
        return 'direct'
    return None


ROOT = pathlib.Path(__file__).resolve().parents[1]
MODULES = ROOT / 'lsp' / 'modules'
EXCLUDED_PARTS = {'efg', 'playground', 'brands'}


def module_files():
    for path in sorted(MODULES.rglob('*.js')):
        if not EXCLUDED_PARTS.intersection(path.relative_to(MODULES).parts):
            yield path


def i18n_import(path):
    target = MODULES / 'core' / 'i18n.js'
    relative = os.path.relpath(target, path.parent).replace(os.sep, '/')
    if not relative.startswith('.'):
        relative = './' + relative
    return "import { tr } from '%s';\n" % relative


def has_tr_binding(source):
    return bool(re.search(r'import\s*\{[^}]*\btr\b[^}]*\}\s*from\s*[\'\"][^\'\"]*i18n\.js[\'\"]', source))


def direct_candidates(source):
    return [
        (start, end, raw, value)
        for start, end, raw, value in scan_strings(source)
        if literal_role(source, start, end, value) == 'direct'
    ]


def extract_markers():
    messages = set()
    for path in module_files():
        source = path.read_text(encoding='utf-8')
        for start, end, raw, value in scan_strings(source):
            if literal_role(source, start, end, value):
                messages.add(value)
    print('function localizationMarkers() {')
    for message in sorted(messages, key=str.casefold):
        print('  msg(' + json.dumps(message, ensure_ascii=False) + ');')
    print('}')


def audit_or_write(write):
    remaining = []
    changed = 0
    for path in module_files():
        source = path.read_text(encoding='utf-8')
        found = direct_candidates(source)
        if not found:
            continue
        if not write:
            for start, _, _, value in found:
                line = source.count('\n', 0, start) + 1
                remaining.append('%s:%d: %s' % (path.relative_to(ROOT), line, value[:100]))
            continue
        for start, end, raw, _ in reversed(found):
            source = source[:start] + 'tr(' + raw + ')' + source[end:]
        if not has_tr_binding(source):
            source = i18n_import(path) + source
        path.write_text(source, encoding='utf-8')
        changed += 1
    if write:
        print('Localized %d MI modules.' % changed)
        return 0
    if remaining:
        print('\n'.join(remaining))
        return 1
    print('No unlocalized static MI DOM literals found.')
    return 0


def main():
    parser = argparse.ArgumentParser()
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--write', action='store_true')
    mode.add_argument('--extract', action='store_true')
    args = parser.parse_args()
    if args.extract:
        extract_markers()
        return 0
    return audit_or_write(args.write)


if __name__ == '__main__':
    raise SystemExit(main())
