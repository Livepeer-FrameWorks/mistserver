#!/usr/bin/env python3

import json
import tempfile
import unittest
from pathlib import Path

from create_release_index import SCHEMA, build_index


class ReleaseIndexTest(unittest.TestCase):
  def setUp(self):
    self.temp = tempfile.TemporaryDirectory()
    self.root = Path(self.temp.name)
    self.dist = self.root / "dist"
    self.dist.mkdir()
    self.tag = "v1.2.3"
    (self.dist / "docker-tag.txt").write_text(self.tag + "\n")
    for arch in ("amd64", "arm64"):
      (self.dist / f"docker-digest-{arch}.txt").write_text("sha256:" + arch[0] * 64)
      (self.dist / f"mistserver-linux-{arch}-{self.tag}.tar.gz").write_bytes(arch.encode())
    (self.dist / f"mistserver-darwin-arm64-{self.tag}.tar.gz").write_bytes(b"coreml")

    profiles = {}
    include = []
    targets = [{"profile": "coreml", "platform": "darwin/arm64"}]
    for profile in ("cuda", "tensorrt", "openvino"):
      profiles[profile] = {"family": profile, "cpu_fallback": True}
      targets.append({"profile": profile, "platform": "linux/amd64"})
      include.append(
          {
              "profile": profile,
              "platform": "linux/amd64",
              "runtime_base": f"vendor/{profile}:runtime@sha256:base",
          }
      )
      stem = f"onnx-{profile}-amd64"
      (self.dist / f"{stem}-image.txt").write_text(
          f"ghcr.io/example/mist:{self.tag}-onnx-{profile}-amd64\n"
      )
      (self.dist / f"{stem}-digest.txt").write_text("sha256:" + profile[0] * 64)
      (self.dist / f"mistserver-linux-amd64-onnx-{profile}-{self.tag}.tar.gz").write_bytes(
          profile.encode()
      )
    profiles["coreml"] = {"family": "apple-coreml", "cpu_fallback": True}
    platforms = {
        "linux/amd64": {"architecture": "amd64", "system_packages": ["ffmpeg"]},
        "linux/arm64": {"architecture": "arm64", "system_packages": ["ffmpeg"]},
        "darwin/arm64": {"architecture": "arm64", "system_packages": ["ffmpeg"]},
    }
    matrix = {
        "native_deployment": {
            "platforms": platforms,
            "profiles": profiles,
            "targets": targets,
        },
        "include": include,
    }
    self.matrix = self.root / "matrix.json"
    self.matrix.write_text(json.dumps(matrix))

  def tearDown(self):
    self.temp.cleanup()

  def test_builds_complete_profile_index(self):
    index = build_index(
        self.dist,
        self.matrix,
        self.tag,
        "deadbeef",
        "ghcr.io/example",
    )
    self.assertEqual(index["schema"], SCHEMA)
    self.assertEqual(index["source_revision"], "deadbeef")
    self.assertEqual(set(index["profiles"]), {"cpu", "coreml", "cuda", "tensorrt", "openvino"})
    self.assertEqual(
        index["profiles"]["cpu"]["platforms"]["linux/arm64"]["image"]["ref"],
        f"ghcr.io/example/mistserver:{self.tag}-arm64",
    )
    accelerator = index["profiles"]["tensorrt"]["platforms"]["linux/amd64"]
    self.assertIn("-onnx-tensorrt-", accelerator["artifact"]["name"])
    self.assertTrue(accelerator["artifact"]["checksum"].startswith("sha256:"))
    self.assertEqual(accelerator["host"]["system_packages"], ["ffmpeg"])

  def test_missing_declared_artifact_fails(self):
    (self.dist / f"mistserver-linux-amd64-onnx-cuda-{self.tag}.tar.gz").unlink()
    with self.assertRaises(FileNotFoundError):
      build_index(self.dist, self.matrix, self.tag, "deadbeef", "ghcr.io/example")

  def test_accepts_merged_actions_artifact_directories(self):
    metadata = self.dist / "release-metadata"
    native = self.dist / "release-native"
    metadata.mkdir()
    native.mkdir()
    for path in self.dist.glob("onnx-*-amd64-*.txt"):
      path.rename(metadata / path.name)
    for path in self.dist.glob("mistserver-linux-amd64-onnx-*.tar.gz"):
      path.rename(native / path.name)

    index = build_index(
        self.dist,
        self.matrix,
        self.tag,
        "deadbeef",
        "ghcr.io/example",
    )

    self.assertEqual(
        index["profiles"]["cuda"]["platforms"]["linux/amd64"]["artifact"]["name"],
        f"mistserver-linux-amd64-onnx-cuda-{self.tag}.tar.gz",
    )

  def test_rejects_duplicate_release_inputs(self):
    duplicate = self.dist / "release-metadata"
    duplicate.mkdir()
    (duplicate / "onnx-cuda-amd64-image.txt").write_text("duplicate\n")

    with self.assertRaisesRegex(ValueError, "release input is ambiguous"):
      build_index(self.dist, self.matrix, self.tag, "deadbeef", "ghcr.io/example")


if __name__ == "__main__":
  unittest.main()
