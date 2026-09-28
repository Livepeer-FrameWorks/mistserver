#!/usr/bin/env bash
# build_accelerator_local.sh PROFILE [OUTPUT_DIR]
#
# Builds one accelerator profile the way onnx-accelerators.yml does (same
# matrix entry and build arguments), then runs the same image audit and native
# bundle packaging. Needs Docker, not a GPU. Run it for every profile before
# tagging a release.
set -euo pipefail
profile=${1:?usage: build_accelerator_local.sh PROFILE [OUTPUT_DIR]}
output_dir=${2:-build-accelerators}
root=$(cd "$(dirname "$0")/../.." && pwd)
matrix="$root/scripts/ONNX/accelerator-images.json"

entry=$(jq -ce --arg profile "$profile" 'first(.include[] | select(.profile == $profile))' "$matrix") || {
  echo "No accelerator matrix entry for profile $profile" >&2
  exit 2
}
field() { jq -r --arg key "$1" '.[$key] // ""' <<<"$entry"; }

version=$(git -C "$root" describe --tags --always --dirty)
revision=$(git -C "$root" rev-parse HEAD)
image="mistserver-local:$version-onnx-$profile-$(field arch)"

docker buildx build --load \
  --platform "$(field platform)" \
  --file "$root/Dockerfile.mistserver-onnx" \
  --tag "$image" \
  --build-arg BUILD_BASE="$(field build_base)" \
  --build-arg RUNTIME_BASE="$(field runtime_base)" \
  --build-arg RUNTIME_PACKAGES="$(field runtime_packages)" \
  --build-arg NVIDIA_DRIVER_CAPABILITIES="$(field nvidia_driver_capabilities)" \
  --build-arg ONNX_PROFILE="$profile" \
  --build-arg ONNX_STATIC=false \
  --build-arg ONNX_RUN_PROVIDER_TEST=false \
  --build-arg ONNX_INSTALL_TEST_PROBE=true \
  --build-arg ONNX_DEP_JOBS="$(field dependency_jobs)" \
  --build-arg ONNX_RUNTIME_DISTRIBUTION="$(field runtime_distribution)" \
  --build-arg ONNX_NVCC_THREADS="$(field nvcc_threads)" \
  --build-arg ONNX_CUDA_ARCHITECTURES="$(field cuda_architectures)" \
  --build-arg CUDA_HOME="$(field cuda_home)" \
  --build-arg CUDNN_HOME="$(field cudnn_home)" \
  --build-arg TENSORRT_HOME="$(field tensorrt_home)" \
  --build-arg OpenVINO_DIR="$(field openvino_dir)" \
  --build-arg VERSION="$version" \
  --build-arg RELEASE="$(field platform)_$profile" \
  "$root"

"$root/scripts/ONNX/audit_accelerator_image.sh" "$image" "$profile" "$(field runtime_distribution)"

mkdir -p "$output_dir"
bundle="$output_dir/mistserver-linux-$(field arch)-onnx-$profile-$version.tar.gz"
"$root/scripts/ONNX/package_native_bundle.sh" \
  --image "$image" --output "$bundle" \
  --profile "$profile" --platform "$(field platform)" \
  --version "$version" --source-revision "$revision"

docker image inspect --format '{{.Size}}' "$image" |
  awk -v image="$image" '{printf "%s: %.2f GB uncompressed\n", image, $1 / 1e9}'
ls -l "$bundle"
