#!/usr/bin/env python3
"""Exercise the dependency wrapper's source path without building vendor SDKs."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SHIM = r'''#!/usr/bin/env python3
import json, os, pathlib, sys
p = pathlib.Path
name = p(sys.argv[0]).name
a = sys.argv[1:]
with open(os.environ['TRACE'], 'a') as f: f.write(json.dumps([name] + a) + '\n')
if name == 'uname':
 print(os.environ['TEST_PLATFORM'] if a == ['-s'] else os.environ['TEST_ARCH'])
elif name == 'git':
 if a[0] == 'clone':
  dest = p(a[-1]); (dest / '.git').mkdir(parents=True)
  (dest / 'LICENSE').write_text('test license')
  (dest / 'ThirdPartyNotices.txt').write_text('test notices')
  (dest / 'build.sh').symlink_to(p(sys.argv[0]).resolve())
 elif 'checkout' in a: (p(a[1]) / '.revision').write_text(a[-1])
 elif 'rev-parse' in a: print((p(a[1]) / '.revision').read_text())
elif name == 'build.sh':
 pass
elif name == 'cmake' and a[0] == '--install':
 prefix = p(os.environ['PREFIX']); (prefix / 'lib/pkgconfig').mkdir(parents=True, exist_ok=True)
 (prefix / 'include/onnxruntime').mkdir(parents=True, exist_ok=True)
 (prefix / 'include/onnxruntime/onnxruntime_c_api.h').write_text('header')
elif name == 'c++':
 output = p(a[a.index('-o') + 1])
 output.write_text('#!/bin/sh\nprintf "%s\\n" CPUExecutionProvider CoreMLExecutionProvider CUDAExecutionProvider TensorrtExecutionProvider OpenVINOExecutionProvider\n')
 output.chmod(0o755)
'''
# Host tools build_dependencies.sh checks for and runs for real; the build tools are faked.
REQUIRED_TOOLS = ('bash', 'awk', 'curl', 'unzip')


def setUpModule():
  missing = [tool for tool in REQUIRED_TOOLS if not shutil.which(tool)]
  if not shutil.which('sha256sum') and not shutil.which('shasum'):
    missing.append('sha256sum')
  if missing:
    raise AssertionError('missing tool ' + ', '.join(missing) +
                         ': install it in every environment that runs the ONNX test suite')


class SourceBuildTest(unittest.TestCase):
  def run_build(self, platform, arch, profile, extra=None):
    with tempfile.TemporaryDirectory() as directory:
      base = Path(directory)
      binaries = base / 'bin'
      binaries.mkdir()
      shim = binaries / 'shim'
      shim.write_text(SHIM)
      shim.chmod(0o755)
      for name in ('uname', 'git', 'cmake', 'pkg-config', 'c++', 'patchelf', 'readelf', 'otool'):
        (binaries / name).symlink_to(shim)
      sdk = base / 'sdk'
      sdk.mkdir()
      (sdk / 'OpenVINOConfig.cmake').write_text('')
      prefix = base / 'prefix'
      env = dict(os.environ, PATH=str(binaries) + os.pathsep + os.environ['PATH'],
                 CXX=str(binaries / 'c++'), TRACE=str(base / 'trace'), PREFIX=str(prefix),
                 TEST_PLATFORM=platform, TEST_ARCH=arch, CUDA_HOME=str(sdk), CUDNN_HOME=str(sdk),
                 TENSORRT_HOME=str(sdk), OpenVINO_DIR=str(sdk), ONNX_CUDA_ARCHITECTURES='87;90')
      command = ['bash', str(ROOT / 'scripts/ONNX/build_dependencies.sh'), '--prefix', str(prefix),
                 '--work-dir', str(base / 'work'), '--profile', profile,
                 '--runtime-distribution', 'source', '--jobs', '2']
      result = subprocess.run(command + (extra or []), env=env, text=True, capture_output=True)
      self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
      trace = [json.loads(line) for line in (base / 'trace').read_text().splitlines()]
      builds = [row[1:] for row in trace if row[0] == 'build.sh']
      self.assertEqual(len(builds), 1, trace)
      self.assertIn('--build_shared_lib', builds[0])
      self.assertIn('onnxruntime_BUILD_SHARED_LIB=ON', builds[0])
      self.assertTrue((prefix / '.mist-onnx-dependencies').is_file())
      verified = subprocess.run(command + (extra or []) + ['--verify-only'], env=env, text=True, capture_output=True)
      self.assertEqual(verified.returncode, 0, verified.stdout + verified.stderr)
      if profile in ('cuda', 'tensorrt'):
        changed = dict(env, ONNX_CUDA_ARCHITECTURES='75')
        mismatch = subprocess.run(command + (extra or []) + ['--verify-only'], env=changed,
                                  text=True, capture_output=True)
        self.assertNotEqual(mismatch.returncode, 0)
        self.assertIn('different build', mismatch.stderr)
      return builds[0], (prefix / '.mist-onnx-dependencies').read_text()

  def test_linux_aarch64_accelerators(self):
    for profile in ('cuda', 'tensorrt', 'openvino'):
      with self.subTest(profile=profile):
        args, stamp = self.run_build('Linux', 'aarch64', profile)
        self.assertIn('--use_' + profile, args)
        if profile in ('cuda', 'tensorrt'):
          self.assertIn('CMAKE_CUDA_ARCHITECTURES=87;90', args)
          self.assertIn('87;90', stamp)

  def test_darwin_x86_64_coreml(self):
    args, _ = self.run_build('Darwin', 'x86_64', 'coreml')
    self.assertIn('--use_coreml', args)

  def test_custom_cuda_architectures_and_nvcc_threads(self):
    args, _ = self.run_build('Linux', 'x86_64', 'cuda', ['--nvcc-threads', '3'])
    self.assertIn('CMAKE_CUDA_ARCHITECTURES=87;90', args)
    self.assertIn('onnxruntime_NVCC_THREADS=3', args)


if __name__ == '__main__':
  unittest.main()
