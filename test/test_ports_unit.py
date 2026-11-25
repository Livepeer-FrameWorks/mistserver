#!/usr/bin/env python3
"""Fixture ports stay outside the ephemeral range, skip held ports and never overlap."""
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_ports  # noqa: E402


def held(port):
    """A TCP socket in the state an outgoing connection leaves its local port: bound, not listening."""
    sock = socket.socket()
    sock.bind(('0.0.0.0', port))
    return sock


class TestPorts(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix='mist-test-ports-unit-')
        patcher = mock.patch.object(tempfile, 'tempdir', self.directory.name)
        patcher.start()
        self.addCleanup(patcher.stop)
        self.addCleanup(self.directory.cleanup)
        child = subprocess.Popen([sys.executable, '-c', 'pass'])
        child.wait()
        self.dead = child.pid

    def scratch(self, count):
        """A free block whose reservation belongs to an exited process, so the next call may hand it out."""
        return test_ports.free_port(count, owner=self.dead)

    def test_ports_avoid_the_ephemeral_range(self):
        low, high = test_ports.ephemeral_range()
        for _ in range(50):
            base = test_ports.free_port(4)
            for port in range(base, base + 4):
                self.assertFalse(low <= port <= high, '%d lies in the ephemeral range %d-%d' % (port, low, high))
                self.assertGreaterEqual(port, test_ports.LOWEST)

    def test_a_held_port_is_skipped(self):
        base = self.scratch(2)
        with held(base):
            with mock.patch.object(test_ports, 'candidate_ports', return_value=[base, base + 1]):
                self.assertEqual(test_ports.free_port(), base + 1)

    def test_a_block_is_consecutive_and_free(self):
        base = self.scratch(4)
        with held(base + 1):
            with mock.patch.object(test_ports, 'candidate_ports', return_value=[base, base + 1, base + 2, base + 3]):
                self.assertEqual(test_ports.free_port(2), base + 2)

    def test_live_reservations_never_overlap(self):
        base = self.scratch(3)
        candidates = [base, base + 1, base + 2]
        with mock.patch.object(test_ports, 'candidate_ports', return_value=candidates):
            first = test_ports.free_port(owner=os.getpid())
            second = test_ports.free_port(owner=os.getppid())
            third = test_ports.free_port()
            self.assertEqual(sorted((first, second, third)), candidates)
            with self.assertRaises(RuntimeError):
                test_ports.free_port()

    def test_a_dead_owner_releases_its_ports(self):
        base = self.scratch(1)
        with mock.patch.object(test_ports, 'candidate_ports', return_value=[base]):
            self.assertEqual(test_ports.free_port(), base)
            with self.assertRaises(RuntimeError):
                test_ports.free_port()

    def test_shell_fixtures_get_the_first_port_of_a_block(self):
        environment = dict(os.environ, TMPDIR=self.directory.name)
        output = subprocess.check_output([sys.executable, str(Path(test_ports.__file__)), '3', str(os.getpid())],
                                         env=environment, text=True)
        base = int(output)
        reserved = {int(entry.name) for entry in Path(self.directory.name).glob('mist-test-ports-*/[0-9]*')}
        self.assertEqual(reserved, {base, base + 1, base + 2})


if __name__ == '__main__':
    unittest.main()
