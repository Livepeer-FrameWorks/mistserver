#!/usr/bin/env python3
"""Declaration probes must not publish supervisor-owned runtime state."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from fixture_ipc import page_exists, release_page


class DeclarationIPC(unittest.TestCase):
    def test_ffmpeg_declarations_do_not_create_heartbeat_pages(self):
        binary = Path(sys.argv[1]).resolve()
        with tempfile.TemporaryDirectory(prefix="mist-declaration-ipc-") as directory:
            env = dict(os.environ, TMP=directory, TMPDIR=directory, TEMP=directory)
            for codec in ("H264", "AAC"):
                with self.subTest(codec=codec):
                    child = subprocess.Popen(
                        [str(binary), "--describe-outputs",
                         json.dumps({"source": "declaration-ipc-fixture", "codec": codec})],
                        env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                    page = "/MstProcState_" + str(child.pid)
                    try:
                        output, errors = child.communicate(timeout=10)
                        self.assertEqual(child.returncode, 0, errors.decode(errors="replace"))
                        declaration = json.loads(output)
                        self.assertEqual(declaration["outputs"][0]["codec"], codec)
                        self.assertFalse(page_exists(page, env),
                                         "declaration probe retained runtime state: " + page)
                    finally:
                        if child.poll() is None:
                            child.kill()
                            child.wait()
                        release_page(page, env)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
