#!/bin/sh
set -eu

usage() {
  echo "Usage: $0 --build-dir DIR [--model MODEL.onnx] [--image IMAGE] [--allow-shared]" >&2
  exit 2
}

build_dir=
model=
image=
allow_shared=0
while [ "$#" -gt 0 ]; do
  case "$1" in
    --build-dir) [ "$#" -ge 2 ] || usage; build_dir=$2; shift 2 ;;
    --model) [ "$#" -ge 2 ] || usage; model=$2; shift 2 ;;
    --image) [ "$#" -ge 2 ] || usage; image=$2; shift 2 ;;
    --allow-shared) allow_shared=1; shift ;;
    *) usage ;;
  esac
done
[ -n "$build_dir" ] || usage

proc="$build_dir/MistProcONNX"
probe="$build_dir/test/onnxmodelprobe"
[ -x "$proc" ] || { echo "Missing $proc" >&2; exit 1; }
[ -x "$probe" ] || { echo "Missing $probe" >&2; exit 1; }

meson compile -C "$build_dir" MistProcONNX test/onnxmodelprobe \
  test/onnxtensorwiretest test/onnxresultschematest test/onnxsidecartest \
  test/onnxfbanktest test/onnxwindowertest
meson test -C "$build_dir" --suite ONNX --print-errorlogs

# Mist process binaries conventionally return -1 after printing their capability JSON.
capability=$("$proc" -j 2>/dev/null || true)
for required in \
  '"default":"yolo26n"' \
  '"default":5,"help":"Timestamp-based inference rate cap' \
  '"annotated_video":{"default":false' \
  '"enable_tracking":{"default":false' \
  '"input_mode":{"default":"auto"' \
  '"tensor_queue_depth":{"default":8' \
  '"ONNXTENSOR"'; do
  printf '%s' "$capability" | grep -F "$required" >/dev/null || {
    echo "Capability contract missing: $required" >&2
    exit 1
  }
done

if [ "$allow_shared" -eq 0 ]; then
  linkage=
  if command -v otool >/dev/null 2>&1; then
    linkage=$(find "$build_dir" -maxdepth 3 -type f \( -name 'MistProcONNX' -o -name 'libmistonnx.dylib' -o -name 'libmistonnx.so' \) \
      -exec otool -L {} \;)
  elif command -v ldd >/dev/null 2>&1; then
    linkage=$(find "$build_dir" -maxdepth 3 -type f \( -name 'MistProcONNX' -o -name 'libmistonnx.dylib' -o -name 'libmistonnx.so' \) \
      -exec ldd {} \; 2>/dev/null || true)
  else
    echo "Neither otool nor ldd is available for the static-link audit" >&2
    exit 1
  fi
  if printf '%s\n' "$linkage" | grep -E 'onnxruntime|opencv_(core|imgproc|imgcodecs|video|geometry)' >/dev/null; then
    printf '%s\n' "$linkage" >&2
    echo "Release artifact still has dynamic ONNX Runtime/OpenCV dependencies" >&2
    exit 1
  fi
fi

if [ -n "$model" ]; then
  [ -f "$model" ] || { echo "Missing model: $model" >&2; exit 1; }
  if [ -n "$image" ]; then
    [ -f "$image" ] || { echo "Missing image: $image" >&2; exit 1; }
    "$probe" "$model" 640 auto "$image"
  else
    "$probe" "$model" 640
  fi
  "$probe" --tensor "$model"
fi

echo "ONNX release checks passed"
