#!/usr/bin/env python3
"""Observe the real listener's signal-handler descriptor after dup2."""
from pathlib import Path
import os
import re
import signal
import socket
import subprocess
import sys
import tempfile
import time
from test_ports import free_port

binary, auditor = sys.argv[1:]
with tempfile.TemporaryDirectory(prefix='mist-webrtc-fd-') as directory:
    marker = Path(directory) / 'shutdown'
    # GCC's shared ASan runtime must precede an interposer; Clang's static runtime initializes in preinit.
    libraries = subprocess.run(['ldd', binary], capture_output=True, text=True, check=True).stdout
    runtimes = re.findall(r'=> (/\S*(?:libasan|libclang_rt\.asan)\S*)', libraries)
    env = dict(os.environ, LD_PRELOAD=':'.join(runtimes + [auditor]), MIST_SHUTDOWN_AUDIT=str(marker), TMP=directory)
    port = free_port()
    with (Path(directory) / 'listener.log').open('w+') as log:
        process = subprocess.Popen([binary, '-i', '127.0.0.1', '-p', str(port)], env=env, stdout=log, stderr=subprocess.STDOUT)
        try:
            until = time.monotonic() + 10
            while True:
                assert process.poll() is None, 'listener failed before readiness'
                with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
                    try:probe.bind(('127.0.0.1', port))
                    except OSError:break
                assert time.monotonic() < until, 'listener never bound'
                time.sleep(.02)
            process.send_signal(signal.SIGTERM)
            assert process.wait(timeout=2) == 0, 'listener stop failed'
            actual = marker.read_text() if marker.exists() else ''
            assert actual == '0', 'signal handler used pre-dup2 descriptor instead of fd 0: ' + repr(actual)
        finally:
            if process.poll() is None:process.kill();process.wait()
    print('PASS real WebRTC shutdown targets the assimilated fd 0')
