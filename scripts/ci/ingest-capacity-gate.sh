#!/usr/bin/env bash
# Run the ONNX-enabled lifecycle and capacity contract in the CI toolchain.
set -euo pipefail
cd "$(dirname "$0")/../.."
mode=${1:-full}
build_dir=${MIST_GATE_BUILD_DIR:-build}
case "$mode" in
  build)
    if [ -f "$build_dir/meson-info/intro-buildoptions.json" ]; then
      meson setup --reconfigure "$build_dir" -DWITH_AV=true -DONNX=enabled -DONNX_PROFILE=cpu -DDEBUG=4 -Db_coverage=true
    else
      meson setup "$build_dir" -DWITH_AV=true -DONNX=enabled -DONNX_PROFILE=cpu -DDEBUG=4 -Db_coverage=true
    fi
    ninja -C "$build_dir" -j "${MIST_GATE_JOBS:-6}"
    ;;
  regression)
    regressions=(
      'Controller capacity lifecycle webrtc-idle'
      'Controller capacity lifecycle webrtc-reload'
      'Controller capacity lifecycle whep'
      'Controller capacity lifecycle whep-idle-timer'
      'Controller capacity lifecycle bandwidth'
      'Controller capacity lifecycle progressive-mp4'
      'Controller statistics preserve collection semantics'
      'Numeric bandwidth exceptions work without configured address families'
    )
    meson test -C "$build_dir" "${regressions[@]}" --print-errorlogs --verbose
    ;;
  full)
    python3 scripts/ci/check_test_inventory.py "$build_dir"
    test_status=0
    MIST_RUN_MEDIA_TESTS=1 meson test -C "$build_dir" --print-errorlogs --verbose --num-processes "${MIST_GATE_JOBS:-6}" || test_status=$?
    python3 scripts/ci/check_test_inventory.py "$build_dir" --results
    exit "$test_status"
    ;;
  loop)
    MIST_RUN_MEDIA_TESTS=1 meson test -C "$build_dir" --suite Processing --repeat "${MIST_GATE_LOOPS:-20}" --print-errorlogs --num-processes "${MIST_GATE_JOBS:-6}"
    ;;
  deps-image|image|image-builder)
    matrix=scripts/ONNX/accelerator-images.json
    entry=$(jq -ce '.dependency_images.cpu[] | select(.arch=="amd64")' "$matrix")
    runtime_distribution=${MIST_GATE_RUNTIME_DISTRIBUTION:-$(jq -r .runtime_distribution <<<"$entry")}
    dependency_image=${MIST_GATE_DEPENDENCY_IMAGE:-"frameworks-mist-ingest-deps:$(scripts/ONNX/dependency_image.sh tag cpu amd64)-$runtime_distribution"}
    if [ -n "${MIST_GATE_DEPENDENCY_IMAGE:-}" ]; then
      docker image inspect "$dependency_image" >/dev/null
    elif ! docker image inspect "$dependency_image" >/dev/null 2>&1; then
      docker build --platform linux/amd64 --file Dockerfile.onnx-deps --tag "$dependency_image" \
        --build-arg BUILD_BASE="$(jq -r .build_base <<<"$entry")" \
        --build-arg ONNX_PROFILE=cpu --build-arg ONNX_DEP_JOBS="${MIST_GATE_JOBS:-4}" \
        --build-arg ONNX_RUNTIME_DISTRIBUTION="$runtime_distribution" .
    fi
    if [ "$mode" != deps-image ]; then
      image=${MIST_GATE_IMAGE:-frameworks-mist-ingest:v316}
      target_args=()
      if [ "$mode" = image-builder ]; then target_args=(--target mist_build); fi
      docker build --platform linux/amd64 "${target_args[@]}" --file Dockerfile.mistserver-onnx --tag "$image" \
        --build-arg ONNX_DEPS_IMAGE="$dependency_image" \
        --build-arg BUILD_BASE="$(jq -r .build_base <<<"$entry")" \
        --build-arg RUNTIME_BASE="$(jq -r .build_base <<<"$entry")" \
        --build-arg ONNX_PROFILE=cpu \
        --build-arg MIST_BUILD_JOBS="${MIST_GATE_JOBS:-4}" \
        --build-arg DEBUG=4 \
        --build-arg ONNX_RUNTIME_DISTRIBUTION="$runtime_distribution" \
        --build-arg ONNX_INSTALL_TEST_PROBE=true \
        --build-arg VERSION="${MIST_GATE_VERSION:-v0.3.16}" .
      if [ "$mode" = image ]; then
        scripts/ONNX/audit_accelerator_image.sh "$image" cpu "$runtime_distribution"
      fi
    fi
    ;;
  *) echo "usage: $0 build|regression|full|loop|deps-image|image|image-builder" >&2; exit 2 ;;
esac
