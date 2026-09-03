#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat >&2 <<'EOF'
Usage: build_dependencies.sh --prefix DIR --work-dir DIR [options]

Builds the locked ONNX Runtime and minimal OpenCV dependency set used by
MistProcONNX.

Options:
  --profile PROFILE   cpu, coreml, cuda, tensorrt, or openvino (default: cpu)
  --jobs COUNT        Parallel build jobs (default: detected CPU count)
  --lock FILE         Dependency lock file (default: dependencies.lock.tsv)
  --runtime-distribution NAME
                      Locked ONNX Runtime binary distribution, or source (default)
  --nvcc-threads COUNT
                      Threads used internally by each NVCC compilation (default: 1)

Provider SDK environment:
  CUDA_HOME, CUDNN_HOME                       cuda and tensorrt
  TENSORRT_HOME                               tensorrt
  ONNX_CUDA_ARCHITECTURES                    explicit CMake SM list for headless builds
  OpenVINO_DIR                                openvino (directory containing OpenVINOConfig.cmake)
  ONNX_OPENVINO_DEVICE                       openvino (default: AUTO:GPU,CPU)
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

while [ "$#" -gt 0 ]; do
  case "$1" in
    --prefix) [ "$#" -ge 2 ] || usage; prefix=$2; shift 2 ;;
    --work-dir) [ "$#" -ge 2 ] || usage; work_dir=$2; shift 2 ;;
    --profile) [ "$#" -ge 2 ] || usage; profile=$2; shift 2 ;;
    --jobs) [ "$#" -ge 2 ] || usage; jobs=$2; shift 2 ;;
    --lock) [ "$#" -ge 2 ] || usage; lock_file=$2; shift 2 ;;
    --runtime-distribution) [ "$#" -ge 2 ] || usage; runtime_distribution=$2; shift 2 ;;
    --nvcc-threads) [ "$#" -ge 2 ] || usage; nvcc_threads=$2; shift 2 ;;
    -h|--help) usage ;;
    *) usage ;;
  esac
done

[ -n "$prefix" ] || usage
[ -n "$work_dir" ] || usage
case "$profile" in
  cpu|coreml|cuda|tensorrt|openvino) ;;
  *) echo "Unsupported ONNX profile: $profile" >&2; exit 2 ;;
esac

for command_name in awk cmake git pkg-config; do
  command -v "$command_name" >/dev/null 2>&1 || {
    echo "Required command not found: $command_name" >&2
    exit 1
  }
done
if [ "$runtime_distribution" != source ]; then
  for command_name in curl tar; do
    command -v "$command_name" >/dev/null 2>&1 || {
      echo "Required command not found: $command_name" >&2
      exit 1
    }
  done
  if ! command -v sha256sum >/dev/null 2>&1 && ! command -v shasum >/dev/null 2>&1; then
    echo "sha256sum or shasum is required for a binary runtime distribution" >&2
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
  ''|*[!0-9]*) echo "Invalid NVCC thread count: $nvcc_threads" >&2; exit 2 ;;
esac
[ "$nvcc_threads" -gt 0 ] || { echo "NVCC thread count must be positive" >&2; exit 2; }

locked_field() {
  dependency=$1
  field=$2
  awk -F '\t' -v dependency="$dependency" -v field="$field" \
    '$1 == dependency { print $field; found = 1; exit } END { if (!found) exit 1 }' "$lock_file"
}

ort_version=$(locked_field onnxruntime 2)
ort_commit=$(locked_field onnxruntime 3)
ort_source=$(locked_field onnxruntime 5)
opencv_version=$(locked_field opencv 2)
opencv_commit=$(locked_field opencv 3)
opencv_source=$(locked_field opencv 5)

platform=$(uname -s)
architecture=$(uname -m)
static_runtime=0
case "$profile" in
  cpu|coreml) static_runtime=1 ;;
esac

if [ "$runtime_distribution" != source ]; then
  runtime_version=$(locked_field "$runtime_distribution" 2)
  runtime_sha256=$(locked_field "$runtime_distribution" 3)
  runtime_source=$(locked_field "$runtime_distribution" 5)
  [ "$runtime_version" = "$ort_version" ] || {
    echo "ONNX Runtime distribution version mismatch: $runtime_version != $ort_version" >&2
    exit 1
  }
  case "$platform/$architecture/$profile/$runtime_distribution" in
    Linux/x86_64/cpu/onnxruntime-linux-x64|\
    Linux/aarch64/cpu/onnxruntime-linux-aarch64|\
    Linux/x86_64/cuda/onnxruntime-linux-x64-gpu-cuda12|\
    Linux/x86_64/tensorrt/onnxruntime-linux-x64-gpu-cuda13|\
    Darwin/arm64/coreml/onnxruntime-osx-arm64) ;;
    *)
      echo "Invalid ONNX Runtime distribution for target: $platform/$architecture/$profile/$runtime_distribution" >&2
      exit 1
      ;;
  esac
  static_runtime=0
fi
if [ "$profile" = coreml ] && [ "$platform" != Darwin ]; then
  echo "The CoreML profile can only be built on Darwin" >&2
  exit 1
fi
if [ "$profile" = cuda ] || [ "$profile" = tensorrt ]; then
  : "${CUDA_HOME:?CUDA_HOME is required for the $profile profile}"
  : "${CUDNN_HOME:?CUDNN_HOME is required for the $profile profile}"
  : "${ONNX_CUDA_ARCHITECTURES:?ONNX_CUDA_ARCHITECTURES is required for headless $profile builds}"
  [ -d "$CUDA_HOME" ] || { echo "CUDA_HOME is not a directory: $CUDA_HOME" >&2; exit 1; }
  [ -d "$CUDNN_HOME" ] || { echo "CUDNN_HOME is not a directory: $CUDNN_HOME" >&2; exit 1; }
fi
if [ "$profile" = tensorrt ]; then
  : "${TENSORRT_HOME:?TENSORRT_HOME is required for the tensorrt profile}"
  [ -d "$TENSORRT_HOME" ] || {
    echo "TENSORRT_HOME is not a directory: $TENSORRT_HOME" >&2
    exit 1
  }
fi
if [ "$profile" = openvino ]; then
  : "${OpenVINO_DIR:?OpenVINO_DIR is required for the openvino profile}"
  [ -f "$OpenVINO_DIR/OpenVINOConfig.cmake" ] || {
    echo "OpenVINOConfig.cmake not found in OpenVINO_DIR: $OpenVINO_DIR" >&2
    exit 1
  }
fi

mkdir -p "$prefix" "$work_dir"
prefix=$(CDPATH='' cd -- "$prefix" && pwd)
work_dir=$(CDPATH='' cd -- "$work_dir" && pwd)
stamp="$platform/$architecture/$profile/onnxruntime-$ort_version-$ort_commit/opencv-$opencv_version-$opencv_commit"
if [ "$runtime_distribution" != source ]; then
  stamp="$stamp/distribution-$runtime_distribution-$runtime_sha256"
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

checkout_locked_source() {
  name=$1
  source_url=$2
  commit=$3
  destination=$4

  fresh_checkout=0
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

opencv_src="$work_dir/opencv-src"
opencv_build="$work_dir/opencv-build"
ort_src="$work_dir/onnxruntime-src"
ort_build="$work_dir/onnxruntime-build"

checkout_locked_source OpenCV "$opencv_source" "$opencv_commit" "$opencv_src"
if [ "$runtime_distribution" = source ]; then
  checkout_locked_source "ONNX Runtime" "$ort_source" "$ort_commit" "$ort_src"
fi

opencv_shared=ON
if [ "$static_runtime" -eq 1 ]; then opencv_shared=OFF; fi

cmake -S "$opencv_src" -B "$opencv_build" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$prefix" \
  -DCMAKE_INSTALL_LIBDIR=lib \
  -DCMAKE_INSTALL_NAME_DIR=@rpath \
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
  -DBUILD_SHARED_LIBS="$opencv_shared" \
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
  runtime_archive="$work_dir/$runtime_distribution-$runtime_version.tgz"
  curl -fL --retry 3 --output "$runtime_archive" "$runtime_source"
  if command -v sha256sum >/dev/null 2>&1; then
    actual_runtime_sha256=$(sha256sum "$runtime_archive" | awk '{print $1}')
  else
    actual_runtime_sha256=$(shasum -a 256 "$runtime_archive" | awk '{print $1}')
  fi
  [ "$actual_runtime_sha256" = "$runtime_sha256" ] || {
    echo "ONNX Runtime distribution checksum mismatch" >&2
    echo "Expected: $runtime_sha256" >&2
    echo "Actual:   $actual_runtime_sha256" >&2
    exit 1
  }

  runtime_unpack="$work_dir/$runtime_distribution-unpack"
  mkdir -p "$runtime_unpack"
  tar -xzf "$runtime_archive" -C "$runtime_unpack"
  runtime_root=$(find "$runtime_unpack" -mindepth 1 -maxdepth 3 -type f \
    -name VERSION_NUMBER -exec dirname {} \; -quit)
  [ -n "$runtime_root" ] && [ -d "$runtime_root/include" ] && [ -d "$runtime_root/lib" ] || {
    echo "Invalid ONNX Runtime distribution layout: $runtime_distribution" >&2
    exit 1
  }
  [ "$(cat "$runtime_root/VERSION_NUMBER")" = "$runtime_version" ] || {
    echo "ONNX Runtime distribution VERSION_NUMBER mismatch" >&2
    exit 1
  }

  mkdir -p "$prefix/include/onnxruntime" "$prefix/lib/pkgconfig" \
    "$prefix/share/licenses/onnxruntime"
  cp -a "$runtime_root/include/." "$prefix/include/onnxruntime/"
  for runtime_lib in \
    "$runtime_root"/lib/libonnxruntime*.so \
    "$runtime_root"/lib/libonnxruntime*.so.* \
    "$runtime_root"/lib/libonnxruntime*.dylib; do
    if [ -f "$runtime_lib" ] || [ -L "$runtime_lib" ]; then
      cp -a "$runtime_lib" "$prefix/lib/"
    fi
  done
  if [ "$profile" = cuda ]; then
    rm -f "$prefix/lib/libonnxruntime_providers_tensorrt.so"
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
  cp "$runtime_root/LICENSE" "$prefix/share/licenses/onnxruntime/LICENSE"
  cp "$runtime_root/ThirdPartyNotices.txt" \
    "$prefix/share/licenses/onnxruntime/ThirdPartyNotices.txt"
else
  ort_args=(
  --config Release
  --update
  --build
  --parallel "$jobs"
  --skip_tests
  --compile_no_warning_as_error
  --no_telemetry
  --build_dir "$ort_build"
  --cmake_extra_defines
  "CMAKE_INSTALL_PREFIX=$prefix"
  CMAKE_INSTALL_LIBDIR=lib
  CMAKE_POSITION_INDEPENDENT_CODE=ON
  CMAKE_POLICY_VERSION_MINIMUM=3.5
  FETCHCONTENT_TRY_FIND_PACKAGE_MODE=NEVER
  onnxruntime_BUILD_UNIT_TESTS=OFF
  onnxruntime_ENABLE_PYTHON=OFF
  "onnxruntime_BUILD_SHARED_LIB=$([ "$static_runtime" -eq 1 ] && echo OFF || echo ON)"
  )
  if [ "$(id -u)" -eq 0 ]; then
    ort_args+=(--allow_running_as_root)
  fi
  case "$profile" in
    coreml) ort_args+=(--use_coreml) ;;
    cuda)
      ort_args+=(--use_cuda --cuda_home "$CUDA_HOME" --cudnn_home "$CUDNN_HOME"
        --cmake_extra_defines "CMAKE_CUDA_ARCHITECTURES=$ONNX_CUDA_ARCHITECTURES"
        "onnxruntime_NVCC_THREADS=$nvcc_threads" "onnxruntime_FLASH_NVCC_THREADS=$nvcc_threads")
      ;;
    tensorrt)
      ort_args+=(--use_cuda --cuda_home "$CUDA_HOME" --cudnn_home "$CUDNN_HOME"
        --use_tensorrt --tensorrt_home "$TENSORRT_HOME"
        --cmake_extra_defines "CMAKE_CUDA_ARCHITECTURES=$ONNX_CUDA_ARCHITECTURES"
        "onnxruntime_NVCC_THREADS=$nvcc_threads" "onnxruntime_FLASH_NVCC_THREADS=$nvcc_threads")
      ;;
    openvino)
      ort_args+=(--use_openvino "${ONNX_OPENVINO_DEVICE:-AUTO:GPU,CPU}"
        --cmake_extra_defines "OpenVINO_DIR=$OpenVINO_DIR")
      ;;
  esac

  if [ "$static_runtime" -eq 0 ]; then
    ort_args+=(--build_shared_lib)
  fi

  (cd "$ort_src" && ./build.sh "${ort_args[@]}")
  cmake --install "$ort_build/Release"
fi

# The upstream static install exports an interface target but its pkg-config file still
# names a non-existent monolithic archive, while many third-party archives in that target's
# link closure are not installed. Build that closure explicitly into one relocatable archive
# and replace the unusable pkg-config metadata. Accelerator profiles intentionally remain
# shared: CUDA, TensorRT and OpenVINO have their own driver/SDK runtime closure.
if [ "$static_runtime" -eq 1 ]; then
  cmake --build "$ort_build/Release" --target re2 --parallel "$jobs"
  archive_list="$work_dir/onnxruntime-static-archives.txt"
  find "$ort_build/Release" -type f -name '*.a' | LC_ALL=C sort > "$archive_list"
  # ORT is built with its default lite protobuf runtime for every static profile.
  # Keep libprotobuf-lite in the aggregate and discard the unused full/compiler archives.
  grep -v '/libprotobuf\.a$' "$archive_list" | grep -v '/libprotoc\.a$' > "$archive_list.filtered"
  mv "$archive_list.filtered" "$archive_list"
  [ -s "$archive_list" ] || { echo "ONNX Runtime produced no static archives" >&2; exit 1; }

  aggregate="$prefix/lib/libonnxruntime.a"
  rm -f "$aggregate"
  if [ "$platform" = Darwin ]; then
    command -v libtool >/dev/null 2>&1 || { echo "Apple libtool is required" >&2; exit 1; }
    archives=()
    while IFS= read -r archive; do archives+=("$archive"); done < "$archive_list"
    libtool -static -o "$aggregate" "${archives[@]}"
  else
    command -v ar >/dev/null 2>&1 || { echo "ar is required" >&2; exit 1; }
    mri_script="$work_dir/onnxruntime-static.mri"
    {
      printf 'create %s\n' "$aggregate"
      sed 's/^/addlib /' "$archive_list"
      printf 'save\nend\n'
    } > "$mri_script"
    ar -M < "$mri_script"
  fi

  # Keep the aggregate and OpenCV archives only. The component archives and the provider
  # bridge shared object are implementation debris from the upstream static install.
  find "$prefix/lib" -maxdepth 1 -type f -name '*.a' \
    ! -name 'libonnxruntime.a' ! -name 'libopencv*.a' -delete
  rm -f "$prefix/lib/libonnxruntime_providers_shared.so" \
        "$prefix/lib/libonnxruntime_providers_shared.dylib"
  rm -rf "$prefix/lib/cmake/onnxruntime"

  mkdir -p "$prefix/lib/pkgconfig"
  private_libs='-ldl -lrt -lpthread'
  if [ "$profile" = coreml ]; then
    private_libs='-framework Foundation -framework CoreML -liconv'
  fi
  cat > "$prefix/lib/pkgconfig/libonnxruntime.pc" <<EOF
prefix=$prefix
libdir=\${prefix}/lib
includedir=\${prefix}/include/onnxruntime

Name: onnxruntime
Description: ONNX Runtime static $profile profile for MistServer
Version: $ort_version
Libs: -L\${libdir} -lonnxruntime
Libs.private: $private_libs
Cflags: -I\${includedir}
EOF
fi

# Exercise the installed consumer interface. This catches the broken upstream static
# pkg-config metadata and missing aggregate members before the prefix reaches CI caches.
smoke_source="$work_dir/onnxruntime-link-smoke.cpp"
cat > "$smoke_source" <<'EOF'
#include <onnxruntime_c_api.h>
#include <cstdio>
int main() {
  const OrtApiBase *base = OrtGetApiBase();
  if (!base) return 1;
  const OrtApi *api = base->GetApi(ORT_API_VERSION);
  if (!api) return 1;
  char **providers = nullptr;
  int provider_count = 0;
  OrtStatus *status = api->GetAvailableProviders(&providers, &provider_count);
  if (status) {
    std::fprintf(stderr, "%s\n", api->GetErrorMessage(status));
    api->ReleaseStatus(status);
    return 1;
  }
  for (int i = 0; i < provider_count; ++i) std::puts(providers[i]);
  status = api->ReleaseAvailableProviders(providers, provider_count);
  if (status) {
    std::fprintf(stderr, "%s\n", api->GetErrorMessage(status));
    api->ReleaseStatus(status);
    return 1;
  }
  return 0;
}
EOF
pkg_config_path="$prefix/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
# shellcheck disable=SC2046
PKG_CONFIG_PATH="$pkg_config_path" "${CXX:-c++}" -std=c++17 "$smoke_source" \
  $(PKG_CONFIG_PATH="$pkg_config_path" pkg-config --cflags --libs --static libonnxruntime) \
  -o "$work_dir/onnxruntime-link-smoke"
if [ "$static_runtime" -eq 1 ]; then
  available_providers=$("$work_dir/onnxruntime-link-smoke")
elif [ "$platform" = Darwin ]; then
  available_providers=$(DYLD_LIBRARY_PATH="$prefix/lib${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}" \
    "$work_dir/onnxruntime-link-smoke")
else
  available_providers=$(LD_LIBRARY_PATH="$prefix/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
    "$work_dir/onnxruntime-link-smoke")
fi
expected_provider=CPUExecutionProvider
case "$profile" in
  coreml) expected_provider=CoreMLExecutionProvider ;;
  cuda) expected_provider=CUDAExecutionProvider ;;
  tensorrt) expected_provider=TensorrtExecutionProvider ;;
  openvino) expected_provider=OpenVINOExecutionProvider ;;
esac
printf '%s\n' "$available_providers" | grep -Fx "$expected_provider" >/dev/null || {
  printf '%s\n' "$available_providers" >&2
  echo "ONNX Runtime did not compile the expected provider: $expected_provider" >&2
  exit 1
}

mkdir -p "$prefix/share/licenses/onnxruntime" "$prefix/share/licenses/opencv"
if [ "$runtime_distribution" = source ]; then
  cp "$ort_src/LICENSE" "$prefix/share/licenses/onnxruntime/LICENSE"
  cp "$ort_src/ThirdPartyNotices.txt" \
    "$prefix/share/licenses/onnxruntime/ThirdPartyNotices.txt"
fi
cp "$opencv_src/LICENSE" "$prefix/share/licenses/opencv/LICENSE"
printf '%s\n' "$stamp" > "$prefix/.mist-onnx-dependencies"

echo "Built ONNX dependency profile: $stamp"
echo "Prefix: $prefix"
