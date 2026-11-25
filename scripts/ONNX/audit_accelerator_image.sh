#!/usr/bin/env bash
# audit_accelerator_image.sh IMAGE PROFILE RUNTIME_DISTRIBUTION
#
# Audits a built accelerator image without a GPU: the packaged profile, every
# library each Mist and provider binary links (only the host driver's
# libcuda.so.1 may be absent) and that bundled ones load from the bundle under
# any LD_LIBRARY_PATH, the vendor codecs, and that no build toolchain
# reached the runtime layer. CI runs it after building each profile; run it
# locally against a local build before cutting a release.
set -euo pipefail
IMAGE=${1:?image}
PROFILE=${2:?profile}
RUNTIME_DISTRIBUTION=${3:?runtime distribution}
capability=$(docker run --rm --entrypoint MistProcONNX "$IMAGE" -j 2>/dev/null || true)
printf '%s' "$capability" | grep -F "This binary packages the '$PROFILE' profile"
docker run --rm -e PROFILE="$PROFILE" \
  -e RUNTIME_DISTRIBUTION="$RUNTIME_DISTRIBUTION" \
  --entrypoint /bin/sh "$IMAGE" -ec '
  lib=$(find /usr/local/lib -name libmistonnx.so -print -quit)
  test -n "$lib"
  ldd "$lib" > /tmp/libmistonnx.ldd
  ! grep -F "not found" /tmp/libmistonnx.ldd
  grep -F "libonnxruntime" /tmp/libmistonnx.ldd
  test -x /opt/mist-onnx/libexec/onnxmodelprobe
  ldd /opt/mist-onnx/libexec/onnxmodelprobe > /tmp/onnxmodelprobe.ldd
  ! grep -F "not found" /tmp/onnxmodelprobe.ldd
  for dependency in /opt/mist-onnx/lib/*.so /opt/mist-onnx/lib/*.so.*; do
    [ -e "$dependency" ] || continue
    ldd "$dependency" >> /tmp/provider-libs.ldd
  done
  if grep -F "not found" /tmp/provider-libs.ldd > /tmp/provider-libs.missing; then
    case "$PROFILE" in
      cuda|tensorrt)
        grep -Ev "^[[:space:]]*libcuda\.so\.1 => not found$" \
          /tmp/provider-libs.missing > /tmp/provider-libs.unexpected || true
        if [ -s /tmp/provider-libs.unexpected ]; then
          cat /tmp/provider-libs.unexpected >&2
          exit 1
        fi
        ;;
      *) cat /tmp/provider-libs.missing >&2; exit 1 ;;
    esac
  fi
  # Every library the bundle ships loads from the bundle whatever LD_LIBRARY_PATH holds: the
  # image environment (the OpenVINO base points it at /opt/intel/openvino), and a decoy
  # directory with a copy of every bundled library in front of it, as a host that exports
  # another OpenVINO, ONNX Runtime or OpenCV would. The bundled libraries and the Mist
  # objects linking them carry DT_RPATH, which the loader searches before LD_LIBRARY_PATH.
  resolve_bundle() {
    for object in "$lib" /opt/mist-onnx/libexec/onnxmodelprobe /usr/local/bin/Mist* \
                  /opt/mist-onnx/lib/*.so /opt/mist-onnx/lib/*.so.*; do
      [ -e "$object" ] || continue
      ldd "$object" 2>/dev/null
    done | sed -n "s/^[[:space:]]*\([^[:space:]]*\) => \(\/[^[:space:]]*\) .*/\1 \2/p" | sort -u > /tmp/resolved.txt
    grep -q " /opt/mist-onnx/lib/" /tmp/resolved.txt
    while read -r name resolved; do
      if [ -e "/opt/mist-onnx/lib/$name" ] && [ "$(readlink -f "${resolved%/*}")" != /opt/mist-onnx/lib ]; then
        echo "bundled $name resolves to $resolved (LD_LIBRARY_PATH=${LD_LIBRARY_PATH:-})" >&2
        exit 1
      fi
    done < /tmp/resolved.txt
  }
  mkdir /tmp/decoy-libs
  cp -L /opt/mist-onnx/lib/*.so* /tmp/decoy-libs/
  (
    LD_LIBRARY_PATH=/tmp/decoy-libs${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
    export LD_LIBRARY_PATH
    resolve_bundle
  )
  rm -rf /tmp/decoy-libs
  resolve_bundle
  case "$PROFILE" in
    cuda)
      test -f /opt/mist-onnx/lib/libonnxruntime_providers_cuda.so
      test ! -e /opt/mist-onnx/lib/libonnxruntime_providers_tensorrt.so
      ffmpeg -hide_banner -encoders 2>/dev/null | grep -F h264_nvenc
      ffmpeg -hide_banner -encoders 2>/dev/null | grep -F av1_nvenc
      ffmpeg -hide_banner -decoders 2>/dev/null | grep -F h264_cuvid
      ffmpeg -hide_banner -decoders 2>/dev/null | grep -F av1_cuvid
      ;;
    tensorrt)
      test -f /opt/mist-onnx/lib/libonnxruntime_providers_cuda.so
      test -f /opt/mist-onnx/lib/libonnxruntime_providers_tensorrt.so
      ffmpeg -hide_banner -encoders 2>/dev/null | grep -F h264_nvenc
      ffmpeg -hide_banner -encoders 2>/dev/null | grep -F av1_nvenc
      ffmpeg -hide_banner -decoders 2>/dev/null | grep -F h264_cuvid
      ffmpeg -hide_banner -decoders 2>/dev/null | grep -F av1_cuvid
      ;;
    openvino)
      test -f /opt/mist-onnx/lib/libonnxruntime_providers_openvino_plugin.so.1
      test -L /opt/mist-onnx/lib/libonnxruntime_providers_openvino_plugin.so
      ls /opt/mist-onnx/lib/libopenvino.so.* >/dev/null
      grep -E "^libopenvino\.so\.[0-9]+ /opt/mist-onnx/lib/" /tmp/resolved.txt
      test -f /opt/mist-onnx/lib/libopenvino_intel_cpu_plugin.so
      test -f /opt/mist-onnx/lib/libopenvino_intel_gpu_plugin.so
      test -f /opt/mist-onnx/lib/libopenvino_intel_npu_plugin.so
      test -f /opt/mist-onnx/share/licenses/onnxruntime-openvino-ep/third-party-programs.txt
      grep -F "/plugin-onnxruntime-openvino-ep-linux-x64-" /opt/mist-onnx/.mist-onnx-dependencies
      ;;
  esac
  test -f /opt/mist-onnx/share/licenses/onnxruntime/ThirdPartyNotices.txt
  test -d /opt/mist-onnx/share/licenses/opencv5
  grep -F "/$PROFILE/onnxruntime-" /opt/mist-onnx/.mist-onnx-dependencies
  grep -F "/distribution-$RUNTIME_DISTRIBUTION-" /opt/mist-onnx/.mist-onnx-dependencies
  test -f /opt/mist-onnx/share/mistserver/onnx/dependencies.lock.tsv
  test -f /opt/mist-onnx/share/mistserver/onnx/models.manifest.tsv
  # The runtime layer carries only what MistServer installed, not the build toolchain.
  test -s /usr/local/share/mistserver/installed-files.txt
  test ! -e /usr/local/cuda/bin/nvcc
  test ! -d /opt/mist-onnx/include
  ! grep -E "\.a$|/python3|/include/" /usr/local/share/mistserver/installed-files.txt
  ! find /opt/mist-onnx -name "*.a" | grep -q .
'

# NVIDIA profiles encode with NVENC, which the container toolkit only mounts
# when the image asks for the driver's video libraries.
case "$PROFILE" in
  cuda|tensorrt)
    capabilities=$(docker image inspect --format '{{range .Config.Env}}{{println .}}{{end}}' "$IMAGE" |
      sed -n 's/^NVIDIA_DRIVER_CAPABILITIES=//p')
    case ",$capabilities," in
      *,video,*|*,all,*) ;;
      *) echo "NVIDIA_DRIVER_CAPABILITIES=$capabilities lacks video" >&2; exit 1 ;;
    esac
    ;;
esac
