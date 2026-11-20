#!/usr/bin/env python3
"""Keep the recording oracle strict when decode and presentation times differ."""
import copy
import unittest
from live_ebml_packets import check_packets


def fixture():
    source = []
    for track, timestamps in ((0, ("0", "0.12", "0.04", "3.12")), (1, ("-0.021", "0", "0.021", "2.987"))):
        for index, pts in enumerate(timestamps):
            row = {"stream_index": track, "pts_time": pts, "data_hash": f"{track}-{index}"}
            if track == 1:
                row["dts_time"] = pts
            elif index >= 2:
                row["dts_time"] = ("0", "2.96")[index - 2]
            source.append(row)
    recording = copy.deepcopy(source[:-1])
    for row in recording:
        if row["stream_index"] == 1:
            from decimal import Decimal
            row["pts_time"] = str(Decimal(row["pts_time"]) + Decimal("0.021"))
    return source, recording


class PacketOracleTest(unittest.TestCase):
    def test_decode_cutoff_and_audio_priming(self):
        self.assertEqual(check_packets(*fixture()), [4, 3])

    def test_missing_tail(self):
        source, recording = fixture()
        recording.pop()
        with self.assertRaisesRegex(AssertionError, "packets differ"):
            check_packets(source, recording)

    def test_changed_payload(self):
        source, recording = fixture()
        recording[2]["data_hash"] = "corrupt"
        with self.assertRaisesRegex(AssertionError, "packets differ"):
            check_packets(source, recording)

    def test_changed_timestamp(self):
        source, recording = fixture()
        recording[2]["pts_time"] = "0.05"
        with self.assertRaisesRegex(AssertionError, "presentation timestamp"):
            check_packets(source, recording)

    def test_extra_tail(self):
        source, recording = fixture()
        recording.append(copy.deepcopy(source[-1]))
        with self.assertRaisesRegex(AssertionError, "packets differ"):
            check_packets(source, recording)


if __name__ == "__main__":
    unittest.main()
