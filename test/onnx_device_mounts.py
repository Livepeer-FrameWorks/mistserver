#!/usr/bin/env python3
import json
import importlib.util
from pathlib import Path
import shlex
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class DeviceMountTest(unittest.TestCase):
  def test_openvino_includes_npu_device_directory(self):
    matrix = json.loads((ROOT / 'scripts/ONNX/accelerator-images.json').read_text())
    self.assertIn('/dev/accel', matrix['native_deployment']['profiles']['openvino']['optional_device_paths'])
    target = next(row for row in matrix['include'] if row['profile'] == 'openvino')
    self.assertIn('--device=/dev/accel', shlex.split(target['docker_args']))

  def test_runner_mounts_only_present_optional_devices(self):
    spec = importlib.util.spec_from_file_location('device_args', ROOT / 'scripts/ONNX/container_device_args.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    with tempfile.TemporaryDirectory() as directory:
      root = Path(directory)
      self.assertEqual(module.device_arguments('openvino', root), [])
      (root / 'dev/accel').mkdir(parents=True)
      self.assertEqual(module.device_arguments('openvino', root), ['--device=/dev/accel'])
      (root / 'dev/dri').mkdir()
      self.assertEqual(module.device_arguments('openvino', root), ['--device=/dev/dri', '--device=/dev/accel'])
      (root / 'dev/accel').rmdir()
      self.assertEqual(module.device_arguments('openvino', root), ['--device=/dev/dri'])
      self.assertEqual(module.device_arguments('cuda', root), ['--gpus', 'all'])
      self.assertEqual(module.device_arguments('tensorrt', root), ['--gpus', 'all'])


if __name__ == '__main__':
  unittest.main()
