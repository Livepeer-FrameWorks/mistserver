#!/usr/bin/env bash
# dependency_image.sh plan
# dependency_image.sh tag|ref|manifest PROFILE ARCH
#
# Names the content-addressed image that carries one locked ONNX dependency
# prefix (/opt/mist-onnx: ONNX Runtime and OpenCV) for a profile/architecture.
# The tag is a hash of everything that decides the prefix contents:
# Dockerfile.onnx-deps, build_dependencies.sh, the dependency lock, and the
# target's platform, digest-pinned build base, runtime distribution and SDK
# settings from accelerator-images.json. Build parallelism (jobs, NVCC threads,
# swap, runner) is left out because it does not change the output, so the same
# inputs always resolve to the same tag no matter which MistServer revision or
# release asks for it.
#
#   plan      JSON array of every target with its key, tag and ref
#   tag       <profile>-<arch>-<hash>
#   ref       <repository>:<tag>
#   manifest  the exact text that is hashed
set -euo pipefail

root=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
matrix="$root/scripts/ONNX/accelerator-images.json"
hashed_files=(
  Dockerfile.onnx-deps
  scripts/ONNX/build_dependencies.sh
  scripts/ONNX/dependencies.lock.tsv
)

usage() {
  sed -n '2,3p' "$0" | sed 's/^# //' >&2
  exit 2
}

sha256() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | awk '{print $1}'
  else
    shasum -a 256 "$1" | awk '{print $1}'
  fi
}

target_entry() {
  jq -ce --arg profile "$1" --arg arch "$2" '
    first(.include[], .dependency_images.cpu[]
      | select(.profile == $profile and .arch == $arch))
  ' "$matrix" || {
    echo "No ONNX dependency target for $1/$2 in $matrix" >&2
    exit 2
  }
}

manifest() {
  local entry file
  entry=$(target_entry "$1" "$2")
  printf 'schema mist-onnx-deps/v1\n'
  for file in "${hashed_files[@]}"; do
    printf 'file %s %s\n' "$file" "$(sha256 "$root/$file")"
  done
  printf 'target %s\n' "$(jq -cS '
    {platform, profile, build_base, runtime_distribution, cuda_architectures,
     cuda_home, cudnn_home, tensorrt_home, openvino_dir}
    | map_values(. // "" | tostring)
  ' <<<"$entry")"
}

tag() {
  local digest
  digest=$(manifest "$1" "$2" | { sha256sum 2>/dev/null || shasum -a 256; } | awk '{print $1}')
  printf '%s-%s-%s\n' "$1" "$2" "${digest:0:32}"
}

repository() {
  jq -re '.dependency_images.repository' "$matrix"
}

command=${1:-}
case "$command" in
  tag|ref|manifest)
    [ "$#" -eq 3 ] || usage
    case "$command" in
      tag) tag "$2" "$3" ;;
      ref) printf '%s:%s\n' "$(repository)" "$(tag "$2" "$3")" ;;
      manifest) manifest "$2" "$3" ;;
    esac
    ;;
  plan)
    [ "$#" -eq 1 ] || usage
    repo=$(repository)
    while IFS= read -r entry; do
      profile=$(jq -r .profile <<<"$entry")
      arch=$(jq -r .arch <<<"$entry")
      image_tag=$(tag "$profile" "$arch")
      jq -c --arg key "$profile-$arch" --arg tag "$image_tag" --arg ref "$repo:$image_tag" \
        '. + {key: $key, tag: $tag, ref: $ref}' <<<"$entry"
    done < <(jq -c '.include[], .dependency_images.cpu[]' "$matrix") | jq -cs .
    ;;
  *) usage ;;
esac
