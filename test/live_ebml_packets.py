#!/usr/bin/env python3
"""Check every packet in a bounded live Matroska recording against its source."""
import json
import subprocess
import sys
from decimal import Decimal


def check_packets(source, recording, duration=Decimal("3")):
    counts = []
    for index in (0, 1):
        reference = [row for row in source if row["stream_index"] == index]
        actual = [row for row in recording if row["stream_index"] == index]
        assert reference and actual, f"track {index} is missing"
        assert actual[0]["data_hash"] == reference[0]["data_hash"], f"track {index} lost its first packet"
        # Live ingest rebases timestamps; AAC priming and B-frame reorder mean
        # the first PTS and packet count need not match across FFmpeg versions.
        shift = Decimal(actual[0]["pts_time"]) - Decimal(reference[0]["pts_time"])
        expected = [row for row in reference
                    if Decimal(row.get("dts_time", row["pts_time"])) + shift < duration]
        assert [row["data_hash"] for row in actual] == [row["data_hash"] for row in expected], (
            f"track {index} packets differ: expected {len(expected)}, recorded {len(actual)}")
        for original, recorded in zip(expected, actual):
            assert Decimal(recorded["pts_time"]) == Decimal(original["pts_time"]) + shift, (
                f"track {index} changed a packet presentation timestamp")
        counts.append(len(actual))
    return counts


def probe(ffprobe, path):
    result = subprocess.run([ffprobe, "-v", "error", "-show_packets", "-show_data_hash", "sha256",
                             "-show_entries", "packet=stream_index,pts_time,dts_time,data_hash",
                             "-of", "json", path], check=True, capture_output=True, text=True)
    return json.loads(result.stdout)["packets"]


if __name__ == "__main__":
    counts = check_packets(probe(sys.argv[1], sys.argv[2]), probe(sys.argv[1], sys.argv[3]))
    print(f"live EBML recording finalized all {counts[0]} video and {counts[1]} audio packets without diagnostics")
