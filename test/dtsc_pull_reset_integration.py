#!/usr/bin/env python3
"""A DTSC pull whose source connection resets ends once, without replaying its last message.

A replica pulls its media over DTSC. Some messages make the pull write to its source: a ping gets
a pong, and a keyframe after keys of very unequal duration asks the source to confirm the key
duration. When the source connection was reset, that write fails and closes the connection after
the message was read. The pull must then end with "no more data", and its viewers must receive
each source packet once: replaying the message that was last read floods the replica with copies
of one packet, filling its data page, or logs it as a packet without a track, until the session
shuts the input down.

A proxy between the replica and the source forwards the source's DTSC stream, then pauses the pull
process, sends one trigger message and resets the connection, so the pull reads the trigger from
a connection that is already reset:
  ping      a ping the proxy sends in place of the source's second keyframe.
  keyframe  the source keyframe at 6 s. The source has keyframes at 0, 2, 2.2, 6 and 8 s, so the
            replica asks the source to confirm the key duration on this keyframe.
"""
import contextlib
import fcntl
import os
from pathlib import Path
import select
import signal
import socket
import struct
import subprocess
import sys
import termios
import threading
import time

from controller_capacity_lifecycle_integration import Fixture, fixture_inputs, until
from test_ports import free_port


def pack(values):
    """Encodes a flat dict as a DTSC object."""
    out = b'\xe0'
    for key, value in values.items():
        out += struct.pack('>H', len(key)) + key.encode()
        if isinstance(value, int):
            out += b'\x01' + struct.pack('>Q', value)
        else:
            out += b'\x02' + struct.pack('>I', len(value)) + value.encode()
    return out + b'\x00\x00\xee'


def command(values):
    body = pack(values)
    return b'DTCM' + struct.pack('>I', len(body)) + body


def frames(sock, buf):
    """Yields complete DTSC frames from sock; buf carries partial data across calls."""
    while True:
        while len(buf) >= 8:
            size = struct.unpack('>I', buf[4:8])[0]
            if len(buf) < 8 + size:
                break
            frame = bytes(buf[:8 + size])
            del buf[:8 + size]
            yield frame
        chunk = sock.recv(65536)
        if not chunk:
            return
        buf += chunk


def is_keyframe(frame):
    return frame[:4] == b'DTP2' and b'\x00\x08keyframe\x01' in frame[20:]


def media_time(frame):
    return struct.unpack('>Q', frame[12:20])[0]


class ResetProxy:
    """Relays one DTSC pull to the source and resets it right after the trigger message."""

    def __init__(self, upstream_port, mode, pause):
        self.upstream_port, self.mode, self.pause = upstream_port, mode, pause
        self.ready = lambda: False
        self.listener = socket.socket()
        self.listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.port = free_port()
        self.listener.bind(('127.0.0.1', self.port))
        self.listener.listen(4)
        self.reset_at = None
        self.trigger = None
        self.error = None
        threading.Thread(target=self.run, daemon=True).start()

    def run(self):
        try:
            client, _ = self.listener.accept()
            upstream = socket.create_connection(('127.0.0.1', self.upstream_port))
            stop = threading.Event()
            pump = threading.Thread(target=self.pump, args=(client, upstream, stop), daemon=True)
            pump.start()
            keyframes = 0
            trigger = None
            for frame in frames(upstream, bytearray()):
                if self.mode == 'ping' and is_keyframe(frame) and keyframes:
                    # The replica buffered media; now it reads a ping it must answer.
                    trigger = command({'cmd': 'ping'})
                    self.trigger = 'ping after %d ms' % media_time(frame)
                    break
                if self.mode == 'keyframe' and is_keyframe(frame) and media_time(frame) >= 6000:
                    trigger = frame
                    self.trigger = 'keyframe at %d ms' % media_time(frame)
                    break
                client.sendall(frame)
                keyframes += is_keyframe(frame)
            assert trigger, 'the source ended before the trigger message'
            # The viewer must be playing the replica before the reset ends it.
            deadline = time.monotonic() + 20
            while not self.ready():
                assert time.monotonic() < deadline, 'the replica viewer received no media before the trigger'
                time.sleep(0.05)
            # The trigger and the reset both arrive while the pull is paused, as when a loaded host
            # does not schedule it: it then reads the trigger, and every write it makes fails.
            # A reset discards what the replica's host did not acknowledge yet, so the reset waits
            # until the replica's host holds all data.
            self.drain(client)
            resume = self.pause()
            try:
                client.sendall(trigger)
                self.drain(client)
                # The reset goes out when the socket is released, which a pending recv delays.
                stop.set()
                pump.join()
                client.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0))
                client.close()
                upstream.close()
                time.sleep(0.2)
            finally:
                resume()
            self.reset_at = time.monotonic()
        except (OSError, AssertionError) as error:
            self.error = repr(error)

    @staticmethod
    def drain(sock):
        """Waits until the peer acknowledged every byte sent on sock."""
        deadline = time.monotonic() + 10
        while struct.unpack('i', fcntl.ioctl(sock.fileno(), termios.TIOCOUTQ, b'\0' * 4))[0]:
            assert time.monotonic() < deadline, 'the replica did not take the forwarded data'
            time.sleep(0.01)

    @staticmethod
    def pump(client, upstream, stop):
        try:
            while not stop.is_set():
                if not select.select([client], [], [], 0.05)[0]:
                    continue
                data = client.recv(65536)
                if not data:
                    return
                upstream.sendall(data)
        except OSError:
            return


class Viewer:
    """Plays a stream over DTSC and records each media packet's track and time."""

    def __init__(self, port, stream):
        self.packets = []
        self.sock = socket.create_connection(('127.0.0.1', port))
        self.sock.sendall(command({'cmd': 'play', 'stream': stream}))
        threading.Thread(target=self.run, daemon=True).start()

    def run(self):
        try:
            for frame in frames(self.sock, bytearray()):
                if frame[:4] == b'DTCM' and b'ping' in frame:
                    self.sock.sendall(command({'cmd': 'ok', 'msg': 'Pong!'}))
                elif frame[:4] == b'DTP2':
                    self.packets.append((struct.unpack('>I', frame[8:12])[0], media_time(frame)))
        except OSError:
            return

    def repeats(self):
        last, repeated = {}, []
        for track, stamp in self.packets:
            if track in last and stamp <= last[track]:
                repeated.append((track, stamp))
            last[track] = stamp
        return repeated


def pull_pids(env, stream):
    pids = []
    for pid in fixture_inputs(env):
        try:
            args = Path('/proc/%d/cmdline' % pid).read_bytes().split(b'\0')
        except (FileNotFoundError, ProcessLookupError, PermissionError):
            continue
        if args and args[0].endswith(b'MistInDTSC') and stream.encode() in args:
            pids.append(pid)
    return pids


def main():
    # Pausing the pull needs its process ids from /proc.
    if os.environ.get('MIST_RUN_MEDIA_TESTS') != '1' or not Path('/proc/self/stat').exists():
        return 77
    build, ffmpeg, mode = Path(sys.argv[1]).resolve(), sys.argv[2], sys.argv[3]
    f = Fixture(build / 'MistController', ffmpeg)
    f.env['UDP_API'] = 'udp://127.0.0.1:0'
    success = False
    viewer = None
    try:
        source = f.root / 'source.mkv'
        subprocess.run([ffmpeg, '-v', 'error', '-y', '-f', 'lavfi', '-i', 'testsrc2=size=160x90:rate=10:duration=12',
                        '-c:v', 'libx264', '-threads', '1', '-preset', 'ultrafast', '-bf', '0', '-g', '1000',
                        '-sc_threshold', '0', '-force_key_frames', '0,2,2.2,6,8,10', str(source)],
                       check=True, timeout=60, env=f.tool_env)
        origin, replica = 'origin' + str(os.getpid()), 'replica' + str(os.getpid())
        dtsc = free_port()

        def pause():
            pids = pull_pids(f.env, replica)
            assert pids, 'the pull process is not running'
            for pid in pids:
                os.kill(pid, signal.SIGSTOP)

            def resume():
                for pid in pids:
                    with contextlib.suppress(ProcessLookupError):
                        os.kill(pid, signal.SIGCONT)
            return resume

        proxy = ResetProxy(dtsc, mode, pause)
        streams = {origin: {'source': str(source)}, replica: {'source': 'dtsc://127.0.0.1:%d/%s' % (proxy.port, origin)}}
        f.start(streams_override=streams, extra_protocols=[{'connector': 'DTSC', 'interface': '127.0.0.1', 'port': dtsc}])
        f.streams.update(streams)
        viewer = Viewer(dtsc, replica)
        proxy.ready = lambda: bool(viewer.packets)
        until(lambda: proxy.reset_at is not None or proxy.error, 40, 'the proxy did not reach its trigger')
        assert not proxy.error, ('proxy failed', proxy.error)
        log_path = f.root / 'mist.log'
        ended = 'MistInDTSC:%s' % replica

        def pull_ended():
            return any(ended in line and 'Input closing cleanly with reason: no more data' in line
                       for line in log_path.read_text(errors='replace').splitlines())

        deadline = time.monotonic() + 10
        while time.monotonic() < deadline and not pull_ended():
            time.sleep(0.2)
        log = log_path.read_text(errors='replace')
        mine = [line for line in log.splitlines() if ended in line]
        stray = sum('has no valid index' in line or 'exceeds size' in line for line in mine)
        repeated = viewer.repeats()
        assert pull_ended(), ('the pull did not end once its source connection reset', mode, proxy.trigger,
                              '%d stray buffer writes' % stray, '%d repeated packets' % len(repeated),
                              [line[-160:] for line in mine[-3:]])
        assert not stray, ('the pull buffered a message again after its source reset', mode, stray)
        assert not repeated, ('a replica viewer received a packet again', mode, repeated[:5], len(repeated))
        assert viewer.packets, 'the replica viewer received no media'
        success = True
        print('PASS %s: pull ended after the reset at the %s, viewer got %d packets once each' %
              (mode, proxy.trigger, len(viewer.packets)), flush=True)
        return 0
    finally:
        if viewer:
            viewer.sock.close()
        f.close(failed=not success)


if __name__ == '__main__':
    sys.exit(main())
