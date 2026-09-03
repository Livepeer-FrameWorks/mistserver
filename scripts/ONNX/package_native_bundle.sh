#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat >&2 <<'EOF'
Usage: package_native_bundle.sh --image IMAGE --output FILE.tar.gz
       --profile PROFILE --platform linux/ARCH --version VERSION
       --source-revision SHA
EOF
  exit 2
}

image=
output=
profile=
platform=
version=
source_revision=
while [ "$#" -gt 0 ]; do
  case "$1" in
    --image) [ "$#" -ge 2 ] || usage; image=$2; shift 2 ;;
    --output) [ "$#" -ge 2 ] || usage; output=$2; shift 2 ;;
    --profile) [ "$#" -ge 2 ] || usage; profile=$2; shift 2 ;;
    --platform) [ "$#" -ge 2 ] || usage; platform=$2; shift 2 ;;
    --version) [ "$#" -ge 2 ] || usage; version=$2; shift 2 ;;
    --source-revision) [ "$#" -ge 2 ] || usage; source_revision=$2; shift 2 ;;
    *) usage ;;
  esac
done

[ -n "$image" ] || usage
[ -n "$output" ] || usage
[ -n "$profile" ] || usage
[ -n "$platform" ] || usage
[ -n "$version" ] || usage
[ -n "$source_revision" ] || usage
case "$profile" in cuda|tensorrt|openvino) ;; *) usage ;; esac
case "$platform" in linux/amd64|linux/arm64) ;; *) usage ;; esac

for command_name in docker jq tar sha256sum; do
  command -v "$command_name" >/dev/null || {
    echo "Missing packaging command: $command_name" >&2
    exit 1
  }
done

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
matrix="$script_dir/accelerator-images.json"
[ -f "$matrix" ] || { echo "Missing accelerator matrix: $matrix" >&2; exit 1; }

capability=$(docker run --rm --entrypoint MistProcONNX "$image" -j 2>/dev/null || true)
printf '%s' "$capability" | grep -F "This binary packages the '$profile' profile" >/dev/null || {
  echo "Image does not advertise the requested ONNX profile: $profile" >&2
  exit 1
}

stage=$(mktemp -d)
container=
cleanup() {
  if [ -n "$container" ]; then docker rm -f "$container" >/dev/null 2>&1 || true; fi
  rm -rf "$stage"
}
trap cleanup EXIT

mkdir -p "$stage/bin" "$stage/lib" "$stage/opt/mist-onnx" \
  "$stage/share/mistserver/onnx"
container=$(docker create --entrypoint /bin/true "$image")
docker cp "$container:/usr/local/bin/." "$stage/bin/"
docker cp "$container:/usr/local/lib/." "$stage/lib/"
docker cp "$container:/opt/mist-onnx/." "$stage/opt/mist-onnx/"

jq -e \
  --arg profile "$profile" \
  --arg platform "$platform" \
  --arg version "$version" \
  --arg source_revision "$source_revision" '
    . as $matrix
    | first(.include[] | select(.profile == $profile and .platform == $platform)) as $target
    | if $target == null then error("profile/platform missing from accelerator matrix") else
        {
          schema: $matrix.native_deployment.schema,
          artifact_kind: "mistserver-native-accelerator",
          version: $version,
          source_revision: $source_revision,
          profile: $profile,
          platform: $platform,
          host: $matrix.native_deployment.platforms[$platform],
          provider_runtime: ($matrix.native_deployment.profiles[$profile] + {
            reference_runtime_image: $target.runtime_base
          }),
          install: {
            requires_root: true,
            copy: [
              {source: "bin", destination: "/usr/local/bin"},
              {source: "lib", destination: "/usr/local/lib"},
              {source: "opt/mist-onnx", destination: "/opt/mist-onnx"},
              {source: "share", destination: "/usr/local/share"}
            ],
            run_ldconfig: true,
            loader_paths: $matrix.native_deployment.platforms[$platform].loader_paths
          }
        }
      end
  ' "$matrix" > "$stage/share/mistserver/onnx/deployment-contract.json"
jq -e '.install.loader_paths | type == "array" and length > 0' \
  "$stage/share/mistserver/onnx/deployment-contract.json" >/dev/null

docker run --rm --entrypoint /bin/sh "$image" -ec '
  for file in /usr/local/bin/Mist* \
              /usr/local/lib/*/libmistonnx.so \
              /opt/mist-onnx/libexec/onnxmodelprobe \
              /opt/mist-onnx/lib/*.so /opt/mist-onnx/lib/*.so.*; do
    [ -e "$file" ] || continue
    printf "[%s]\n" "$file"
    ldd "$file"
  done
' > "$stage/share/mistserver/onnx/runtime-linkage.txt"
if grep -F "not found" "$stage/share/mistserver/onnx/runtime-linkage.txt" \
     > "$stage/share/mistserver/onnx/runtime-missing.txt"; then
  if [ "$profile" = cuda ] || [ "$profile" = tensorrt ]; then
    grep -Ev '^[[:space:]]*libcuda\.so\.1 => not found$' \
      "$stage/share/mistserver/onnx/runtime-missing.txt" \
      > "$stage/share/mistserver/onnx/runtime-unexpected-missing.txt" || true
  else
    cp "$stage/share/mistserver/onnx/runtime-missing.txt" \
      "$stage/share/mistserver/onnx/runtime-unexpected-missing.txt"
  fi
  if [ -s "$stage/share/mistserver/onnx/runtime-unexpected-missing.txt" ]; then
    cat "$stage/share/mistserver/onnx/runtime-unexpected-missing.txt" >&2
    echo "Built image has unexpected unresolved runtime libraries" >&2
    exit 1
  fi
fi

mkdir -p "$(dirname "$output")"
tar -C "$stage" -czf "$output" bin lib opt share
(cd "$(dirname "$output")" && sha256sum "$(basename "$output")") > "$output.sha256.txt"

tar -tzf "$output" | grep -Fx 'share/mistserver/onnx/deployment-contract.json' >/dev/null
echo "Native accelerator bundle: $output"
