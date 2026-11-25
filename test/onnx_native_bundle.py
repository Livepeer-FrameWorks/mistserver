#!/usr/bin/env python3
"""Inspect the real native bundle contract using a small local Docker fixture."""
import io
import json
import os
from pathlib import Path
import subprocess
import tarfile
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
DOCKER = r'''#!/usr/bin/env python3
import io, os, pathlib, subprocess, sys, tarfile
a = sys.argv[1:]
if a[0] == 'create': print('fixture-container')
elif a[0] == 'cp':
 p = pathlib.Path(a[-1]); (p / 'lib').mkdir(parents=True, exist_ok=True)
 (p / 'lib/libfixture.so').write_text('fixture')
 (p / 'lib/libopenvino_intel_gpu_plugin.so').write_text('fixture')
elif a[0] == 'build':
 context = pathlib.Path(a[-1])
 command = (context / 'Dockerfile').read_text().split('RUN set -eu; ', 1)[1]
 command = command.replace('\\\n', '').replace('/opt/mist-onnx/lib', str(context / 'prefix/lib'))
 command = command.replace('/tmp/native-runtime.ldd', str(context / 'native-runtime.ldd'))
 sys.exit(subprocess.run(['/bin/sh', '-c', 'set -eu; ' + command]).returncode)
elif a[0] == 'run':
 if 'MistProcONNX' in a: print("This binary packages the '%s' profile" % os.environ['TEST_PROFILE'])
 elif 'installed-files.txt' in a[-1]:
  with tarfile.open(fileobj=sys.stdout.buffer, mode='w|') as t:
   data = b'fixture'; entry = tarfile.TarInfo('bin/MistProcONNX'); entry.size = len(data)
   t.addfile(entry, io.BytesIO(data))
 else: print('libc.so.6 => /lib/libc.so.6 (0x1)')
'''


class NativeBundleTest(unittest.TestCase):
  def test_native_audit_rejects_unresolved_libraries(self):
    for missing in (False, True):
      with self.subTest(missing=missing), tempfile.TemporaryDirectory() as directory:
        base = Path(directory)
        for name, body in {
            'docker': DOCKER,
            'ldd': '#!/bin/sh\necho "libfixture.so => ' + ('not found' if missing else '/lib/libfixture.so (0x1)') + '"\n',
            'dpkg-query': '#!/bin/sh\nprintf installed\n',
        }.items():
          path = base / name
          path.write_text(body)
          path.chmod(0o755)
        env = dict(os.environ, PATH=str(base) + os.pathsep + os.environ['PATH'])
        result = subprocess.run(['bash', str(ROOT / 'scripts/ONNX/audit_native_runtime.sh'), 'fixture'],
                                env=env, text=True, capture_output=True)
        self.assertEqual(result.returncode == 0, not missing, result.stdout + result.stderr)

  def test_openvino_bundle_declares_host_opencl_loader_and_driver(self):
    with tempfile.TemporaryDirectory() as directory:
      base = Path(directory)
      docker = base / 'docker'
      docker.write_text(DOCKER)
      docker.chmod(0o755)
      output = base / 'bundle.tar.gz'
      env = dict(os.environ, PATH=str(base) + os.pathsep + os.environ['PATH'], TEST_PROFILE='openvino')
      result = subprocess.run(['bash', str(ROOT / 'scripts/ONNX/package_native_bundle.sh'),
                               '--image', 'fixture', '--output', str(output), '--profile', 'openvino',
                               '--platform', 'linux/amd64', '--version', 'test', '--source-revision', 'test'],
                              env=env, text=True, capture_output=True)
      self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
      with tarfile.open(output) as archive:
        contract = json.load(archive.extractfile('share/mistserver/onnx/deployment-contract.json'))
      for package in ('ocl-icd-libopencl1', 'intel-opencl-icd'):
        self.assertIn(package, contract['host']['system_packages'])


if __name__ == '__main__':
  unittest.main()
