#!/usr/bin/env python3
"""Exercise controller process ownership, WHEP liveness and local byte accounting."""
import contextlib
import ctypes
import hashlib
import hmac
import http.client
import json
import os
import select
import shutil
from pathlib import Path
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time
import urllib.parse
import urllib.request
import zlib
from fixture_ipc import cleanup_logs


def until(predicate, seconds, message):
    deadline = time.monotonic() + seconds
    while True:
        result = predicate()
        if result:
            return result
        if time.monotonic() >= deadline:
            raise AssertionError(message)
        time.sleep(0.1)


def free_port(kind=socket.SOCK_STREAM):
    with socket.socket(socket.AF_INET, kind) as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def process_table():
    if Path('/proc/self/stat').exists():
        result = {}
        for path in Path('/proc').glob('[0-9]*/stat'):
            try:
                fields = path.read_text().rsplit(')', 1)[1].split()
                result[int(path.parent.name)] = (int(fields[1]), int(fields[2]), fields[0], '')
            except (FileNotFoundError, ProcessLookupError, PermissionError):
                continue
        return result
    rows = subprocess.check_output(['ps', '-axo', 'pid=,ppid=,pgid=,stat=,command='], text=True)
    return {int(parts[0]): (int(parts[1]), int(parts[2]), parts[3], parts[4])
            for line in rows.splitlines() if len(parts := line.split(None, 4)) == 5}


def processes(group, name=None):
    result = set()
    for pid, (_, pgid, state, command) in process_table().items():
        if pgid != group or state.startswith('Z'):
            continue
        if name is not None and Path('/proc/self/stat').exists():
            try:
                command = Path('/proc/%d/cmdline' % pid).read_bytes().replace(b'\0', b' ').decode()
            except (FileNotFoundError, ProcessLookupError, PermissionError):
                continue
        if name is None or name in command:
            result.add(pid)
    return result


class Fixture:
    def __init__(self, controller, ffmpeg):
        self.temp = tempfile.TemporaryDirectory(prefix='mist-capacity-lifecycle-')
        self.root = Path(self.temp.name)
        (self.root / 'ipc').mkdir()
        self.api, self.http, self.udp = free_port(), free_port(), free_port(socket.SOCK_DGRAM)
        self.stream = 'capacity' + str(os.getpid())
        self.env = dict(os.environ, TMP=str(self.root / 'ipc'), MIST_CONTROL='1')
        self.env.pop('ATHEIST', None)
        self.env.pop('CTRL_ATHEIST', None)
        self.log = (self.root / 'controller.log').open('w+')
        self.controller = str(Path(controller).resolve())
        self.children = []
        self.process = None
        self.group = None
        self.ffmpeg = ffmpeg

    def start(self, media=False, streams_override=None, extra_protocols=(), triggers_override=None):
        streams = {}
        if media:
            source = self.root / ('source.mkv' if media == 'mkv' else 'source.mp4' if media in ('mp4', 'mp4-no-b', 'fragmented', 'fragmented-moof') else 'source.webm')
            codecs = ['-c:v', 'libx264', '-bf', '0' if media == 'mp4-no-b' else '2', '-c:a', 'aac', *([] if media == 'mkv' else ['-movflags', '+faststart'])] if media in ('mp4', 'mp4-no-b', 'mkv', 'fragmented', 'fragmented-moof') else ['-c:v', 'libvpx', '-deadline', 'realtime', '-cpu-used', '8', '-b:v', '2M', '-c:a', 'libopus']
            subprocess.run([self.ffmpeg, '-hide_banner', '-loglevel', 'error', '-y', '-f', 'lavfi', '-i', 'testsrc2=size=320x180:rate=24', '-f', 'lavfi', '-i', 'sine=frequency=440:sample_rate=48000', '-t', '12', *codecs, *(['-movflags', 'frag_keyframe+empty_moov'+('+default_base_moof' if media=='fragmented-moof' else '')] if media in ('fragmented', 'fragmented-moof') else []), str(source)], check=True, timeout=40)
            streams[self.stream] = {'source': str(source)}
        config = {'bandwidth': {'exceptions': ['::/127']}, 'config': {'controller': {'interface': '127.0.0.1', 'port': self.api, 'username': None}, 'prometheus': 'metrics', 'debug': 4, 'protocols': [{'connector': 'HTTP', 'interface': '127.0.0.1', 'port': self.http}, {'connector': 'WebRTC', 'interface': '127.0.0.1', 'pubhost': '127.0.0.1', 'port': self.udp}, {'connector': 'EBML'}, {'connector': 'HLS'}, {'connector': 'MP4'}]}, 'streams': streams}
        if streams_override is not None:
            config['streams'] = streams_override
        config['config']['protocols'].extend(extra_protocols)
        if triggers_override is not None:
            config['config']['triggers'] = triggers_override
        path = self.root / 'config.json'
        path.write_text(json.dumps(config))
        self.process = subprocess.Popen([self.controller, '-c', str(path), '-L', str(self.root / 'mist.log')], env=self.env, stdout=self.log, stderr=subprocess.STDOUT, start_new_session=True)
        def ready():
            try:
                with urllib.request.urlopen('http://127.0.0.1:%d/crossdomain.xml' % self.http, timeout=1) as response:
                    return response.status == 200
            except (OSError, http.client.HTTPException):
                return False
        until(ready, 30, 'HTTP connector did not start')
        self.group = os.getpgid(self.process.pid)

    def request(self, command):
        url = 'http://127.0.0.1:%d/api2?%s' % (self.api, urllib.parse.urlencode({'command': json.dumps(command)}))
        with urllib.request.urlopen(url, timeout=5) as response:
            return json.load(response)

    def stats(self):
        with urllib.request.urlopen('http://127.0.0.1:%d/metrics.json' % self.api, timeout=5) as response:
            return json.load(response)

    def close(self, failed=False):
        if self.process and self.group is None:
            child = (self.process.pid, os.getpgid(self.process.pid))
            if child:
                _, self.group = child
        if failed:
            if self.process:
                for pid in sorted(processes(self.group)):
                    with contextlib.suppress(FileNotFoundError, ProcessLookupError):
                        print('owned process', pid, (Path('/proc/%d/cmdline' % pid).read_bytes().replace(b'\0', b' ')), 'wait', Path('/proc/%d/wchan' % pid).read_text(), file=sys.stderr)
            for filename in ('controller.log', 'mist.log'):
                if (self.root / filename).exists():
                    print((self.root / filename).read_text(errors='replace')[-10000:], file=sys.stderr)
        for child in self.children:
            child.terminate()
            with contextlib.suppress(subprocess.TimeoutExpired):
                child.wait(timeout=2)
            if child.poll() is None:
                child.kill()
                child.wait()
        if self.process:
            if self.process.poll() is None:
                self.process.send_signal(signal.SIGINT)
                with contextlib.suppress(subprocess.TimeoutExpired):
                    self.process.wait(timeout=6)
            # Give the fixture's remaining handlers the same graceful group
            # shutdown that the edge supervisor owns, then bound the wait.
            group = self.group or self.process.pid
            if processes(group):
                with contextlib.suppress(ProcessLookupError):
                    os.killpg(group, signal.SIGTERM)
            deadline = time.monotonic() + 8
            while processes(group) and time.monotonic() < deadline:
                time.sleep(0.05)
            if processes(group):
                with contextlib.suppress(ProcessLookupError):
                    os.killpg(group, signal.SIGKILL)
            self.process.wait()
        self.log.close()
        cleanup_logs(self.root, [self.stream], env=self.env)
        self.temp.cleanup()


def udp_listener_ready(process, port):
    assert process.poll() is None, 'WebRTC listener exited before binding'
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
            probe.bind(('127.0.0.1', port))
    except OSError:
        return True
    return False


def webrtc_idle(fixture):
    for stop_signal in (signal.SIGTERM, signal.SIGUSR1):
        path = fixture.root / ('listener-' + str(stop_signal))
        path.symlink_to(Path(fixture.controller).parent / 'MistOutWebRTC')
        listener = subprocess.Popen([str(path), '-p', str(fixture.udp), '-i', '127.0.0.1'], env=fixture.env, stdout=fixture.log, stderr=subprocess.STDOUT)
        try:
            until(lambda: udp_listener_ready(listener, fixture.udp), 5, 'idle WebRTC listener did not bind')
            if stop_signal == signal.SIGUSR1:
                path.unlink()
                path.write_text('#!/bin/sh\nexit 0\n')
                path.chmod(0o755)
            started = time.monotonic()
            listener.send_signal(stop_signal)
            assert listener.wait(timeout=1) == 0, 'idle listener did not stop/reload cleanly'
            assert time.monotonic() - started < 1, 'idle listener took over one second to stop/reload'
        finally:
            if listener.poll() is None:
                listener.kill(); listener.wait()


def webrtc_reload(fixture):
    path = fixture.root / 'reload-listener'
    marker = fixture.root / 'reexec-socket'
    path.symlink_to(Path(fixture.controller).parent / 'MistOutWebRTC')
    listener = subprocess.Popen([str(path), '-p', str(fixture.udp), '-i', '127.0.0.1'], env=fixture.env, stdout=fixture.log, stderr=subprocess.STDOUT)
    try:
        until(lambda: udp_listener_ready(listener, fixture.udp), 5, 'WebRTC reload listener did not bind')
        path.unlink()
        path.write_text('#!/usr/bin/env python3\nimport socket\nfrom pathlib import Path\ns=socket.socket(fileno=0)\nassert s.getsockname()[1]==' + str(fixture.udp) + '\nPath(' + repr(str(marker)) + ').write_text("reused")\n')
        path.chmod(0o755)
        listener.send_signal(signal.SIGUSR1)
        assert listener.wait(timeout=2) == 0, 'WebRTC reload did not complete'
        assert marker.exists(), 'WebRTC rolling restart closed the UDP socket before re-exec'
    finally:
        if listener.poll() is None:
            listener.kill(); listener.wait()


def record_mp4(fixture, source):
    fixture.start(media=source)
    target = fixture.root / 'recording.mp4'
    output = Path(fixture.controller).parent / 'MistOutMP4'
    child = subprocess.Popen([str(output), '-s', fixture.stream, str(target)], env=fixture.env,
                             stdout=fixture.log, stderr=subprocess.STDOUT)
    fixture.children.append(child)
    assert child.wait(timeout=40) == 0, 'MP4 recording exited with an error'
    assert target.exists() and target.stat().st_size > 4096, 'MP4 recording produced no media'
    decode = subprocess.run([fixture.ffmpeg, '-hide_banner', '-loglevel', 'error', '-xerror', '-i', str(target),
                             '-map', '0:v', '-map', '0:a', '-f', 'null', '-'], capture_output=True, timeout=30)
    assert decode.returncode == 0, 'MP4 recording does not decode: ' + decode.stderr.decode(errors='replace')
    probe = shutil.which('ffprobe') or str(Path(fixture.ffmpeg).with_name('ffprobe'))
    def frames(path):
        info = json.loads(subprocess.check_output([probe, '-v', 'error', '-count_frames', '-show_entries',
                          'stream=codec_type,nb_read_frames', '-of', 'json', str(path)], timeout=30))
        return {row['codec_type']: int(row['nb_read_frames']) for row in info['streams']}
    original = fixture.root / ('source.mkv' if source == 'mkv' else 'source.mp4')
    actual, expected = frames(target), frames(original)
    assert actual['video'] == expected['video'], 'MP4 recording lost video frames: %r vs %r' % (actual, expected)
    # Source MP4 edit lists may hide AAC priming; transmuxing must retain every audible frame.
    assert expected['audio'] <= actual['audio'] <= expected['audio'] + 2, 'MP4 recording lost audio frames: %r vs %r' % (actual, expected)


def progressive_mp4(fixture, media='mp4'):
    fixture.start(media=media)
    target = fixture.root / 'progressive.mp4'
    with urllib.request.urlopen('http://127.0.0.1:%d/%s.mp4' % (fixture.http, fixture.stream), timeout=30) as response:
        assert response.status == 200, 'progressive MP4 request was refused'
        target.write_bytes(response.read())
    result = subprocess.run([fixture.ffmpeg, '-hide_banner', '-loglevel', 'error', '-xerror', '-i', str(target), '-map', '0:v:0', '-map', '0:a:0', '-f', 'null', '-'], capture_output=True, text=True, timeout=30)
    assert result.returncode == 0, 'progressive H264/AAC MP4 does not decode cleanly: ' + result.stderr


def offer(fixture):
    sdp = '\r\n'.join(['v=0', 'o=- 1 1 IN IP4 127.0.0.1', 's=-', 't=0 0', 'a=group:BUNDLE 0', 'm=audio 9 UDP/TLS/RTP/SAVPF 111', 'c=IN IP4 0.0.0.0', 'a=ice-ufrag:capacityclient', 'a=ice-pwd:capacity-client-password-long', 'a=fingerprint:sha-256 ' + ':'.join(['AB'] * 32), 'a=setup:actpass', 'a=mid:0', 'a=recvonly', 'a=rtcp-mux', 'a=rtpmap:111 opus/48000/2', ''])
    request = urllib.request.Request('http://127.0.0.1:%d/whep/%s' % (fixture.http, fixture.stream), data=sdp.encode(), headers={'Content-Type': 'application/sdp'}, method='POST')
    with urllib.request.urlopen(request, timeout=15) as response:
        assert response.status == 201, 'WHEP offer was not answered'
        return response.read().decode()


def stun_packet(username, password):
    def attribute(kind, value):
        return struct.pack('!HH', kind, len(value)) + value + b'\0' * (-len(value) % 4)
    attrs = attribute(6, username.encode()) + attribute(0x24, struct.pack('!I', 0x6effffff))
    transaction = os.urandom(12)
    header = struct.pack('!HHI', 1, len(attrs) + 24, 0x2112a442) + transaction
    packet = header + attrs + attribute(8, hmac.new(password.encode(), header + attrs, hashlib.sha1).digest())
    packet = struct.pack('!HH', 1, len(packet) - 20 + 8) + packet[4:]
    return packet + attribute(0x8028, struct.pack('!I', zlib.crc32(packet) ^ 0x5354554e)), transaction


def receive_stun_success(udp, transaction, deadline):
    while time.monotonic() < deadline:
        response, _ = udp.recvfrom(2048)
        assert len(response) >= 20, 'ICE check returned a truncated STUN response'
        if response[8:20] != transaction:
            continue
        kind, length, cookie = struct.unpack('!HHI', response[:8])
        assert (kind, cookie) == (0x101, 0x2112a442), 'ICE check was not accepted for this transaction'
        assert length == len(response) - 20, 'STUN response has an invalid length'
        return
    raise TimeoutError('ICE transaction deadline expired')


def stale_stun_replies(fixture):
    transaction = b'currentcheck'
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp, socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as peer:
        udp.bind(('127.0.0.1', 0))
        udp.settimeout(0.5)
        for tid in (b'old-reply-id', transaction):
            peer.sendto(struct.pack('!HHI', 0x101, 0, 0x2112a442) + tid, udp.getsockname())
        receive_stun_success(udp, transaction, time.monotonic() + 1)


def whep_liveness(fixture):
    fixture.start(media=True)
    offer(fixture)
    handlers = until(lambda: processes(fixture.group, 'MistOutWebRTC') & processes(fixture.group, fixture.stream), 5, 'answered WHEP handler is missing')
    until(lambda: not (handlers & processes(fixture.group)), 15, 'silent WHEP client retained its output after the liveness deadline')
    until(lambda: 'received no data for 10+ seconds' in (fixture.root / 'mist.log').read_text(errors='replace'), 2, 'silent WHEP output did not retain its 10-second inactivity exit reason')
    answer = offer(fixture)
    handlers = until(lambda: processes(fixture.group, 'MistOutWebRTC') & processes(fixture.group, fixture.stream), 5, 'second WHEP handler is missing')
    fields = dict(line[2:].split(':', 1) for line in answer.splitlines() if line.startswith(('a=ice-ufrag:', 'a=ice-pwd:')))
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
        udp.settimeout(0.25)
        candidates = [line.split() for line in answer.splitlines() if line.startswith('a=candidate:')]
        candidate = next((row for row in candidates if len(row) >= 8 and row[1] == '1' and row[2].lower() == 'udp' and row[6:8] == ['typ', 'host']), None)
        assert candidate is not None, 'WHEP answer has no UDP host candidate: ' + answer
        destination = (candidate[4], int(candidate[5]))
        deadline = time.monotonic() + 13
        while time.monotonic() < deadline:
            packet, transaction = stun_packet(fields['ice-ufrag'] + ':capacityclient', fields['ice-pwd'])
            check_deadline = time.monotonic() + 2
            while True:
                # The first datagram creates the connected UDP socket; ICE retransmits
                # until that socket can answer, just as a browser's ICE agent does.
                udp.sendto(packet, destination)
                try:
                    receive_stun_success(udp, transaction, check_deadline)
                except TimeoutError:
                    assert time.monotonic() < check_deadline, 'WHEP candidate did not answer ICE checks: %r' % (destination,)
                    continue
                break
            assert handlers <= processes(fixture.group), 'STUN-only WHEP client was expired while sending checks'
            time.sleep(1)
    until(lambda: not (handlers & processes(fixture.group)), 15, 'WHEP output did not end after authenticated ICE checks stopped')


def whep_dtls_idle(fixture):
    fixture.start(media=True)
    offer(fixture)
    handlers = until(lambda: processes(fixture.group, 'MistOutWebRTC') & processes(fixture.group, fixture.stream), 5, 'idle WHEP handler is missing')
    assert len(handlers) == 1, 'idle WHEP probe requires one playback handler'
    pid = next(iter(handlers))
    path = Path('/proc/%d/status' % pid)
    def switches():
        if path.exists():
            return int(next(line.split(':', 1)[1] for line in path.read_text().splitlines() if line.startswith('voluntary_ctxt_switches:')))
        # Darwin's ps exposes nvcsw as '-', so read native process wakeups instead.
        usage = ctypes.create_string_buffer(96)  # struct rusage_info_v0
        assert ctypes.CDLL('/usr/lib/libproc.dylib').proc_pid_rusage(pid, 0, ctypes.byref(usage)) == 0
        return struct.unpack_from('Q', usage.raw, 40)[0]
    before = switches()
    time.sleep(2.2)
    delta = switches() - before
    assert delta < 12, 'a non-DTLS-client WHEP session kept its 100 ms timer: %d wakeups' % delta


def internal_bandwidth(fixture):
    fixture.start(media=True)
    output = Path(fixture.controller).parent / 'MistOutEBML'
    target = fixture.root / 'local.mkv'
    child = subprocess.Popen([str(output), '-s', fixture.stream, str(target)], env=fixture.env, stdout=fixture.log, stderr=subprocess.STDOUT)
    fixture.children.append(child)
    until(lambda: target.exists() and target.stat().st_size > 65536, 25, 'local recording did not write media bytes')
    def recorded_counters():
        counters = fixture.stats()
        return counters if counters['obw'][0] + counters['bw'][0] > 65536 else None
    counters = until(recorded_counters, 10, 'recording counters did not reach the controller')
    assert counters['obw'][0] > 65536, 'local file bytes are absent from internal bandwidth: %r' % counters
    assert counters['bw'][0] == 0, 'local file bytes inflated external node bandwidth: %r' % counters
    assert counters['streams'][fixture.stream]['bw'][0] == 0, 'local file bytes inflated per-stream bandwidth: %r' % counters
    connection = http.client.HTTPConnection('127.0.0.1', fixture.http, timeout=10)
    try:
        connection.request('GET', '/%s.mkv' % fixture.stream, headers={'X-Forwarded-For': '203.0.113.7'})
        response = connection.getresponse()
        assert response.status == 200
        assert len(response.read(131072)) == 131072, 'forwarded viewer received no media'
        until(lambda: fixture.stats()['bw'][0] > 65536, 12, 'real forwarded viewer bytes were excluded from external bandwidth')
    finally:
        connection.close()


def main():
    mode, controller, ffmpeg = sys.argv[1:]
    if mode in ('whep', 'whep-idle-timer', 'bandwidth', 'progressive-mp4', 'fragmented-mp4', 'fragmented-mp4-moof', 'record-mp4-no-b', 'record-mp4-av', 'record-mkv') and not shutil.which(ffmpeg):
        print('SKIP: this media regression requires ffmpeg')
        sys.exit(77)
    fixture = Fixture(controller, ffmpeg)
    failed = True
    try:
        {'record-mp4-no-b': lambda f: record_mp4(f, 'mp4-no-b'), 'record-mp4-av': lambda f: record_mp4(f, 'mp4'), 'record-mkv': lambda f: record_mp4(f, 'mkv'), 'whep-idle-timer': whep_dtls_idle, 'stun-replies': stale_stun_replies, 'webrtc-idle': webrtc_idle, 'webrtc-reload': webrtc_reload, 'whep': whep_liveness, 'bandwidth': internal_bandwidth, 'progressive-mp4': progressive_mp4, 'fragmented-mp4': lambda f: progressive_mp4(f, 'fragmented'), 'fragmented-mp4-moof': lambda f: progressive_mp4(f, 'fragmented-moof')}[mode](fixture)
        failed = False
        print('PASS:', mode)
    finally:
        fixture.close(failed)


if __name__ == '__main__':
    main()
