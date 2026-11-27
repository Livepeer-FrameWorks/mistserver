#!/usr/bin/env python3
"""Ending the session of an idle keep-alive HLS connection only closes it.

A pooling reverse proxy keeps the connection after its playlist request was answered. When the
controller then stops the viewer's session, MistOutHLS must close the connection without writing
anything: an unrequested error playlist on an idle connection would be read by the proxy as the
answer to the next request it sends on it.
"""
from pathlib import Path
import select
import socket
import sys
import time

from controller_capacity_lifecycle_integration import Fixture


def read_response(conn, deadline):
    data = b''
    while b'\r\n\r\n' not in data:
        data += receive(conn, deadline)
    head, body = data.split(b'\r\n\r\n', 1)
    lines = head.decode('latin-1').split('\r\n')
    headers = {k.strip().lower(): v.strip() for k, v in (line.split(':', 1) for line in lines[1:])}
    assert 'chunked' not in headers.get('transfer-encoding', ''), ('playlist unexpectedly chunked', lines)
    length = int(headers['content-length'])
    while len(body) < length:
        body += receive(conn, deadline)
    assert len(body) == length, ('playlist response carried trailing bytes', body[length:])
    return lines[0], body


def receive(conn, deadline):
    remaining = deadline - time.monotonic()
    assert remaining > 0, 'timed out waiting for the playlist response'
    ready, _, _ = select.select([conn], [], [], remaining)
    assert ready, 'timed out waiting for the playlist response'
    chunk = conn.recv(65536)
    assert chunk, 'connection closed before the playlist response completed'
    return chunk


def main():
    build, ffmpeg = Path(sys.argv[1]).resolve(), sys.argv[2]
    fixture = Fixture(build / 'MistController', ffmpeg)
    success = False
    try:
        fixture.start(media='mp4')
        conn = socket.create_connection(('127.0.0.1', fixture.http), timeout=30)
        try:
            conn.sendall(('GET /hls/%s/index.m3u8 HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: keep-alive\r\n'
                          'User-Agent: hls-idle-session-end\r\n\r\n' % fixture.stream).encode())
            status, playlist = read_response(conn, time.monotonic() + 60)
            assert status.startswith('HTTP/1.1 200'), (status, playlist)
            assert b'#EXTM3U' in playlist and b'EXT-X-ERROR' not in playlist, ('no playable playlist', playlist)

            # The request is answered and the connection is idle. Stop the stream's sessions until
            # the output takes the session end and closes the connection.
            stray = b''
            closed = False
            deadline = time.monotonic() + 40
            while not closed and time.monotonic() < deadline:
                fixture.request({'stop_sessions': fixture.stream})
                ready, _, _ = select.select([conn], [], [], 1)
                while ready:
                    chunk = conn.recv(65536)
                    if not chunk:
                        closed = True
                        break
                    stray += chunk
                    ready, _, _ = select.select([conn], [], [], 1)
            assert not stray, ('session end wrote an unrequested response on an idle connection', stray.decode('latin-1'))
            assert closed, 'the idle HLS connection stayed open after its session was stopped'
        finally:
            conn.close()
        print('PASS a stopped session closes an idle HLS connection without an unrequested response')
        success = True
    finally:
        fixture.close(failed=not success)
    return 0


if __name__ == '__main__':
    sys.exit(main())
