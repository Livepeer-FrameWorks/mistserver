#!/usr/bin/env python3
"""A ranged progressive MP4 of a live DVR returns the requested duration promptly.

A DVR that a recording writes as a sliding HLS playlist with long segments lags the wall clock by
up to a segment: `startunix=-8` lies past its newest media, and no new media arrives for many
seconds. The MP4 must still start near the requested time, never at the DVR's beginning, and end
after the requested duration instead of streaming the whole DVR while waiting for the next segment.

Modes:
  hls   the DVR stream reads the playlist through MistInHLS.
  dtsc  a replica pulls that DVR stream over DTSC. A second range starts 2 s before the replica's
        newest media, so the replica holds less than the duration: the reply ends with the media
        that exists once the stalled source has not advanced for the live wait bound.
  live  a realtime-fed live stream whose range stop lies a few seconds ahead of the live edge, so
        the stop is reached while streaming. The reply must end cleanly and keep the connection
        usable, never switch to a recording's next file.
"""
from datetime import datetime, timezone
import http.client
import json
import os
from pathlib import Path
import subprocess
import sys
import time
import urllib.request

from controller_capacity_lifecycle_integration import Fixture, until
from test_ports import free_port

GOP = 2.0
DURATION = 4
SEGMENT = 24
LAG = 20  # seconds between the DVR's newest media and the wall clock at request time


def run(*args, **kwargs):
    return subprocess.run(list(args), check=True, timeout=120, **kwargs)


def frame_hashes(ffmpeg, env, path, frames=None):
    out = subprocess.check_output([ffmpeg, '-v', 'error', '-i', str(path), '-map', '0:v:0',
                                   *(['-frames:v', str(frames)] if frames else []), '-f', 'framemd5', '-'],
                                  env=env, text=True, timeout=120)
    return [line.rsplit(',', 1)[1].strip() for line in out.splitlines() if line and not line.startswith('#')]


def media_duration(ffprobe, env, path):
    probe = json.loads(subprocess.check_output([ffprobe, '-v', 'error', '-show_entries', 'format=duration',
                                                '-of', 'json', str(path)], env=env, text=True, timeout=30))
    return float(probe['format']['duration'])


def fetch(conn, path, deadline):
    """GETs path on a keep-alive connection; returns (body, seconds, complete)."""
    started = time.monotonic()
    conn.timeout = deadline
    if conn.sock:
        conn.sock.settimeout(deadline)
    try:
        conn.request('GET', path)
        response = conn.getresponse()
    except (OSError, http.client.HTTPException) as error:
        return b'', time.monotonic() - started, repr(error)
    assert response.status == 200, ('ranged MP4 request failed', path, response.status)
    body = bytearray()
    try:
        while time.monotonic() - started < deadline:
            chunk = response.read1(65536)
            if not chunk:
                return bytes(body), time.monotonic() - started, True
            body += chunk
    except (OSError, http.client.HTTPException) as error:
        return bytes(body), time.monotonic() - started, repr(error)
    return bytes(body), time.monotonic() - started, 'still streaming'


def main():
    if os.environ.get('MIST_RUN_MEDIA_TESTS') != '1':
        return 77
    build, ffmpeg, ffprobe, mode = Path(sys.argv[1]).resolve(), sys.argv[2], sys.argv[3], sys.argv[4]
    f = Fixture(build / 'MistController', ffmpeg)
    f.env['UDP_API'] = 'udp://127.0.0.1:0'
    success = False
    try:
        total = 3 * SEGMENT
        source = f.root / 'source.mkv'
        run(ffmpeg, '-v', 'error', '-y', '-f', 'lavfi', '-i', 'testsrc2=size=160x90:rate=10:duration=%d' % total,
            '-f', 'lavfi', '-i', 'sine=frequency=440:sample_rate=48000:duration=%d' % total,
            '-c:v', 'libx264', '-threads', '1', '-preset', 'ultrafast', '-g', '20', '-keyint_min', '20',
            '-sc_threshold', '0', '-bf', '0', '-c:a', 'aac', str(source), env=f.tool_env)
        hashes = frame_hashes(ffmpeg, f.tool_env, source)
        dvr = 'dvr' + str(os.getpid())
        streams, protocols = {}, [{'connector': 'MP4'}]
        if mode == 'live':
            streams[dvr] = {'source': str(source)}
        else:
            segments = f.root / 'segments'
            segments.mkdir()
            run(ffmpeg, '-v', 'error', '-y', '-i', str(source), '-c', 'copy', '-f', 'segment',
                '-segment_time', str(SEGMENT), '-segment_format', 'mpegts', str(segments / 'seg%d.ts'), env=f.tool_env)
            # The newest segment ended LAG seconds ago, as for a DVR that closes 24 s segments.
            begin = time.time() - LAG - total
            lines = ['#EXTM3U', '#EXT-X-VERSION:3', '#EXT-X-TARGETDURATION:%d' % (SEGMENT + 1), '#EXT-X-MEDIA-SEQUENCE:0']
            for index in range(3):
                stamp = datetime.fromtimestamp(begin + index * SEGMENT, timezone.utc).isoformat(timespec='milliseconds')
                lines += ['#EXT-X-PROGRAM-DATE-TIME:' + stamp.replace('+00:00', 'Z'), '#EXTINF:%.3f,' % SEGMENT,
                          'segments/seg%d.ts' % index]
            (f.root / 'dvr.m3u8').write_text('\n'.join(lines) + '\n')
            streams[dvr] = {'source': str(f.root / 'dvr.m3u8')}
        target = dvr
        if mode == 'dtsc':
            dtsc = free_port()
            target = 'replica' + str(os.getpid())
            streams[target] = {'source': 'dtsc://127.0.0.1:%d/%s' % (dtsc, dvr)}
            protocols.append({'connector': 'DTSC', 'interface': '127.0.0.1', 'port': dtsc})
        f.start(streams_override=streams, extra_protocols=protocols)
        f.streams.update(streams)

        def lastms(stream=dvr):
            info = f.request({'active_streams': {'longform': True, 'fields': ['status', 'lastms']}})
            return ((info.get('active_streams') or {}).get(stream) or {}).get('lastms', 0)

        query = '?startunix=-8&duration=%d' % DURATION
        if mode != 'live':
            # A viewer asks an already running DVR: boot it and let it load its playlist first.
            with urllib.request.urlopen('http://127.0.0.1:%d/json_%s.js' % (f.http, dvr), timeout=30) as response:
                response.read()
            until(lambda: lastms() >= (total - 1) * 1000, 60, 'the DVR did not load its playlist')
        else:
            feeder_log = (f.root / 'feeder.log').open('wb')
            f.children.append(subprocess.Popen([str(build / 'MistInEBML'), '--debug', '4', '-r', '-s', dvr, str(source)],
                                               env=f.env, stdout=feeder_log, stderr=subprocess.STDOUT))

            until(lambda: lastms() > 12000, 60, 'the realtime live stream did not buffer 12 s')
            # The range starts 1 s back and stops 3 s past the live edge: only streaming reaches the stop.
            live_start = lastms() - 1000
            query = '?start=%d&duration=%d' % (live_start, DURATION)

        conn = http.client.HTTPConnection('127.0.0.1', f.http)
        body, seconds, complete = fetch(conn, '/%s.mp4%s' % (target, query), 30)
        output = f.root / 'range.mp4'
        output.write_bytes(body)
        length = media_duration(ffprobe, f.tool_env, output) if body else 0.0
        assert complete is True and seconds < 20, ('ranged MP4 did not end promptly', mode, complete, '%.1f s' % seconds,
                                           '%d bytes' % len(body), '%.3f s of media' % length)
        assert DURATION - 0.1 <= length <= DURATION + GOP + 0.1, ('ranged MP4 length is not the requested duration',
                                                                  mode, length)
        first = frame_hashes(ffmpeg, f.tool_env, output, 1)
        assert first and first[0] in hashes, ('ranged MP4 starts on a frame the source does not have', mode)
        start = hashes.index(first[0]) / 10.0
        if mode == 'live':
            assert live_start / 1000.0 - GOP - 0.2 <= start <= live_start / 1000.0, ('live range start', live_start, start)
            # The connection serves its next request after the range ended at its stop.
            again, _, again_complete = fetch(conn, '/%s.mp4%s' % (target, query), 30)
            assert again_complete is True and again, ('connection did not serve a second ranged request', again_complete, len(again))
            log = (f.root / 'mist.log').read_text(errors='replace')
            assert 'Failed to open file' not in log, 'ranged MP4 took the recording restart path at its stop'
        else:
            # The newest media ends at the source end; the range covers its latest DURATION seconds.
            assert total - DURATION - GOP - 0.2 <= start <= total - DURATION, ('ranged MP4 start', mode, start)
        conn.close()
        if mode == 'dtsc':
            edge = lastms(target)
            short_start = edge - 2000
            short, short_seconds, short_complete = fetch(http.client.HTTPConnection('127.0.0.1', f.http),
                                                         '/%s.mp4?start=%d&duration=%d' % (target, short_start, DURATION), 30)
            output.write_bytes(short)
            short_length = media_duration(ffprobe, f.tool_env, output) if short else 0.0
            assert short_complete is True and short_seconds < 20, (
                'ranged MP4 stopping past the replica media did not end promptly', short_complete,
                '%.1f s' % short_seconds, '%d bytes' % len(short), '%.3f s of media' % short_length)
            first = frame_hashes(ffmpeg, f.tool_env, output, 1)
            assert first and first[0] in hashes, 'short range starts on a frame the source does not have'
            short_first = hashes.index(first[0]) / 10.0
            assert short_start / 1000.0 - GOP - 0.2 <= short_first <= short_start / 1000.0, ('short range start', short_start, short_first)
            assert edge / 1000.0 - short_first - 0.3 <= short_length <= DURATION + GOP + 0.1, (
                'short range does not hold the replica media from its start', edge, short_first, short_length)
            print('PASS dtsc range past the replica media: %.3f s from source time %.1f s in %.1f s' %
                  (short_length, short_first, short_seconds), flush=True)
        success = True
        print('PASS %s range: %.3f s of media from source time %.1f s in %.1f s' % (mode, length, start, seconds), flush=True)
        return 0
    finally:
        f.close(failed=not success)


if __name__ == '__main__':
    sys.exit(main())
