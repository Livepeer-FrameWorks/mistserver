#!/usr/bin/env bash
# isolate_native_libraries.sh STAGE [ONNX_PREFIX]
#
# Gives every ELF object of a MistServer tree DT_RPATH entries to its own libraries, so
# nothing needs the host loader cache and LD_LIBRARY_PATH cannot replace them. STAGE holds
# bin/ and lib/ (a native bundle root, or the image's /usr/local tree); ONNX_PREFIX is the
# ONNX prefix, by default STAGE/opt/mist-onnx as in a native bundle. The relative entries
# serve an extracted bundle, the absolute ones the installed and container layouts.
set -euo pipefail
stage=${1:?native bundle directory}
prefix=${2:-$stage/opt/mist-onnx}

set_rpath() {
  local file=$1 rpath=$2
  # NSS verifies these providers against their adjacent .chk files before loading.
  # They inherit the executable's DT_RPATH and must remain byte-for-byte intact.
  case "${file##*/}" in libsoftokn3.so|libfreeblpriv3.so|libfreebl3.so) return ;; esac
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
if [ -d "$prefix/lib" ]; then
  while IFS= read -r -d '' file; do
    set_rpath "$file" "\$ORIGIN"
  done < <(find "$prefix/lib" -type f -print0)
fi
# The ONNX probe links libmist, which sits three levels up in a bundle and in /usr/local once
# installed or in the image.
if [ -d "$prefix/libexec" ]; then
  while IFS= read -r -d '' file; do
    set_rpath "$file" "\$ORIGIN/../lib:\$ORIGIN/../../../lib/x86_64-linux-gnu:\$ORIGIN/../../../lib/aarch64-linux-gnu:/opt/mist-onnx/lib:/usr/local/lib/x86_64-linux-gnu:/usr/local/lib/aarch64-linux-gnu"
  done < <(find "$prefix/libexec" -type f -print0)
fi
