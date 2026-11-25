#!/usr/bin/env python3
"""Validate the packet clock of single-video recordings without frame reordering."""
from fractions import Fraction
import json
import subprocess
import sys


def recording_time_base(ffprobe, recording):
    probe = subprocess.run([ffprobe, '-v', 'error', '-select_streams', 'v:0',
                            '-show_entries', 'stream=time_base,has_b_frames:packet=pts,dts',
                            '-of', 'json', recording], text=True, capture_output=True, check=True)
    data = json.loads(probe.stdout)
    streams, packets = data.get('streams', []), data.get('packets', [])
    if len(streams) != 1 or not packets:
        raise ValueError('recording has no single-video packet clock')
    if streams[0].get('has_b_frames', 0):
        raise ValueError('this clock check requires video without frame reordering')
    time_base = Fraction(streams[0]['time_base'])
    if time_base <= 0:
        raise ValueError('recording has an invalid time base')
    for field in ('pts', 'dts'):
        previous = None
        for index, packet in enumerate(packets):
            if field not in packet:
                raise ValueError('missing %s on packet %d' % (field, index))
            value = int(packet[field])
            if value < 0 or (previous is not None and value <= previous):
                raise ValueError('%s is not strictly increasing at packet %d: %s -> %d' %
                                 (field, index, previous, value))
            previous = value
    return str(time_base)


if __name__ == '__main__':
    try:
        if len(sys.argv) != 3:
            raise ValueError('usage: recording_clock.py ffprobe recording')
        print(recording_time_base(sys.argv[1], sys.argv[2]))
    except (ValueError, KeyError, subprocess.CalledProcessError) as error:
        print('invalid recording clock: ' + str(error), file=sys.stderr)
        sys.exit(1)
