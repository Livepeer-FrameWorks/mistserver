"""Check the original input's ownership throughout metadata admission."""
import re


def assert_single_probe_source(fixture, original):
    assert len(original) == 1, ('probe has more than one source input', original)
    boots = set()
    pattern = re.compile(r'\bMistIn(?:AV|EBML|MP4|TS):' + re.escape(fixture.stream) +
                         r'(?:\s+\((\d+)\)|\|(\d+)\|).*?\bInput started\b')
    for name in ('controller.log', 'mist.log'):
        path = fixture.root / name
        if path.exists():
            for match in pattern.finditer(path.read_text(errors='replace')):
                boots.add(int(match[1] or match[2]))
    assert boots == set(original), ('probe source boot history differs from its sole owner', boots, original)
