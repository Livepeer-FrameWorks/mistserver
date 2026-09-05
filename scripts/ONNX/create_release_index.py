#!/usr/bin/env python3
"""Build the machine-readable MistServer release variant index."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path


SCHEMA = "mistserver.release/v1"


def _text(path: Path) -> str:
  value = path.read_text(encoding="utf-8").strip()
  if not value:
    raise ValueError(f"empty metadata file: {path}")
  return value


def _required_file(root: Path, name: str) -> Path:
  matches = sorted(path for path in root.rglob(name) if path.is_file())
  if not matches:
    raise FileNotFoundError(f"release input is missing: {name} under {root}")
  if len(matches) > 1:
    locations = ", ".join(str(path) for path in matches)
    raise ValueError(f"release input is ambiguous: {name} found at {locations}")
  return matches[0]


def _sha256(path: Path) -> str:
  digest = hashlib.sha256()
  with path.open("rb") as handle:
    for block in iter(lambda: handle.read(1024 * 1024), b""):
      digest.update(block)
  return f"sha256:{digest.hexdigest()}"


def _artifact(dist: Path, name: str) -> dict[str, object]:
  path = _required_file(dist, name)
  return {
      "name": name,
      "checksum": _sha256(path),
      "size_bytes": path.stat().st_size,
  }


def _image(image: str, digest: str) -> dict[str, str]:
  if not digest.startswith("sha256:"):
    raise ValueError(f"image {image} has invalid digest {digest!r}")
  return {"ref": image, "digest": digest}


def build_index(
    dist: Path,
    matrix_path: Path,
    release_tag: str,
    source_revision: str,
    registry: str,
) -> dict[str, object]:
  matrix = json.loads(matrix_path.read_text(encoding="utf-8"))
  deployment = matrix["native_deployment"]
  tag = _text(_required_file(dist, "docker-tag.txt"))
  registry = registry.rstrip("/")

  profiles: dict[str, object] = {
      "cpu": {
          "family": "cpu",
          "cpu_fallback": True,
          "platforms": {},
      }
  }
  cpu_platforms = profiles["cpu"]["platforms"]
  for arch in ("amd64", "arm64"):
    platform = f"linux/{arch}"
    cpu_platforms[platform] = {
        "host": deployment["platforms"][platform],
        "image": _image(
            f"{registry}/mistserver:{tag}-{arch}",
            _text(_required_file(dist, f"docker-digest-{arch}.txt")),
        ),
        "artifact": _artifact(
            dist, f"mistserver-linux-{arch}-{release_tag}.tar.gz"
        ),
    }

  coreml_platform = "darwin/arm64"
  coreml_profile = deployment["profiles"]["coreml"]
  profiles["coreml"] = {
      **coreml_profile,
      "platforms": {
          coreml_platform: {
              "host": deployment["platforms"][coreml_platform],
              "artifact": _artifact(
                  dist, f"mistserver-darwin-arm64-{release_tag}.tar.gz"
              ),
          }
      },
  }

  targets = {(entry["profile"], entry["platform"]): entry for entry in matrix["include"]}
  for target in deployment["targets"]:
    profile = target["profile"]
    platform = target["platform"]
    if profile == "coreml":
      continue
    build_target = targets.get((profile, platform))
    if build_target is None:
      raise ValueError(f"missing build target for {profile}/{platform}")
    arch = deployment["platforms"][platform]["architecture"]
    stem = f"onnx-{profile}-{arch}"
    profile_entry = profiles.setdefault(
        profile,
        {**deployment["profiles"][profile], "platforms": {}},
    )
    profile_entry["platforms"][platform] = {
        "host": deployment["platforms"][platform],
        "reference_runtime_image": build_target["runtime_base"],
        "image": _image(
            _text(_required_file(dist, f"{stem}-image.txt")),
            _text(_required_file(dist, f"{stem}-digest.txt")),
        ),
        "artifact": _artifact(
            dist,
            f"mistserver-linux-{arch}-onnx-{profile}-{release_tag}.tar.gz",
        ),
    }

  return {
      "schema": SCHEMA,
      "release_tag": release_tag,
      "runtime_tag": tag,
      "source_revision": source_revision,
      "default_profile": "cpu",
      "profiles": profiles,
  }


def main() -> None:
  parser = argparse.ArgumentParser()
  parser.add_argument("--dist", type=Path, required=True)
  parser.add_argument("--matrix", type=Path, required=True)
  parser.add_argument("--release-tag", required=True)
  parser.add_argument("--source-revision", required=True)
  parser.add_argument(
      "--registry", default="ghcr.io/livepeer-frameworks", help="OCI registry namespace"
  )
  parser.add_argument("--output", type=Path, required=True)
  args = parser.parse_args()
  index = build_index(
      args.dist,
      args.matrix,
      args.release_tag,
      args.source_revision,
      args.registry,
  )
  args.output.write_text(json.dumps(index, indent=2, sort_keys=True) + "\n", encoding="utf-8")


if __name__ == "__main__":
  main()
