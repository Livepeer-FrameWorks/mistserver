#!/bin/sh
set -eu

usage() {
  echo "Usage: $0 --build-dir DIR [--expected-profile PROFILE] [--provider PROVIDER] [--model MODEL.onnx] [--image IMAGE]" >&2
  exit 2
}

build_dir=
model=
image=
expected_profile=
provider=cpu
while [ "$#" -gt 0 ]; do
  case "$1" in
    --build-dir) [ "$#" -ge 2 ] || usage; build_dir=$2; shift 2 ;;
    --model) [ "$#" -ge 2 ] || usage; model=$2; shift 2 ;;
    --image) [ "$#" -ge 2 ] || usage; image=$2; shift 2 ;;
    --expected-profile) [ "$#" -ge 2 ] || usage; expected_profile=$2; shift 2 ;;
    --provider) [ "$#" -ge 2 ] || usage; provider=$2; shift 2 ;;
    *) usage ;;
  esac
done
[ -n "$build_dir" ] || usage
case "$provider" in
  cpu|coreml|cuda|tensorrt|openvino) ;;
  *) echo "Unsupported test provider: $provider" >&2; exit 2 ;;
esac

proc="$build_dir/MistProcONNX"
probe="$build_dir/test/onnxmodelprobe"
[ -x "$proc" ] || { echo "Missing $proc" >&2; exit 1; }
[ -x "$probe" ] || { echo "Missing $probe" >&2; exit 1; }

meson compile -C "$build_dir" MistProcONNX test/onnxmodelprobe \
  test/onnxtensorwiretest test/onnxresultschematest test/onnxsidecartest \
  test/onnxfbanktest test/onnxwindowertest
meson test -C "$build_dir" --suite ONNX --print-errorlogs

# The runtime the loader resolves must be the locked one, not a stale prefix or system copy.
lock_file="$(dirname "$0")/dependencies.lock.tsv"
locked_runtime=$(awk -F '\t' '$1 == "onnxruntime" { print $2; exit }' "$lock_file")
[ -n "$locked_runtime" ] || { echo "No onnxruntime entry in $lock_file" >&2; exit 1; }
loaded_runtime=$("$probe" --runtime-version)
[ "$loaded_runtime" = "$locked_runtime" ] || {
  echo "Loaded ONNX Runtime $loaded_runtime differs from locked $locked_runtime" >&2
  exit 1
}

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

if [ -n "$expected_profile" ]; then
  printf '%s' "$capability" | grep -F "This binary packages the '$expected_profile' profile" >/dev/null || {
    echo "Capability does not report expected ONNX profile: $expected_profile" >&2
    exit 1
  }
fi

# ONNX Runtime and OpenCV are shared libraries; every one the ONNX binaries need must resolve.
for binary in "$proc" "$probe"; do
  if command -v otool >/dev/null 2>&1; then
    # otool lists direct dependencies only; a shared libmistonnx carries the runtime link.
    linkage=$(otool -L "$binary"
      find "$build_dir" -maxdepth 3 -name 'libmistonnx*.dylib' -exec otool -L {} \;)
    printf '%s\n' "$linkage" | grep -F libonnxruntime >/dev/null || {
      printf '%s\n' "$linkage" >&2
      echo "$binary does not link the shared ONNX Runtime" >&2
      exit 1
    }
  else
    linkage=$(ldd "$binary")
    if printf '%s\n' "$linkage" | grep -E 'onnxruntime|opencv_' | grep -F 'not found' >&2; then
      echo "$binary has unresolved ONNX Runtime/OpenCV libraries" >&2
      exit 1
    fi
    printf '%s\n' "$linkage" | grep -F 'libonnxruntime.so' >/dev/null || {
      printf '%s\n' "$linkage" >&2
      echo "$binary does not link the shared ONNX Runtime" >&2
      exit 1
    }
  fi
done

if [ -n "$model" ]; then
  [ -f "$model" ] || { echo "Missing model: $model" >&2; exit 1; }
  if [ -n "$image" ]; then
    [ -f "$image" ] || { echo "Missing image: $image" >&2; exit 1; }
    MIST_ONNX_TEST_PROVIDER="$provider" "$probe" "$model" 640 auto "$image"
  else
    MIST_ONNX_TEST_PROVIDER="$provider" "$probe" "$model" 640
  fi
  MIST_ONNX_TEST_PROVIDER="$provider" "$probe" --tensor "$model"

  if [ -n "$expected_profile" ]; then
    rejected_provider=cuda
    case "$expected_profile" in
      cuda|tensorrt) rejected_provider=coreml ;;
    esac
    rejection_log="$build_dir/meson-logs/onnx-provider-rejection.log"
    if MIST_ONNX_TEST_PROVIDER="$rejected_provider" "$probe" "$model" 640 \
         >"$rejection_log" 2>&1; then
      echo "Unpackaged provider '$rejected_provider' was accepted by '$expected_profile' profile" >&2
      exit 1
    fi
    grep -F "not included in this '$expected_profile' ONNX build profile" "$rejection_log" >/dev/null || {
      cat "$rejection_log" >&2
      echo "Unpackaged provider rejection was not explicit" >&2
      exit 1
    }
  fi
fi

echo "ONNX release checks passed"
