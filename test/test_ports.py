#!/usr/bin/env python3
"""Reserve listen ports for test fixtures outside the kernel's ephemeral port range.

Outgoing connections and bind(0) in the same network namespace take their local port
from the ephemeral range, so a fixture port picked there can be taken by an unrelated
socket before Mist binds it. Ports come from outside that range, must be free for TCP
and UDP on IPv4 and IPv6, and stay reserved for the owning process while it lives, so
concurrent fixtures never hand out the same port.

Shell fixtures run `test_ports.py COUNT OWNER_PID`, which prints the first of COUNT
consecutive reserved ports; Python fixtures call free_port().
"""
import errno
import fcntl
import os
from pathlib import Path
import random
import socket
import sys
import tempfile

# Below this, Mist's default ports (4242 UDP API, 1935 RTMP, 8080 HTTP, ...) and system services live.
LOWEST = 10000


def ephemeral_range():
    try:
        low, high = Path('/proc/sys/net/ipv4/ip_local_port_range').read_text().split()
        return int(low), int(high)
    except (OSError, ValueError):
        # BSD and macOS default (net.inet.ip.portrange.first/last).
        return 49152, 65535


def candidate_ports():
    low, high = ephemeral_range()
    ports = [port for port in range(LOWEST, 65536) if not low <= port <= high]
    return ports or [port for port in range(1024, 65536) if not low <= port <= high]


def bindable(port):
    """True when nothing holds the port for TCP or UDP on either address family."""
    for family, address in ((socket.AF_INET, '0.0.0.0'), (socket.AF_INET6, '::')):
        for kind in (socket.SOCK_STREAM, socket.SOCK_DGRAM):
            try:
                probe = socket.socket(family, kind)
            except OSError as error:
                if family == socket.AF_INET6 and error.errno in (errno.EAFNOSUPPORT, errno.EPROTONOSUPPORT):
                    break
                raise
            with probe:
                if family == socket.AF_INET6:
                    probe.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 1)
                try:
                    probe.bind((address, port))
                except OSError as error:
                    if family == socket.AF_INET6 and error.errno == errno.EADDRNOTAVAIL:
                        break
                    return False
    return True


def alive(pid):
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True


def owner_of(entry):
    try:
        return int(entry.read_text())
    except (OSError, ValueError):
        return None


def free_port(count=1, owner=None):
    """Reserve `count` consecutive ports for `owner` (default: this process) and return the first."""
    owner = os.getpid() if owner is None else int(owner)
    directory = Path(tempfile.gettempdir()) / ('mist-test-ports-%d' % os.getuid())
    directory.mkdir(mode=0o700, exist_ok=True)
    with (directory / 'lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        reserved = set()
        for entry in directory.glob('[0-9]*'):
            holder = owner_of(entry)
            if holder is None or not alive(holder):
                entry.unlink(missing_ok=True)
            else:
                reserved.add(int(entry.name))
        ports = candidate_ports()
        usable = set(ports)
        start = random.randrange(len(ports))
        for base in ports[start:] + ports[:start]:
            block = range(base, base + count)
            if all(port in usable and port not in reserved for port in block) and all(bindable(port) for port in block):
                for port in block:
                    (directory / str(port)).write_text(str(owner))
                return base
    raise RuntimeError('no %d consecutive free ports outside the ephemeral range' % count)


if __name__ == '__main__':
    if len(sys.argv) != 3:
        sys.exit('usage: test_ports.py COUNT OWNER_PID')
    print(free_port(int(sys.argv[1]), int(sys.argv[2])))
