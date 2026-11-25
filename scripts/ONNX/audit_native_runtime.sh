#!/usr/bin/env bash
# Check OpenVINO's shipped libraries on the declared bare-host baseline, without the
# vendor runtime image's implicit OpenCL packages or loader environment.
set -euo pipefail
image=${1:?image carrying /opt/mist-onnx}
platform=${2:-linux/amd64}
root=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
matrix="$root/scripts/ONNX/accelerator-images.json"
base=$(jq -er --arg platform "$platform" \
  '.dependency_images.cpu[] | select(.platform == $platform) | .build_base' "$matrix")
packages=$(jq -er --arg platform "$platform" \
  '(.native_deployment.platforms[$platform].system_packages +
    .native_deployment.profiles.openvino.system_packages) | unique | join(" ")' "$matrix")
context=$(mktemp -d)
container=
cleanup() {
  if [ -n "$container" ]; then docker rm "$container" >/dev/null; fi
  rm -rf "$context"
}
trap cleanup EXIT
container=$(docker create --entrypoint /bin/true "$image")
docker cp "$container:/opt/mist-onnx" "$context/prefix"
cat > "$context/Dockerfile" <<'EOF'
ARG HOST_BASE
FROM ${HOST_BASE}
ARG HOST_PACKAGES
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends $HOST_PACKAGES \
    && rm -rf /var/lib/apt/lists/*
COPY prefix/lib /opt/mist-onnx/lib
RUN set -eu; \
    test -f /opt/mist-onnx/lib/libopenvino_intel_gpu_plugin.so; \
    for library in /opt/mist-onnx/lib/*.so*; do \
      ldd "$library" >> /tmp/native-runtime.ldd; \
    done; \
    cat /tmp/native-runtime.ldd; \
    if grep -F 'not found' /tmp/native-runtime.ldd; then exit 1; fi; \
    test "$(dpkg-query -W -f='${db:Status-Status}' ocl-icd-libopencl1)" = installed; \
    test "$(dpkg-query -W -f='${db:Status-Status}' intel-opencl-icd)" = installed
EOF
docker build --progress=plain --platform "$platform" --build-arg HOST_BASE="$base" \
  --build-arg HOST_PACKAGES="$packages" "$context"
