#!/usr/bin/env python3
"""The startup poll must get a numeric count before the controller creates its log."""
from pathlib import Path
import subprocess
import tempfile

source = (Path(__file__).parent / 'process_av_republish_integration.sh').read_text()
function = source.split('log_count() {', 1)[1].split('\n}', 1)[0]
with tempfile.TemporaryDirectory(prefix='mist-republish-log-') as work:
    path = Path(work) / 'controller.log'
    for contents, expected in ((None, '0'), ('', '0'), ('unrelated\n', '0'), ('Controller started\nController started\n', '2')):
        if contents is not None:
            path.write_text(contents)
        result = subprocess.run(['sh', '-c', 'work=$1\nlog_count() {' + function + '\n}\nlog_count "Controller started"', 'fixture', work], text=True, capture_output=True, check=True)
        assert result.stdout.strip() == expected and not result.stderr, (contents, result.stdout, result.stderr)
print('PASS startup polling counts missing, empty and populated logs numerically')
