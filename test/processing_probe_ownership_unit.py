#!/usr/bin/env python3
from pathlib import Path
from types import SimpleNamespace
import tempfile
import unittest

from processing_probe_ownership import assert_single_probe_source


class ProbeOwnershipTest(unittest.TestCase):
    def check_history(self, history, accepted):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'controller.log').write_text(history)
            (root / 'mist.log').write_text(history)
            fixture = SimpleNamespace(root=root, stream='probe')
            if accepted:
                assert_single_probe_source(fixture, [42])
            else:
                with self.assertRaises(AssertionError):
                    assert_single_probe_source(fixture, [42])

    def test_input_that_exited_before_the_snapshot_is_still_a_duplicate(self):
        self.check_history('MistInMP4:probe (41) INFO: Input started\n'
                           'MistInMP4:probe (41) INFO: Input closing cleanly\n'
                           'MistInMP4:probe (42) INFO: Input started\n', False)

    def test_one_source_survives_mirrored_log_entries(self):
        self.check_history('MistInMP4:probe (42) INFO: Input started\n', True)

    def test_other_streams_and_the_admitted_encoder_are_not_source_boots(self):
        self.check_history('MistInMP4:other (41) INFO: Input started\n'
                           'MistProcAV:probe (43) INFO: Input started\n'
                           'MistInMP4:probe (42) INFO: Input started\n', True)

    def test_pipe_formatted_source_boots_are_also_historical(self):
        self.check_history('INFO|MistInEBML:probe|41|source.cpp:1||Input started\n'
                           'INFO|MistInEBML:probe|42|source.cpp:1||Input started\n', False)

    def test_missing_boot_history_cannot_prove_single_ownership(self):
        self.check_history('', False)


if __name__ == '__main__':
    unittest.main()
