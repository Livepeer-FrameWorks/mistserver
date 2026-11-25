#!/usr/bin/env bash
set -euo pipefail

# The packaged EP filters CPU hardware by Intel vendor ID before asking OpenVINO.
# Rebuild that EP with CPU matching delegated to OpenVINO, retaining the packaged
# GPU/NPU runtime and their vendor checks.
script_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
prefix=$1
work_dir=$2
jobs=$3
lock_file=$4
mkdir -p "$work_dir"

locked_field() {
  awk -F '\t' -v name="$1" -v field="$2" '$1 == name { print $field; found=1; exit } END { if (!found) exit 1 }' "$lock_file"
}

fetch_source() {
  local name=$1 archive unpack actual expected
  archive="$work_dir/$name.zip"
  unpack="$work_dir/$name"
  expected=$(locked_field "$name" 3)
  curl -fL --retry 3 --silent --show-error --output "$archive" "$(locked_field "$name" 5)"
  actual=$(sha256sum "$archive" | awk '{print $1}')
  [ "$actual" = "$expected" ] || { echo "$name checksum mismatch" >&2; exit 1; }
  mkdir -p "$unpack"
  unzip -qo "$archive" -d "$unpack"
  printf '%s/%s\n' "$unpack" "$(locked_field "$name" 6)"
}

source_dir=$(fetch_source openvino-plugin-source)
sdk_dir=$(fetch_source openvino-plugin-sdk)
gsl_dir=$(fetch_source openvino-plugin-gsl)
json_dir=$(fetch_source openvino-plugin-json)
googletest_dir=$(fetch_source openvino-plugin-googletest)
patch --batch --forward -d "$source_dir" -p1 < "$script_dir/openvino-plugin-cpu.patch"

# NuGet ships the public headers and runtime without a CMake package. The plugin
# uses this package only to validate the installed ORT version; its C API is loaded
# by the host and does not link another runtime into the plugin.
ort_version=$(locked_field onnxruntime 2)
cmake_package="$work_dir/ort-cmake"
mkdir -p "$cmake_package"
printf 'set(onnxruntime_FOUND TRUE)\nset(onnxruntime_VERSION %s)\n' "$ort_version" > "$cmake_package/onnxruntimeConfig.cmake"
printf 'set(PACKAGE_VERSION %s)\nif(PACKAGE_FIND_VERSION VERSION_GREATER PACKAGE_VERSION)\n  set(PACKAGE_VERSION_COMPATIBLE FALSE)\nelse()\n  set(PACKAGE_VERSION_COMPATIBLE TRUE)\nendif()\n' \
  "$ort_version" > "$cmake_package/onnxruntimeConfigVersion.cmake"

ONNXRUNTIME_ROOT_DIR="$prefix" INTEL_OPENVINO_DIR="$sdk_dir" \
  cmake -S "$source_dir" -B "$work_dir/build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -Donnxruntime_DIR="$cmake_package" -DOpenVINO_DIR="$sdk_dir/cmake" \
    -DGIT_COMMIT_ID=73acd09bd2ae2bb778d94dae447479546db8abdf \
    -DOVEP_PLUGIN_BUILD_TYPE=mist-cpu-portable \
    -DOVEP_PLUGIN_BUILD_TESTS=OFF -DOVEP_PLUGIN_BUILD_TOOLS=OFF \
    -DFETCHCONTENT_FULLY_DISCONNECTED=ON \
    -DFETCHCONTENT_SOURCE_DIR_GSL="$gsl_dir" \
    -DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON="$json_dir" \
    -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST="$googletest_dir"
cmake --build "$work_dir/build" --parallel "$jobs" --target onnxruntime_providers_openvino_plugin
plugin=$(find "$work_dir/build" -type f -name 'libonnxruntime_providers_openvino_plugin.so.*' -print -quit)
[ -n "$plugin" ] || { echo 'Built OpenVINO plugin missing' >&2; exit 1; }

# Keep the NuGet runtime intact. A mismatched development SDK must fail here,
# rather than silently mixing OpenVINO releases in the installed prefix.
while IFS= read -r library; do
  test -f "$prefix/lib/$library" || { echo "OpenVINO SDK/runtime mismatch: $library" >&2; exit 1; }
done < <(readelf -d "$plugin" | sed -n 's/.*Shared library: \[\(libopenvino[^]]*\)\].*/\1/p')
cp "$plugin" "$prefix/lib/libonnxruntime_providers_openvino_plugin.so.1"
ln -sf libonnxruntime_providers_openvino_plugin.so.1 "$prefix/lib/libonnxruntime_providers_openvino_plugin.so"
license_dir="$prefix/share/licenses/onnxruntime-openvino-ep"
mkdir -p "$license_dir"
cp "$source_dir/LICENSE" "$license_dir/LICENSE"
cp "$script_dir/openvino-plugin-cpu.patch" "$license_dir/mist-cpu-portable.patch"
cp "$lock_file" "$license_dir/build-inputs.lock.tsv"

# A successful Add requires OpenVINO: the probe disables ORT CPU EP fallback.
patchelf --force-rpath --set-rpath "\$ORIGIN" "$prefix/lib/libonnxruntime_providers_openvino_plugin.so.1"
"${CXX:-c++}" -std=c++17 -O1 "$script_dir/../../test/onnx_openvino_cpu.cpp" \
  -I"$prefix/include/onnxruntime" -L"$prefix/lib" -Wl,-rpath,"$prefix/lib" -lonnxruntime \
  -o "$work_dir/openvino-cpu-probe"
"$work_dir/openvino-cpu-probe" "$prefix/lib/libonnxruntime_providers_openvino_plugin.so"
