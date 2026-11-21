#!/usr/bin/env python3
"""Keep sanitizer CI capable of running the registered media regressions."""
from pathlib import Path
root = Path(__file__).resolve().parents[1]
asan = (root / '.github/workflows/ci.yml').read_text().split('  asan:', 1)[1]
assert '            ffmpeg' in asan, 'ASan CI has no ffmpeg for controller/media fixtures'
assert 'MIST_RUN_MEDIA_TESTS=1 meson test -C build-asan' in asan, 'ASan CI silently skips processing regressions'
meson = (root / 'test/meson.build').read_text()
capacity = meson.split('capacity_contracts =', 1)[1].split('endforeach', 1)[0]
assert "get_option('b_sanitize')" not in capacity, 'controller lifecycle coverage is disabled under ASan'
assert "ffmpeg_program.found()" not in capacity, 'missing ffmpeg removes controller tests from the inventory'

assert 'check_test_inventory.py build-asan' in asan, 'ASan CI never verifies its lifecycle test inventory'

# Any sanitizer-specific exclusion must be listed with its concrete incompatibility.
stack = []
for line in meson.splitlines():
    value = line.strip()
    if value.startswith(('if ', 'foreach ')):
        stack.append(value)
    elif value in ('endif', 'endforeach'):
        stack.pop()
    elif value.startswith('test(') and any("get_option('b_sanitize')" in condition for condition in stack):
        raise AssertionError('ASan excludes a regression: ' + value)
