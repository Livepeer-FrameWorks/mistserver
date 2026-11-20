#!/usr/bin/env python3
"""Prove CI refuses a skipped regression even when it remains registered."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

build = Path(sys.argv[1]).resolve()
inventory = subprocess.check_output(['meson', 'introspect', str(build), '--tests'], text=True)
rows = json.loads(inventory)
root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='mist-inventory-results-') as directory:
    fixture = Path(directory)
    (fixture / 'meson-logs').mkdir()
    (fixture / 'bin').mkdir()
    (fixture / 'inventory.json').write_text(inventory)
    fake = fixture / 'bin' / 'meson'
    fake.write_text('#!' + sys.executable + '\nfrom pathlib import Path\nprint(Path(' + repr(str(fixture / 'inventory.json')) + ').read_text())\n')
    fake.chmod(0o755)
    victim = 'Canonical EAC-3 survives actual mp4 passthrough'
    assert any(row['name'] == victim for row in rows), 'fixture regression is not registered'
    results = [{'name': row['name'], 'result': 'SKIP' if row['name'] == victim else 'OK'} for row in rows]
    (fixture / 'meson-logs' / 'testlog.json').write_text(''.join(json.dumps(row)+'\n' for row in results))
    env = dict(os.environ, PATH=str(fixture / 'bin') + os.pathsep + os.environ['PATH'])
    result = subprocess.run([sys.executable, root / 'scripts/ci/check_test_inventory.py', fixture, '--results'],
                            env=env, capture_output=True, text=True)
    assert result.returncode != 0 and 'required media regressions silently skipped' in result.stderr, 'CI accepted a required media SKIP: ' + result.stdout + result.stderr
print('PASS CI rejects a registered media regression that skipped')
