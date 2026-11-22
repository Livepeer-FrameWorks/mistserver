#!/usr/bin/env python3
"""A fixture releases only its own child and crash IPC, including on timeout."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
import controller_capacity_lifecycle_integration as capacity_fixture
from fixture_ipc import cleanup_logs, release_page, run_owned, wait_dead


class FixtureIpcTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.env = dict(os.environ, TMP=str(self.root))
        (self.root / 'mist').mkdir()

    def test_child_exit_status_and_unrelated_page_survive(self):
        other = self.root / 'mist/MstProcState_100000001'
        other.write_bytes(b'unrelated')
        code = "import os,pathlib; p=os.getpid(); pathlib.Path(os.environ['TMP']+'/mist/MstProcState_'+str(p)).write_bytes(b'state'); print(p); raise SystemExit(2)"
        result = run_owned([sys.executable, '-c', code], env=self.env, stdout=subprocess.PIPE)
        self.assertEqual(result.returncode, 2)
        self.assertFalse((self.root / ('mist/MstProcState_' + result.stdout.decode().strip())).exists())
        self.assertEqual(other.read_bytes(), b'unrelated')

    def test_timeout_releases_child_page(self):
        code = "import os,pathlib,time; p=os.getpid(); pathlib.Path(os.environ['TMP']+'/mist/MstProcState_'+str(p)).write_bytes(b'state'); pathlib.Path(os.environ['TMP']+'/started').write_text(str(p)); time.sleep(30)"
        with self.assertRaises(subprocess.TimeoutExpired):
            run_owned([sys.executable, '-c', code], timeout=1, env=self.env)
        self.assertTrue((self.root / 'started').exists(), 'child did not reach page creation')
        self.assertEqual(list((self.root / 'mist').iterdir()), [])

    def test_crash_cleanup_has_exact_stream_boundary(self):
        names = ['MstProcState_100000002', 'MstAttRes_100000002_0', 'MstDatatest1@8_0', 'MstTraktest1@8-0', 'MstIPIDtest1', 'MstSessionIowned', 'MstSemSessIowned']
        for name in names:
            (self.root / 'mist' / name).write_bytes(b'owned')
        other = self.root / 'mist/MstDatatest10@8_0'
        other.write_bytes(b'unrelated')
        (self.root / 'input.log').write_text('INFO|MistProcLivepeer|100000002|source||Booting new session Iowned\n')
        cleanup_logs(self.root, ['test1'], env=self.env)
        self.assertTrue(other.exists())
        self.assertTrue(all(not (self.root / 'mist' / name).exists() for name in names))

    def test_finished_group_is_not_signaled_again(self):
        fixture = capacity_fixture.Fixture.__new__(capacity_fixture.Fixture)
        fixture.process = mock.Mock(pid=12345)
        fixture.process.poll.return_value = 0
        fixture.group = 12345
        fixture.children = []
        fixture.root = self.root
        fixture.env = self.env
        fixture.stream = 'owned'
        fixture.streams = set()
        fixture.controller = str(self.root / 'MistController')
        fixture.log = mock.Mock()
        fixture.temp = mock.Mock()
        with mock.patch.object(capacity_fixture, 'process_table', return_value={}), \
             mock.patch.object(capacity_fixture, 'fixture_inputs', return_value={}), \
             mock.patch.object(capacity_fixture, 'processes', return_value=set()), \
             mock.patch.object(capacity_fixture.os, 'killpg', side_effect=PermissionError('empty group')), \
             mock.patch.object(capacity_fixture, 'cleanup_logs'):
            fixture.close()

    def test_wait_dead_rejects_live_child_and_accepts_its_death(self):
        child = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(30)'])
        try:
            with self.assertRaises(TimeoutError):
                wait_dead(child.pid, timeout=0.05)
            child.kill()
            wait_dead(child.pid)
        finally:
            if child.poll() is None:
                child.kill()
            child.wait()
        wait_dead(child.pid)

    def test_rejects_path_traversal(self):
        with self.assertRaises(ValueError):
            release_page('/Mst/../../unrelated', self.env)


if __name__ == '__main__':
    unittest.main()
