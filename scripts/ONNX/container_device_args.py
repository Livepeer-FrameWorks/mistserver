#!/usr/bin/env python3
"""Resolve optional device mounts for a hardware runner's actual devices."""
import argparse
import json
from pathlib import Path
import shlex

MATRIX = Path(__file__).resolve().with_name('accelerator-images.json')


def device_arguments(profile, device_root=Path('/')):
  matrix = json.loads(MATRIX.read_text())
  target = next(row for row in matrix['include'] if row['profile'] == profile)
  optional = matrix['native_deployment']['profiles'][profile].get('optional_device_paths', [])
  arguments = []
  for argument in shlex.split(target['docker_args']):
    if argument.startswith('--device='):
      path = argument.split('=', 1)[1].split(':', 1)[0]
      if path in optional and not (device_root / path.lstrip('/')).exists():
        continue
    arguments.append(argument)
  return arguments


if __name__ == '__main__':
  parser = argparse.ArgumentParser()
  parser.add_argument('profile')
  args = parser.parse_args()
  for argument in device_arguments(args.profile):
    print(argument)
