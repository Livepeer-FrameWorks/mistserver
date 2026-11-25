#!/usr/bin/env bash
set -euo pipefail
stage=${1:?native bundle directory}

set_rpath() {
  local file=$1 rpath=$2
  # Native bundles also contain scripts, metadata, and platform-independent files.
  if [ "$(LC_ALL=C head -c 4 "$file")" != "$(printf '\177ELF')" ]; then return; fi
  command -v patchelf >/dev/null || { echo 'patchelf is required for native ELF bundles' >&2; exit 1; }
  patchelf --force-rpath --set-rpath "$rpath" "$file"
}

if [ -d "$stage/bin" ]; then
  while IFS= read -r -d '' file; do
    set_rpath "$file" "\$ORIGIN/../lib:\$ORIGIN/../lib/x86_64-linux-gnu:\$ORIGIN/../lib/aarch64-linux-gnu:\$ORIGIN/../opt/mist-onnx/lib:/opt/mist-onnx/lib"
  done < <(find "$stage/bin" -type f -print0)
fi
if [ -d "$stage/lib" ]; then
  while IFS= read -r -d '' file; do
    set_rpath "$file" "\$ORIGIN:\$ORIGIN/..:\$ORIGIN/../opt/mist-onnx/lib:/opt/mist-onnx/lib"
  done < <(find "$stage/lib" -type f -print0)
fi
if [ -d "$stage/opt/mist-onnx/lib" ]; then
  while IFS= read -r -d '' file; do
    set_rpath "$file" "\$ORIGIN"
  done < <(find "$stage/opt/mist-onnx/lib" -type f -print0)
fi
