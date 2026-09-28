#!/usr/bin/env bash
# audit_accelerator_image.sh IMAGE PROFILE RUNTIME_DISTRIBUTION
#
# Audits a built accelerator image without a GPU: the packaged profile, every
# library each Mist and provider binary links (only the host driver's
# libcuda.so.1 may be absent), the vendor codecs, and that no build toolchain
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
      test -f /opt/mist-onnx/lib/libonnxruntime_providers_openvino.so
      ;;
  esac
  test -f /opt/mist-onnx/share/licenses/onnxruntime/ThirdPartyNotices.txt
  test -d /opt/mist-onnx/share/licenses/opencv5
  grep -F "/$PROFILE/onnxruntime-" /opt/mist-onnx/.mist-onnx-dependencies
  if [ "$RUNTIME_DISTRIBUTION" = source ]; then
    ! grep -F "/distribution-" /opt/mist-onnx/.mist-onnx-dependencies
  else
    grep -F "/distribution-$RUNTIME_DISTRIBUTION-" \
      /opt/mist-onnx/.mist-onnx-dependencies
  fi
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
