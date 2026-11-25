#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat >&2 <<'EOF'
Usage: build_dependencies.sh --prefix DIR --work-dir DIR [options]

Installs a locked ONNX Runtime NuGet distribution or builds the locked source,
and builds the minimal shared OpenCV dependency set used by MistProcONNX.

Options:
  --profile PROFILE   cpu, coreml, cuda, tensorrt, or openvino (default: cpu)
  --runtime-distribution NAME
                      Locked ONNX Runtime NuGet distribution, or source (default)
  --nvcc-threads COUNT Threads per NVCC compilation (default: 1)
  --jobs COUNT        Parallel build jobs (default: detected CPU count)
  --lock FILE         Dependency lock file (default: dependencies.lock.tsv)
  --verify-only       Only check that the prefix already holds this exact locked build

The openvino profile also installs the locked Intel OpenVINO plugin execution
provider next to a packaged runtime. Source builds use the compiled-in EP.

Source SDK environment: CUDA_HOME, CUDNN_HOME and ONNX_CUDA_ARCHITECTURES for
cuda/tensorrt; TENSORRT_HOME for tensorrt; OpenVINO_DIR for openvino.
EOF
  exit 2
}

script_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
lock_file="$script_dir/dependencies.lock.tsv"
prefix=
work_dir=
profile=cpu
jobs=
runtime_distribution=source
nvcc_threads=1
verify_only=0

while [ "$#" -gt 0 ]; do
  case "$1" in
    --prefix) [ "$#" -ge 2 ] || usage; prefix=$2; shift 2 ;;
    --work-dir) [ "$#" -ge 2 ] || usage; work_dir=$2; shift 2 ;;
    --profile) [ "$#" -ge 2 ] || usage; profile=$2; shift 2 ;;
    --jobs) [ "$#" -ge 2 ] || usage; jobs=$2; shift 2 ;;
    --lock) [ "$#" -ge 2 ] || usage; lock_file=$2; shift 2 ;;
    --runtime-distribution) [ "$#" -ge 2 ] || usage; runtime_distribution=$2; shift 2 ;;
    --nvcc-threads) [ "$#" -ge 2 ] || usage; nvcc_threads=$2; shift 2 ;;
    --verify-only) verify_only=1; shift ;;
    -h|--help) usage ;;
    *) usage ;;
  esac
done

[ -n "$prefix" ] || usage
[ -n "$work_dir" ] || usage
[ -n "$runtime_distribution" ] || usage
case "$profile" in
  cpu|coreml|cuda|tensorrt|openvino) ;;
  *) echo "Unsupported ONNX profile: $profile" >&2; exit 2 ;;
esac

platform=$(uname -s)
architecture=$(uname -m)
for command_name in awk cmake git pkg-config; do
  command -v "$command_name" >/dev/null 2>&1 || {
    echo "Required command not found: $command_name" >&2
    exit 1
  }
done
# --verify-only reads the prefix stamp only, so it runs in images without the fetch tools.
if [ "$verify_only" -eq 0 ]; then
  elf_tools="readelf patchelf"
  if [ "$platform" = Darwin ]; then elf_tools=otool; fi
  # shellcheck disable=SC2086
  for command_name in curl unzip $elf_tools; do
    command -v "$command_name" >/dev/null 2>&1 || {
      echo "Required command not found: $command_name" >&2
      exit 1
    }
  done
  if ! command -v sha256sum >/dev/null 2>&1 && ! command -v shasum >/dev/null 2>&1; then
    echo "sha256sum or shasum is required" >&2
    exit 1
  fi
fi

if [ -z "$jobs" ]; then
  jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 2)
fi
case "$jobs" in
  ''|*[!0-9]*) echo "Invalid job count: $jobs" >&2; exit 2 ;;
esac
[ "$jobs" -gt 0 ] || { echo "Job count must be positive" >&2; exit 2; }
case "$nvcc_threads" in
  ''|*[!0-9]*|0) echo "Invalid NVCC thread count: $nvcc_threads" >&2; exit 2 ;;
esac

locked_field() {
  dependency=$1
  field=$2
  awk -F '\t' -v dependency="$dependency" -v field="$field" \
    '$1 == dependency { print $field; found = 1; exit } END { if (!found) exit 1 }' "$lock_file"
}

# The onnxruntime row pins both source builds and the release behind packaged distributions.
ort_version=$(locked_field onnxruntime 2)
ort_commit=$(locked_field onnxruntime 3)
ort_source=$(locked_field onnxruntime 5)
opencv_version=$(locked_field opencv 2)
opencv_commit=$(locked_field opencv 3)
opencv_source=$(locked_field opencv 5)

plugin_distribution=
if [ "$runtime_distribution" = source ]; then
  runtime_version=$ort_version
  runtime_sha256=$ort_commit
  if [ "$profile" = coreml ] && [ "$platform" != Darwin ]; then
    echo "The CoreML profile can only be built on Darwin" >&2
    exit 1
  fi
  if [ "$profile" = cuda ] || [ "$profile" = tensorrt ]; then
    : "${CUDA_HOME:?CUDA_HOME is required for the $profile profile}"
    : "${CUDNN_HOME:?CUDNN_HOME is required for the $profile profile}"
    : "${ONNX_CUDA_ARCHITECTURES:?ONNX_CUDA_ARCHITECTURES is required for headless builds}"
    [ -d "$CUDA_HOME" ] && [ -d "$CUDNN_HOME" ] || {
      echo "CUDA_HOME and CUDNN_HOME must be SDK directories" >&2; exit 1;
    }
  fi
  if [ "$profile" = tensorrt ]; then
    : "${TENSORRT_HOME:?TENSORRT_HOME is required for the tensorrt profile}"
    [ -d "$TENSORRT_HOME" ] || { echo "TENSORRT_HOME is not a directory" >&2; exit 1; }
  fi
  if [ "$profile" = openvino ]; then
    : "${OpenVINO_DIR:?OpenVINO_DIR is required for the openvino profile}"
    [ -f "$OpenVINO_DIR/OpenVINOConfig.cmake" ] || {
      echo "OpenVINOConfig.cmake not found in OpenVINO_DIR" >&2; exit 1;
    }
  fi
else
  runtime_version=$(locked_field "$runtime_distribution" 2) || {
    echo "Unknown ONNX Runtime distribution: $runtime_distribution" >&2
    exit 2
  }
  runtime_sha256=$(locked_field "$runtime_distribution" 3)
  [ "$runtime_version" = "$ort_version" ] || {
    echo "ONNX Runtime distribution version mismatch: $runtime_version != $ort_version" >&2
    exit 1
  }
  case "$platform/$architecture/$profile/$runtime_distribution" in
    Linux/x86_64/cpu/onnxruntime-linux-x64|\
    Linux/aarch64/cpu/onnxruntime-linux-aarch64|\
    Linux/x86_64/cuda/onnxruntime-linux-x64-gpu-cuda12|\
    Linux/x86_64/tensorrt/onnxruntime-linux-x64-gpu-cuda13|\
    Linux/x86_64/openvino/onnxruntime-linux-x64|\
    Darwin/arm64/coreml/onnxruntime-osx-arm64) ;;
    *)
      echo "Invalid ONNX Runtime distribution for target: $platform/$architecture/$profile/$runtime_distribution" >&2
      exit 1
      ;;
  esac
  if [ "$profile" = openvino ]; then
    plugin_distribution=onnxruntime-openvino-ep-linux-x64
    plugin_sha256=$(locked_field "$plugin_distribution" 3)
  fi
fi

mkdir -p "$prefix" "$work_dir"
prefix=$(CDPATH='' cd -- "$prefix" && pwd)
work_dir=$(CDPATH='' cd -- "$work_dir" && pwd)
stamp="$platform/$architecture/$profile/onnxruntime-$ort_version-$ort_commit/opencv-$opencv_version-$opencv_commit"
stamp="$stamp/distribution-$runtime_distribution-$runtime_sha256"
if [ "$runtime_distribution" = source ]; then
  # Different hardware code generation must never reuse a prefix stamped for another SM set.
  stamp="$stamp/cuda-architectures-${ONNX_CUDA_ARCHITECTURES:-default}/nvcc-threads-$nvcc_threads"
  stamp="$stamp/openvino-device-${ONNX_OPENVINO_DEVICE:-AUTO}"
fi
if [ -n "$plugin_distribution" ]; then
  stamp="$stamp/plugin-$plugin_distribution-$plugin_sha256"
fi
if [ -f "$prefix/.mist-onnx-dependencies" ]; then
  if [ "$(cat "$prefix/.mist-onnx-dependencies")" = "$stamp" ]; then
    echo "Using cached ONNX dependencies: $stamp"
    exit 0
  fi
  echo "Dependency prefix contains a different build: $prefix" >&2
  echo "Use an empty profile-specific prefix instead of overwriting it." >&2
  exit 1
fi
if [ "$verify_only" -eq 1 ]; then
  echo "Dependency prefix does not hold the locked build $stamp: $prefix" >&2
  exit 1
fi

checkout_locked_source() {
  name=$1
  source_url=$2
  commit=$3
  destination=$4

  fresh_checkout=0
  # A missing or private upstream must fail instead of waiting for credentials.
  export GIT_TERMINAL_PROMPT=0
  if [ ! -d "$destination/.git" ]; then
    git clone --filter=blob:none --no-checkout "$source_url" "$destination"
    fresh_checkout=1
  fi
  if ! git -C "$destination" cat-file -e "$commit^{commit}" 2>/dev/null; then
    git -C "$destination" fetch --depth=1 origin "$commit"
  fi
  if [ "$fresh_checkout" -eq 0 ] &&
     [ -n "$(find "$destination" -mindepth 1 -maxdepth 1 ! -name .git -print -quit)" ] &&
     [ -n "$(git -C "$destination" status --porcelain)" ]; then
    echo "$name source checkout is dirty: $destination" >&2
    exit 1
  fi
  git -C "$destination" checkout --detach "$commit"
  actual_commit=$(git -C "$destination" rev-parse HEAD)
  [ "$actual_commit" = "$commit" ] || {
    echo "$name checkout mismatch: expected $commit, got $actual_commit" >&2
    exit 1
  }
}

# Downloads one locked NuGet package, checks its SHA-256 and .nuspec version, and unpacks
# it into $work_dir/<name>-unpack. A .nupkg is a zip holding the .nuspec, LICENSE and any
# notices at the root, headers under build/native/include (CPU) or
# buildTransitive/native/include (GPU), and one runtimes/<rid>/native directory per platform.
fetch_locked_package() {
  name=$1
  package_version=$(locked_field "$name" 2)
  package_sha256=$(locked_field "$name" 3)
  package_source=$(locked_field "$name" 5)
  package_archive="$work_dir/$name-$package_version.nupkg"
  curl -fL --retry 3 --silent --show-error --output "$package_archive" "$package_source"
  if command -v sha256sum >/dev/null 2>&1; then
    actual_sha256=$(sha256sum "$package_archive" | awk '{print $1}')
  else
    actual_sha256=$(shasum -a 256 "$package_archive" | awk '{print $1}')
  fi
  [ "$actual_sha256" = "$package_sha256" ] || {
    echo "$name checksum mismatch" >&2
    echo "Expected: $package_sha256" >&2
    echo "Actual:   $actual_sha256" >&2
    exit 1
  }
  package_unpack="$work_dir/$name-unpack"
  rm -rf "$package_unpack"
  mkdir -p "$package_unpack"
  unzip -q "$package_archive" -d "$package_unpack"
  nuspec=$(find "$package_unpack" -mindepth 1 -maxdepth 1 -type f -name '*.nuspec' -print -quit)
  [ -n "$nuspec" ] || { echo "$name has no .nuspec" >&2; exit 1; }
  nuspec_version=$(sed -n 's|.*<version>\([^<]*\)</version>.*|\1|p' "$nuspec" | head -n 1)
  [ "$nuspec_version" = "$package_version" ] || {
    echo "$name package version mismatch: $nuspec_version != $package_version" >&2
    exit 1
  }
  package_native="$package_unpack/$(locked_field "$name" 6)"
  [ -d "$package_native" ] || { echo "$name lacks $(locked_field "$name" 6)" >&2; exit 1; }
}

# Installs a library the package ships under its unversioned linker name as the SONAME /
# install name the linker records in consumers, and points the linker name at it.
install_by_soname() {
  library=$1
  linker_name=$2
  if [ "$platform" = Darwin ]; then
    soname=$(otool -D "$library" | sed -n '2p')
    soname=${soname##*/}
  else
    soname=$(readelf -d "$library" | sed -n 's/.*Library soname: \[\(.*\)\].*/\1/p')
  fi
  case "$soname" in
    "${linker_name%%.*}".*) ;;
    *) echo "Unexpected SONAME for $library: '$soname'" >&2; exit 1 ;;
  esac
  cp "$library" "$prefix/lib/$soname"
  if [ "$soname" != "$linker_name" ]; then
    ln -sf "$soname" "$prefix/lib/$linker_name"
  fi
}

opencv_src="$work_dir/opencv-src"
opencv_build="$work_dir/opencv-build"
checkout_locked_source OpenCV "$opencv_source" "$opencv_commit" "$opencv_src"

cmake -S "$opencv_src" -B "$opencv_build" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$prefix" \
  -DCMAKE_INSTALL_LIBDIR=lib \
  -DCMAKE_INSTALL_NAME_DIR=@rpath \
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
  -DBUILD_SHARED_LIBS=ON \
  -DOPENCV_FORCE_3RDPARTY_BUILD=ON \
  -DBUILD_LIST=core,imgproc,imgcodecs,video,geometry \
  -DBUILD_TESTS=OFF \
  -DBUILD_PERF_TESTS=OFF \
  -DBUILD_EXAMPLES=OFF \
  -DBUILD_opencv_apps=OFF \
  -DBUILD_opencv_python_bindings_generator=OFF \
  -DBUILD_JAVA=OFF \
  -DWITH_FFMPEG=OFF \
  -DWITH_GSTREAMER=OFF \
  -DWITH_AVIF=OFF \
  -DWITH_VTK=OFF \
  -DWITH_OPENGL=OFF \
  -DWITH_QT=OFF \
  -DWITH_COCOA=OFF \
  -DWITH_UNIFONT=OFF \
  -DWITH_OPENCL=OFF \
  -DWITH_OPENMP=OFF \
  -DWITH_TBB=OFF \
  -DWITH_EIGEN=OFF \
  -DWITH_IPP=OFF \
  -DWITH_ITT=OFF \
  -DWITH_TIFF=OFF \
  -DWITH_WEBP=OFF \
  -DWITH_OPENJPEG=OFF \
  -DWITH_OPENEXR=OFF \
  -DWITH_JASPER=OFF \
  -DWITH_IMGCODEC_HDR=OFF \
  -DWITH_IMGCODEC_PFM=OFF \
  -DWITH_IMGCODEC_PXM=OFF \
  -DOPENCV_GENERATE_PKGCONFIG=ON
cmake --build "$opencv_build" --parallel "$jobs"
cmake --install "$opencv_build"

if [ "$runtime_distribution" != source ]; then
  fetch_locked_package "$runtime_distribution"
  runtime_unpack=$package_unpack
  runtime_libs=$package_native
  runtime_include=
  for include_dir in build/native/include buildTransitive/native/include; do
    if [ -f "$runtime_unpack/$include_dir/onnxruntime_c_api.h" ]; then
      runtime_include="$runtime_unpack/$include_dir"
      break
    fi
  done
  [ -n "$runtime_include" ] &&
    [ -f "$runtime_unpack/LICENSE" ] && [ -f "$runtime_unpack/ThirdPartyNotices.txt" ] || {
    echo "Invalid ONNX Runtime package layout: $runtime_distribution" >&2
    exit 1
  }

  mkdir -p "$prefix/include/onnxruntime" "$prefix/lib/pkgconfig" \
    "$prefix/share/licenses/onnxruntime"
  cp -R "$runtime_include/." "$prefix/include/onnxruntime/"
  if [ "$platform" = Darwin ]; then
    install_by_soname "$runtime_libs/libonnxruntime.dylib" libonnxruntime.dylib
  else
    install_by_soname "$runtime_libs/libonnxruntime.so" libonnxruntime.so
  fi
  for runtime_lib in "$runtime_libs"/libonnxruntime_providers_*; do
    if [ -f "$runtime_lib" ]; then
      cp "$runtime_lib" "$prefix/lib/"
    fi
  done
  if [ "$profile" = cuda ]; then
    rm -f "$prefix/lib/libonnxruntime_providers_tensorrt.so"
  fi
  cp "$runtime_unpack/LICENSE" "$prefix/share/licenses/onnxruntime/LICENSE"
  cp "$runtime_unpack/ThirdPartyNotices.txt" \
    "$prefix/share/licenses/onnxruntime/ThirdPartyNotices.txt"

else
  ort_src="$work_dir/onnxruntime-src"
  ort_build="$work_dir/onnxruntime-build"
  checkout_locked_source "ONNX Runtime" "$ort_source" "$ort_commit" "$ort_src"
  ort_args=(
    --config Release --update --build --parallel "$jobs" --skip_tests
    --compile_no_warning_as_error --no_telemetry --build_shared_lib
    --build_dir "$ort_build" --cmake_extra_defines
    "CMAKE_INSTALL_PREFIX=$prefix" CMAKE_INSTALL_LIBDIR=lib
    CMAKE_INSTALL_NAME_DIR=@rpath CMAKE_POSITION_INDEPENDENT_CODE=ON
    CMAKE_POLICY_VERSION_MINIMUM=3.5 FETCHCONTENT_TRY_FIND_PACKAGE_MODE=NEVER
    onnxruntime_BUILD_UNIT_TESTS=OFF onnxruntime_ENABLE_PYTHON=OFF
    onnxruntime_BUILD_SHARED_LIB=ON
  )
  if [ "$(id -u)" -eq 0 ]; then ort_args+=(--allow_running_as_root); fi
  case "$profile" in
    coreml) ort_args+=(--use_coreml) ;;
    cuda|tensorrt)
      ort_args+=(--use_cuda --cuda_home "$CUDA_HOME" --cudnn_home "$CUDNN_HOME"
        --cmake_extra_defines "CMAKE_CUDA_ARCHITECTURES=$ONNX_CUDA_ARCHITECTURES"
        "onnxruntime_NVCC_THREADS=$nvcc_threads" "onnxruntime_FLASH_NVCC_THREADS=$nvcc_threads")
      if [ "$profile" = tensorrt ]; then
        ort_args+=(--use_tensorrt --tensorrt_home "$TENSORRT_HOME")
      fi
      ;;
    openvino)
      ort_args+=(--use_openvino "${ONNX_OPENVINO_DEVICE:-AUTO}"
        --cmake_extra_defines "OpenVINO_DIR=$OpenVINO_DIR")
      ;;
  esac
  (cd "$ort_src" && ./build.sh "${ort_args[@]}")
  cmake --install "$ort_build/Release"
  mkdir -p "$prefix/share/licenses/onnxruntime"
  cp "$ort_src/LICENSE" "$prefix/share/licenses/onnxruntime/LICENSE"
  cp "$ort_src/ThirdPartyNotices.txt" "$prefix/share/licenses/onnxruntime/ThirdPartyNotices.txt"
fi

if [ -n "$plugin_distribution" ]; then
  # Intel's plugin bundles its own OpenVINO runtime (libopenvino, device plugins, TBB,
  # hwloc and the GPU kernel cache.json), which loads from the same directory.
  fetch_locked_package "$plugin_distribution"
  plugin_libs=$package_native
  plugin_name=libonnxruntime_providers_openvino_plugin.so
  [ -f "$plugin_libs/$plugin_name" ] || { echo "Missing $plugin_libs/$plugin_name" >&2; exit 1; }
  install_by_soname "$plugin_libs/$plugin_name" "$plugin_name"
  for plugin_file in "$plugin_libs"/*; do
    case "${plugin_file##*/}" in
      "$plugin_name"*|third-party-programs.txt) ;;
      *) cp "$plugin_file" "$prefix/lib/" ;;
    esac
  done
  mkdir -p "$prefix/share/licenses/onnxruntime-openvino-ep"
  cp "$plugin_libs/third-party-programs.txt" \
    "$prefix/share/licenses/onnxruntime-openvino-ep/third-party-programs.txt"
fi

# LD_LIBRARY_PATH outranks DT_RUNPATH, so a host exporting another OpenVINO, ONNX Runtime or
# OpenCV (OpenVINO's setupvars.sh does) would load it in place of a library's siblings here.
# Every library in the prefix carries DT_RPATH $ORIGIN instead, which the loader searches
# before LD_LIBRARY_PATH; OpenVINO dlopens its device plugins by path from this directory.
# shellcheck disable=SC2016
if [ "$platform" = Linux ]; then
  for library in "$prefix"/lib/*.so*; do
    if [ -L "$library" ] || [ ! -f "$library" ]; then continue; fi
    patchelf --force-rpath --set-rpath '$ORIGIN' "$library"
    dynamic=$(readelf -d "$library")
    if printf '%s\n' "$dynamic" | grep -F '(RUNPATH)' >/dev/null ||
       ! printf '%s\n' "$dynamic" | grep -F '(RPATH)' | grep -F '[$ORIGIN]' >/dev/null; then
      echo "$library does not carry DT_RPATH \$ORIGIN" >&2
      exit 1
    fi
  done
fi

cat > "$prefix/lib/pkgconfig/libonnxruntime.pc" <<EOF
prefix=$prefix
libdir=\${prefix}/lib
includedir=\${prefix}/include/onnxruntime

Name: onnxruntime
Description: ONNX Runtime shared $profile profile for MistServer
Version: $runtime_version
Libs: -L\${libdir} -lonnxruntime
Cflags: -I\${includedir}
EOF

# Exercise the installed consumer interface: link through pkg-config, list the compiled
# providers and, for the openvino profile, register the plugin library.
smoke_source="$work_dir/onnxruntime-link-smoke.cpp"
cat > "$smoke_source" <<'EOF'
#include <onnxruntime_c_api.h>
#include <cstdio>
static int fail(const OrtApi *api, OrtStatus *status) {
  std::fprintf(stderr, "%s\n", api->GetErrorMessage(status));
  api->ReleaseStatus(status);
  return 1;
}
int main(int argc, char **argv) {
  const OrtApiBase *base = OrtGetApiBase();
  if (!base) return 1;
  const OrtApi *api = base->GetApi(ORT_API_VERSION);
  if (!api) return 1;
  char **providers = nullptr;
  int provider_count = 0;
  OrtStatus *status = api->GetAvailableProviders(&providers, &provider_count);
  if (status) return fail(api, status);
  for (int i = 0; i < provider_count; ++i) std::puts(providers[i]);
  status = api->ReleaseAvailableProviders(providers, provider_count);
  if (status) return fail(api, status);
  if (argc > 1) {
    OrtEnv *env = nullptr;
    status = api->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "smoke", &env);
    if (status) return fail(api, status);
    status = api->RegisterExecutionProviderLibrary(env, "plugin", argv[1]);
    if (status) return fail(api, status);
    std::puts("PluginRegistered");
  }
  return 0;
}
EOF
pkg_config_path="$prefix/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
# shellcheck disable=SC2046
PKG_CONFIG_PATH="$pkg_config_path" "${CXX:-c++}" -std=c++17 "$smoke_source" \
  $(PKG_CONFIG_PATH="$pkg_config_path" pkg-config --cflags --libs libonnxruntime) \
  -o "$work_dir/onnxruntime-link-smoke"
smoke_args=()
if [ -n "$plugin_distribution" ]; then
  smoke_args=("$prefix/lib/libonnxruntime_providers_openvino_plugin.so")
fi
if [ "$platform" = Darwin ]; then
  available_providers=$(DYLD_LIBRARY_PATH="$prefix/lib${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}" \
    "$work_dir/onnxruntime-link-smoke" ${smoke_args[@]+"${smoke_args[@]}"})
else
  available_providers=$(LD_LIBRARY_PATH="$prefix/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
    "$work_dir/onnxruntime-link-smoke" ${smoke_args[@]+"${smoke_args[@]}"})
fi
expected_provider=CPUExecutionProvider
case "$profile" in
  coreml) expected_provider=CoreMLExecutionProvider ;;
  cuda) expected_provider=CUDAExecutionProvider ;;
  tensorrt) expected_provider=TensorrtExecutionProvider ;;
  openvino)
    expected_provider=OpenVINOExecutionProvider
    if [ -n "$plugin_distribution" ]; then expected_provider=PluginRegistered; fi
    ;;
esac
printf '%s\n' "$available_providers" | grep -Fx "$expected_provider" >/dev/null || {
  printf '%s\n' "$available_providers" >&2
  echo "ONNX Runtime does not provide the expected provider: $expected_provider" >&2
  exit 1
}

mkdir -p "$prefix/share/licenses/opencv"
cp "$opencv_src/LICENSE" "$prefix/share/licenses/opencv/LICENSE"
printf '%s\n' "$stamp" > "$prefix/.mist-onnx-dependencies"

echo "Built ONNX dependency profile: $stamp"
echo "Prefix: $prefix"
