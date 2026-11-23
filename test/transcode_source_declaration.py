#!/usr/bin/env python3
"""Transcoders declare the original track they read and the renditions an original retains.

A ladder transcodes from the highest-bitrate original continuous-video track: never from a
rendition a process made and never from a cover-art or thumbnail image. MistProcLivepeer and
MistProcAV read their maxbps source selection that way, and MistProcLivepeer declares each
profile's track_retain and target bitrate so the buffer's processing graph can leave out the
profiles an original track already satisfies.
"""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

TRANSCODE_SOURCE = "source,!jpeg,!png,|maxbps"
RETAIN_720 = "video=h264,hevc,|source,|>0x683,|<0x757,|>2197kbps,|<8790kbps"


def describe(binary, config):
    with tempfile.TemporaryDirectory(prefix="mist-transcode-source-") as directory:
        env = {"TMP": directory, "TMPDIR": directory, "TEMP": directory, "PATH": "/usr/bin:/bin"}
        result = subprocess.run([str(binary), "--describe-outputs", json.dumps(config)], env=env,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=10)
        if result.returncode != 0:
            raise AssertionError(result.stderr.decode(errors="replace"))
        return json.loads(result.stdout)


class TranscodeSourceDeclaration(unittest.TestCase):
    livepeer = None
    av = None

    def test_livepeer_reads_the_original_maxbps_track(self):
        profile = {"name": "720p", "height": 720, "bitrate": 4500000, "track_inhibit": "video=<0x720",
                   "track_retain": RETAIN_720}
        for source in (None, "maxbps"):
            config = {"source": "declaration-fixture", "target_profiles": [profile]}
            if source:
                config["source_track"] = source
            with self.subTest(source_track=source):
                declaration = describe(self.livepeer, config)
                self.assertEqual(declaration["select"]["target"]["video"], TRANSCODE_SOURCE)
                output = declaration["outputs"][0]
                self.assertEqual(output["track_retain"], RETAIN_720)
                self.assertEqual(output["track_retain_bitrate"], 4500000)
                self.assertEqual(output["track_inhibit"], "video=<0x720")

    def test_livepeer_keeps_an_explicit_source_track(self):
        config = {"source": "declaration-fixture", "source_track": "1", "target_profiles": [{"name": "360p"}]}
        declaration = describe(self.livepeer, config)
        self.assertEqual(declaration["select"]["target"]["video"], "1")
        self.assertNotIn("track_retain", declaration["outputs"][0])

    def test_av_reads_the_original_maxbps_track(self):
        config = {"source": "declaration-fixture", "codec": "H264", "resolution": "x720", "bitrate": 4500000,
                  "track_select": "video=maxbps&audio=none&subtitle=none&meta=none", "track_retain": RETAIN_720}
        declaration = describe(self.av, config)
        self.assertEqual(declaration["select"]["target"]["video"], TRANSCODE_SOURCE + ",|first")
        config["track_select"] = "video=H264&audio=none"
        declaration = describe(self.av, config)
        self.assertEqual(declaration["select"]["target"]["video"], "H264,|first")


if __name__ == "__main__":
    TranscodeSourceDeclaration.livepeer = Path(sys.argv[1]).resolve()
    TranscodeSourceDeclaration.av = Path(sys.argv[2]).resolve()
    unittest.main(argv=[sys.argv[0]])
