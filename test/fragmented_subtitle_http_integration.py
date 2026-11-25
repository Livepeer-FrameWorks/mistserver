#!/usr/bin/env python3
"""Read actual fragmented tx3g through the controller's HTTP subtitle output.

Cues are served on the stream's presentation timeline, which may lead the file's movie
timeline by a constant (a progressive file's audio priming edit); they must stay exactly in
step with the stream's video.
"""
from pathlib import Path
import os
import json
import struct
from decimal import Decimal
import subprocess
import sys
import urllib.request
from controller_capacity_lifecycle_integration import Fixture

def boxes(data, start=0, end=None):
    end = len(data) if end is None else end
    while start < end:
        size, kind = struct.unpack_from('>I4s', data, start)
        header = 8
        if size == 1:
            size = struct.unpack_from('>Q', data, start + 8)[0]
            header = 16
        elif size == 0:
            size = end - start
        assert size >= header and start + size <= end, (kind, start, size, end)
        yield kind, start + header, start + size
        start += size


def fragmented_cue_times(source, stream):
    """Independent ISO BMFF sample timing, including trex/tfhd defaults."""
    data = source.read_bytes()
    track = int(stream['id'], 0)
    numerator, denominator = map(int, stream['time_base'].split('/'))
    scale = Decimal(numerator) / denominator
    defaults = None
    for kind, begin, end in boxes(data):
        if kind != b'moov':
            continue
        for child, cb, ce in boxes(data, begin, end):
            if child != b'mvex':
                continue
            for entry, eb, ee in boxes(data, cb, ce):
                if entry == b'trex' and struct.unpack_from('>I', data, eb + 4)[0] == track:
                    defaults = struct.unpack_from('>II', data, eb + 12)
    assert defaults is not None, ('missing subtitle trex', track)
    result = []
    for kind, begin, end in boxes(data):
        if kind != b'moof':
            continue
        for child, cb, ce in boxes(data, begin, end):
            if child != b'traf':
                continue
            entries = list(boxes(data, cb, ce))
            tfhd = next(b for k, b, e in entries if k == b'tfhd')
            if struct.unpack_from('>I', data, tfhd + 4)[0] != track:
                continue
            flags = struct.unpack_from('>I', data, tfhd)[0] & 0xffffff
            cursor = tfhd + 8 + (8 if flags & 1 else 0) + (4 if flags & 2 else 0)
            duration, size = defaults
            if flags & 8:
                duration = struct.unpack_from('>I', data, cursor)[0]
                cursor += 4
            if flags & 16:
                size = struct.unpack_from('>I', data, cursor)[0]
            tfdt = next(b for k, b, e in entries if k == b'tfdt')
            decode = struct.unpack_from('>Q' if data[tfdt] == 1 else '>I', data, tfdt + 4)[0]
            for entry, eb, ee in entries:
                if entry != b'trun':
                    continue
                flags = struct.unpack_from('>I', data, eb)[0] & 0xffffff
                count = struct.unpack_from('>I', data, eb + 4)[0]
                cursor = eb + 8 + (4 if flags & 1 else 0) + (4 if flags & 4 else 0)
                for _ in range(count):
                    sample_duration, sample_size, composition = duration, size, 0
                    if flags & 256:
                        sample_duration = struct.unpack_from('>I', data, cursor)[0]
                        cursor += 4
                    if flags & 512:
                        sample_size = struct.unpack_from('>I', data, cursor)[0]
                        cursor += 4
                    if flags & 1024:
                        cursor += 4
                    if flags & 2048:
                        composition = struct.unpack_from('>i' if data[eb] == 1 else '>I', data, cursor)[0]
                        cursor += 4
                    if sample_size > 2:
                        result.append(((decode + composition) * scale, (decode + composition + sample_duration) * scale))
                    decode += sample_duration
                assert cursor == ee, (cursor, ee)
    assert len(result) == 2 and all(end > start for start, end in result), result
    return result


build, ffmpeg = Path(sys.argv[1]).resolve(), sys.argv[2]
ffprobe = sys.argv[3]
for mode in ('moof', 'absolute', 'progressive', 'moof-noaudio', 'absolute-noaudio'):
    fixture = Fixture(build / 'MistController', ffmpeg)
    success = False
    try:
        text = fixture.root / 'source.srt'
        text.write_text('1\n00:00:01,000 --> 00:00:03,000\n<b>hello</b>\n\n2\n00:00:04,000 --> 00:00:06,500\nlast cue\n')
        source = fixture.root / 'source.mp4'
        flags = ['-movflags', '+frag_keyframe+empty_moov' + ('+default_base_moof' if mode.startswith('moof') else '')] if mode != 'progressive' else []
        subprocess.run([ffmpeg, '-v','error','-y','-f','lavfi','-i','testsrc2=size=320x180:rate=10:duration=8','-f','lavfi','-i','sine=frequency=701:sample_rate=48000:duration=8','-i',str(text),*(['-an'] if mode.endswith('-noaudio') else ['-c:a','aac']),'-c:v','libx264','-threads','1','-preset','ultrafast','-g','25','-bf','0','-c:s','mov_text',*flags,str(source)], check=True, timeout=30)
        probe = json.loads(subprocess.check_output([ffprobe,'-v','error','-select_streams','s','-show_packets','-show_streams','-of','json',str(source)],text=True))
        packets = probe['packets']
        cues = [packet for packet in packets if int(packet['size']) > 2]
        assert len(cues) == 2, cues
        source_video = json.loads(subprocess.check_output([ffprobe,'-v','error','-select_streams','v:0','-show_entries','packet=pts_time','-of','json',str(source)],text=True))['packets']
        lead = Decimal(0)
        def timestamp(seconds):
            ms = int((Decimal(seconds) + lead) * 1000)
            return '%02d:%02d:%02d.%03d' % (ms//3600000,ms//60000%60,ms//1000%60,ms%1000)
        fixture.start(streams_override={fixture.stream:{'source':str(source)}}, extra_protocols=[{'connector':'SubRip'}])
        with urllib.request.urlopen('http://127.0.0.1:%d/json_%s.js' % (fixture.http, fixture.stream), timeout=30) as response:
            tracks = json.load(response)['meta']['tracks'].values()
        video_first = min(Decimal(track['firstms']) for track in tracks if track['type'] == 'video') / 1000
        lead = video_first - min(Decimal(packet['pts_time']) for packet in source_video)
        assert 0 <= lead < Decimal('0.1'), (mode, 'stream timeline moved by more than one priming delay', lead)
        if all('duration_time' in packet for packet in cues):
            expected_times = [timestamp(packet['pts_time']) + ' --> ' + timestamp(str(Decimal(packet['pts_time'])+Decimal(packet['duration_time']))) for packet in cues]
        else:
            # FFmpeg 6 omits fragmented mov_text durations. Read the sample
            # tables independently of Mist rather than weakening the oracle.
            expected_times = [timestamp(start) + ' --> ' + timestamp(end)
                              for start, end in fragmented_cue_times(source, probe['streams'][0])]
            print('Using independent ISO BMFF subtitle timing for', mode, flush=True)
        for extension in ('srt','vtt'):
            url = 'http://127.0.0.1:%d/%s.%s' % (fixture.http, fixture.stream, extension)
            with urllib.request.urlopen(url, timeout=30) as response:
                body = response.read().decode()
            assert all(ord(c) >= 32 or c in '\r\n\t' for c in body), (mode, extension, 'tx3g styling bytes leaked into cue text', repr(body))
            assert 'hello' in body and 'last cue' in body, (mode, extension, 'HTTP subtitles are empty or incomplete', body)
            for timing in expected_times:
                assert timing in body, (mode, extension, timing, body)
            print('PASS HTTP', mode, extension, 'both tx3g cues retain their durations', flush=True)
        success = True
    finally:
        fixture.close(failed=not success)
