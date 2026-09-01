#!/bin/sh
# Prepare ONNX models for MistServer — single entry point.
#
# Pre-built models (YOLO26n, Depth Anything, SCRFD, ArcFace, RTMO, Parakeet) are plain downloads
# and are fetched here directly with curl/wget — no Python needed. Models that must be
# compiled/exported from PyTorch (other YOLO11 / YOLO26 / RT-DETR variants) are delegated to
# export_models.py, which needs Python + ultralytics.
#
# Downloads are PINNED to specific HuggingFace commit revisions (see REV_* below) so a
# curated model id always resolves to the exact same artifact — upstream can't silently
# change shapes/vocab/behaviour under a running deployment. To bump a model, fetch the new
# commit sha (`curl -sSL https://huggingface.co/api/models/<repo> | grep -o '"sha":"[^"]*"'`)
# and update the matching REV_* here.
#
# Usage:
#   ./prepare_models.sh [--dir DIR] <model-id> [<model-id> ...]
#   ./prepare_models.sh --list
#
#   --dir DIR   output directory. Default matches MistProcONNX's persistent platform
#               cache lookup; $MIST_MODEL_DIR overrides it.
#   --list      list download models here, then the compiled models from export_models.py

set -eu

SCRIPT_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
HF="https://huggingface.co"
MANIFEST="$SCRIPT_DIR/models.manifest.tsv"

# Default output dir matches MistProcONNX::getModelDir(): explicit override first,
# then the platform's persistent user cache, with /tmp only as a last resort.
if [ -n "${MIST_MODEL_DIR:-}" ]; then
  DIR="$MIST_MODEL_DIR"
elif [ -n "${XDG_CACHE_HOME:-}" ]; then
  DIR="$XDG_CACHE_HOME/mistserver/onnx"
elif [ -n "${LOCALAPPDATA:-}" ]; then
  DIR="$LOCALAPPDATA/MistServer/onnx"
elif [ -n "${HOME:-}" ]; then
  DIR="$HOME/.cache/mistserver/onnx"
else
  _tmp="${TMP:-${TEMP:-${TMPDIR:-/tmp}}}"
  DIR="$_tmp/mist/models"
fi

# Pinned upstream revisions (commit SHAs).
REV_PARAKEET_ISTUPAKOV=8f23f0c03c8761650bdb5b40aaf3e40d2c15f1ce
REV_PARAKEET_FP16=dc9871ec5ad84a420940077e76e8741b3609bf8b
REV_DEPTH_SMALL=4472b7362082ad9968fee890ca0f1e5aca36b93d
REV_DEPTH_BASE=dd4557d492cd7b563738ac8d9ccff9094620983c
REV_DEPTH_LARGE=1fa1591c7b080e98da9655827c3a33a3972a4a83
REV_ANTELOPE=ba0c3e10f4548361eb9a63265d87ce1140ab5a05
REV_FACEFUSION=728b9659bd9691bf32cbf7f61af478d94b7ba81e
REV_RTMO_S=d8c526187f341d287753831c9c8b1ecc4855bba1
REV_RTMO_L=866da24c537be882180eb1bd278bc22cef85c6fd
REV_NSFW_VIT=1ceb3c7fe1e9f3f2507e6df577437f23a9149fd5
REV_VIOLENCE_VIT=c04818da5a78f241275f7b58184b24e2f15e3265
REV_CLIP_VITB32=d15189d7028b43f1d3e65039190477f6af591c2a
REV_SILERO_VAD=e71cae966052b992a7eca6b17738916ce0eca4ec
REV_WAV2VEC2_EMOTION=8aad397ba03ded6b7613c178ed708c91a9081b6b
REV_AST_AUDIOSET=2fe67046c486b8b8bc90a7b469239b2dec822f72
REV_WESPEAKER=6a61a1833ff2583aabeba044f5c8221f00b67ceb
REV_PPOCR=7b02d0a30a07ba2b92ad1ff5a8941ae2c633de65
REV_AGE_GENDER=6c138f6454d37dd55e5d4648e23e1ec23844e705
# NudeNet is fetched from a GitHub release; the tag pins it (release assets are immutable).
NUDENET_TAG=v3.4-weights

fetch() {
  # $1 = url, $2 = output path. Skips if the file already exists.
  if [ -f "$2" ]; then echo "    Skip $(basename "$2") (already exists)"; return 0; fi
  echo "    Downloading $(basename "$2") ..."
  if command -v curl >/dev/null 2>&1; then
    curl -fL --retry 3 -o "$2.partial" "$1" && mv "$2.partial" "$2"
  elif command -v wget >/dev/null 2>&1; then
    wget -O "$2.partial" "$1" && mv "$2.partial" "$2"
  else
    echo "Neither curl nor wget is available" >&2; exit 1
  fi
}

sha256_file() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | awk '{print $1}'
  elif command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "$1" | awk '{print $1}'
  elif command -v openssl >/dev/null 2>&1; then
    openssl dgst -sha256 "$1" | awk '{print $NF}'
  else
    echo "No SHA-256 implementation found (sha256sum, shasum or openssl required)" >&2
    return 1
  fi
}

verify_file() {
  file="$1"; expected_hash="$2"; expected_size="$3"
  [ -f "$file" ] || return 1
  actual_size=$(wc -c < "$file" | tr -d ' ')
  [ "$actual_size" = "$expected_size" ] || return 1
  actual_hash=$(sha256_file "$file") || return 1
  [ "$actual_hash" = "$expected_hash" ]
}

fetch_verified() {
  # Content-addressed, atomic and concurrency-safe download from models.manifest.tsv.
  # Existing invalid data is preserved as .corrupt.<pid> for diagnosis/recovery.
  url="$1"; dest="$2"; expected_hash="$3"; expected_size="$4"
  mkdir -p "$(dirname "$dest")"
  if verify_file "$dest" "$expected_hash" "$expected_size"; then
    echo "    Verified $(basename "$dest")"
    return 0
  fi

  lock="$dest.lock"
  attempts=0
  until mkdir "$lock" 2>/dev/null; do
    attempts=$((attempts + 1))
    if [ "$attempts" -ge 120 ]; then
      echo "Timed out waiting for model-cache lock: $lock" >&2
      return 1
    fi
    sleep 1
    if verify_file "$dest" "$expected_hash" "$expected_size"; then return 0; fi
  done
  (
    trap 'rmdir "$lock" 2>/dev/null || true' EXIT HUP INT TERM
    if verify_file "$dest" "$expected_hash" "$expected_size"; then exit 0; fi
    if [ -e "$dest" ]; then mv "$dest" "$dest.corrupt.$$"; fi
    partial="$dest.partial.$$"
    echo "    Downloading and verifying $(basename "$dest") ..."
    ok=0
    if command -v curl >/dev/null 2>&1; then
      curl -fL --retry 3 -o "$partial" "$url" && ok=1
    elif command -v wget >/dev/null 2>&1; then
      wget -O "$partial" "$url" && ok=1
    else
      echo "Neither curl nor wget is available" >&2
    fi
    if [ "$ok" -eq 1 ] && verify_file "$partial" "$expected_hash" "$expected_size"; then
      mv "$partial" "$dest"
      exit 0
    fi
    [ ! -e "$partial" ] || mv "$partial" "$partial.invalid"
    echo "Checksum or size verification failed for $dest" >&2
    exit 1
  )
}

provision_manifest() {
  id="$1"
  [ -r "$MANIFEST" ] || return 2
  if ! awk -F '\t' -v wanted="$id" '$1 == wanted { found=1 } END { exit !found }' "$MANIFEST"; then
    return 2
  fi
  echo "  Provisioning verified model pack '$id'..."
  awk -F '\t' -v wanted="$id" '$1 == wanted { print }' "$MANIFEST" |
  while IFS="$(printf '\t')" read -r _row_id relpath url digest bytes license source; do
    echo "    License: $license; source: $source"
    fetch_verified "$url" "$DIR/$relpath" "$digest" "$bytes" || exit $?
  done
}

dl_single() {
  # $1 = url, $2 = filename
  mkdir -p "$DIR"
  fetch "$1" "$DIR/$2"
}

dl_sidecar_model() {
  # Model + HF sidecar assets into a per-model subdirectory. MistProcONNX reads the
  # sidecars (config.json id2label -> class labels, preprocessor_config.json ->
  # normalization/resize) at model load, so they must live next to the .onnx.
  # $1 = repo (owner/name), $2 = revision, $3 = subdir, $4 = remote model path
  repo="$1"; rev="$2"; sub="$3"; remote="$4"
  dest="$DIR/$sub"
  mkdir -p "$dest"
  echo "  Provisioning $sub (model + sidecars)..."
  fetch "$HF/$repo/resolve/$rev/$remote" "$dest/model.onnx"
  for f in config.json preprocessor_config.json; do
    fetch "$HF/$repo/resolve/$rev/$f" "$dest/$f" || echo "    ($f not available upstream, skipping)"
  done
}

fetch_gh_asset() {
  # GitHub release asset download. The plain browser_download_url can serve an HTML
  # interstitial to non-browser agents, so resolve the asset id via the API and fetch
  # it with Accept: application/octet-stream (the documented binary endpoint).
  # $1 = owner/repo, $2 = tag, $3 = asset name, $4 = output path
  if [ -f "$4" ]; then echo "    Skip $(basename "$4") (already exists)"; return 0; fi
  mkdir -p "$(dirname "$4")"
  echo "    Downloading $3 ($1 @ $2) ..."
  api="https://api.github.com/repos/$1/releases/tags/$2"
  # Guard every network step against set -e: a failed API fetch or download must fall
  # through to the gh fallback / error message, not kill the whole script.
  reljson=""
  if command -v curl >/dev/null 2>&1; then
    reljson=$(curl -fsSL "$api" 2>/dev/null) || reljson=""
  elif command -v wget >/dev/null 2>&1; then
    reljson=$(wget -qO- "$api" 2>/dev/null) || reljson=""
  fi
  aid=$(printf '%s\n' "$reljson" | awk -v name="\"name\": \"$3\"" '
    /"url": ".*\/releases\/assets\/[0-9]+"/ { id = $0; gsub(/.*assets\//, "", id); gsub(/".*/, "", id) }
    index($0, name) { print id; exit }')
  if [ -n "$aid" ]; then
    asset="https://api.github.com/repos/$1/releases/assets/$aid"
    if command -v curl >/dev/null 2>&1; then
      if curl -fL --retry 3 -H "Accept: application/octet-stream" -o "$4.partial" "$asset"; then
        mv "$4.partial" "$4"
        return 0
      fi
    elif command -v wget >/dev/null 2>&1; then
      if wget --header="Accept: application/octet-stream" -O "$4.partial" "$asset"; then
        mv "$4.partial" "$4"
        return 0
      fi
    fi
  fi
  rm -f "$4.partial"
  # Fallback: the gh CLI handles auth/redirects itself.
  if command -v gh >/dev/null 2>&1; then
    gh release download "$2" -R "$1" -p "$3" -O "$4" && return 0
  fi
  echo "Failed to download $3 from $1@$2 (GitHub API fetch failed; gh CLI fallback unavailable or failed too)" >&2
  return 1
}

write_nudenet_labels() {
  # NudeNet has no labels asset upstream; class order is fixed by the model
  # (nudenet/nudenet.py __labels in the NudeNet repo). One label per line, index = id.
  [ -f "$DIR/nudenet/labels.txt" ] && return 0
  mkdir -p "$DIR/nudenet"
  cat > "$DIR/nudenet/labels.txt" <<'EOF'
FEMALE_GENITALIA_COVERED
FACE_FEMALE
BUTTOCKS_EXPOSED
FEMALE_BREAST_EXPOSED
FEMALE_GENITALIA_EXPOSED
MALE_BREAST_EXPOSED
ANUS_EXPOSED
FEET_EXPOSED
BELLY_COVERED
FEET_COVERED
ARMPITS_COVERED
ARMPITS_EXPOSED
FACE_MALE
BELLY_EXPOSED
MALE_GENITALIA_EXPOSED
ANUS_COVERED
FEMALE_BREAST_COVERED
BUTTOCKS_COVERED
EOF
}

dl_bundle() {
  # $1 = repo (owner/name), $2 = revision, $3 = subdir, remaining args = filenames
  repo="$1"; rev="$2"; sub="$3"; shift 3
  dest="$DIR/$sub"
  mkdir -p "$dest"
  echo "  Provisioning $sub (bundle)..."
  for f in "$@"; do fetch "$HF/$repo/resolve/$rev/$f" "$dest/$f"; done
}

# Model ids this script downloads directly (everything else -> export_models.py).
DOWNLOAD_IDS="depth-anything-v2-small depth-anything-v2-base depth-anything-v2-large \
scrfd-10g arcface-w600k-r50 rtmo-s rtmo-l \
nsfw-vit violence-vit nudenet-320n nudenet-640m clip-vitb32-vision clip-vitb32-text age-gender \
silero-vad wav2vec2-emotion ast-audioset wespeaker-resnet34 ppocr-v5-en \
parakeet-tdt-0.6b-int8 parakeet-tdt-0.6b-fp16 parakeet-tdt-0.6b-fp32"

provision_one() {
  id="$1"
  if provision_manifest "$id"; then return 0; else manifest_status=$?; fi
  if [ "$manifest_status" -ne 2 ]; then return "$manifest_status"; fi
  case "$id" in
    depth-anything-v2-small) dl_single "$HF/onnx-community/depth-anything-v2-small/resolve/$REV_DEPTH_SMALL/onnx/model.onnx" "depth-anything-v2-small.onnx" ;;
    depth-anything-v2-base)  dl_single "$HF/onnx-community/depth-anything-v2-base/resolve/$REV_DEPTH_BASE/onnx/model.onnx"  "depth-anything-v2-base.onnx" ;;
    depth-anything-v2-large) dl_single "$HF/onnx-community/depth-anything-v2-large/resolve/$REV_DEPTH_LARGE/onnx/model.onnx" "depth-anything-v2-large.onnx" ;;
    scrfd-10g)               dl_single "$HF/DIAMONIK7777/antelopev2/resolve/$REV_ANTELOPE/scrfd_10g_bnkps.onnx" "scrfd-10g.onnx" ;;
    arcface-w600k-r50)       dl_single "$HF/facefusion/models-3.0.0/resolve/$REV_FACEFUSION/arcface_w600k_r50.onnx" "arcface-w600k-r50.onnx" ;;
    rtmo-s)                  dl_single "$HF/Xenova/RTMO-s/resolve/$REV_RTMO_S/onnx/model.onnx" "rtmo-s.onnx" ;;
    rtmo-l)                  dl_single "$HF/Xenova/RTMO-l/resolve/$REV_RTMO_L/onnx/model.onnx" "rtmo-l.onnx" ;;
    # Content moderation. The ViTs ship HF sidecars (labels + preprocessing); NudeNet
    # (AGPL-3.0, ultralytics lineage — check distribution requirements) gets its labels
    # written here since the GitHub release carries only the .onnx files.
    nsfw-vit)     dl_sidecar_model "onnx-community/nsfw_image_detection-ONNX" "$REV_NSFW_VIT" "nsfw_vit" "onnx/model.onnx" ;;
    violence-vit) dl_sidecar_model "onnx-community/vit-base-violence-detection-ONNX" "$REV_VIOLENCE_VIT" "violence_vit" "onnx/model.onnx" ;;
    nudenet-320n) write_nudenet_labels; fetch_gh_asset "notAI-tech/NudeNet" "$NUDENET_TAG" "320n.onnx" "$DIR/nudenet/320n.onnx" ;;
    nudenet-640m) write_nudenet_labels; fetch_gh_asset "notAI-tech/NudeNet" "$NUDENET_TAG" "640m.onnx" "$DIR/nudenet/640m.onnx" ;;
    # CLIP vision tower (image embeddings). The split-tower export is the right
    # artifact — the combined model.onnx demands text inputs even for image-only use.
    clip-vitb32-vision) dl_sidecar_model "Xenova/clip-vit-base-patch32" "$REV_CLIP_VITB32" "clip_vitb32" "onnx/vision_model.onnx" ;;
    # Face age/gender attribute head (secondary model on SCRFD face crops)
    age-gender) dl_sidecar_model "onnx-community/age-gender-prediction-ONNX" "$REV_AGE_GENDER" "age_gender" "onnx/model.onnx" ;;
    # CLIP text tower + tokenizer, for the offline zero-shot label embedder
    # (clip_text_embeddings.py). Kept in its OWN dir so its .onnx doesn't sit next to
    # the vision model.onnx — a second .onnx there would trip the sole-model-directory
    # sidecar guard and strip the vision model's preprocessing config.
    clip-vitb32-text)
      dest="$DIR/clip_vitb32_text"
      mkdir -p "$dest"
      echo "  Provisioning clip_vitb32_text tower + tokenizer..."
      fetch "$HF/Xenova/clip-vit-base-patch32/resolve/$REV_CLIP_VITB32/onnx/text_model.onnx" "$dest/text_model.onnx"
      fetch "$HF/Xenova/clip-vit-base-patch32/resolve/$REV_CLIP_VITB32/tokenizer.json" "$dest/tokenizer.json"
      ;;
    # Single-file audio models (VAD / emotion). Silero has no HF sidecars; the emotion
    # model's config.json (labels) + preprocessor_config.json (rate, normalization) do
    # the configuring.
    silero-vad)       dl_sidecar_model "onnx-community/silero-vad" "$REV_SILERO_VAD" "silero_vad" "onnx/model.onnx" ;;
    wav2vec2-emotion) dl_sidecar_model "onnx-community/wav2vec2-base-Speech_Emotion_Recognition-ONNX" "$REV_WAV2VEC2_EMOTION" "wav2vec2_emotion" "onnx/model.onnx" ;;
    ast-audioset)     dl_sidecar_model "onnx-community/ast-finetuned-audioset-10-10-0.4593-ONNX" "$REV_AST_AUDIOSET" "ast_audioset" "onnx/model.onnx" ;;
    wespeaker-resnet34) dl_sidecar_model "onnx-community/wespeaker-voxceleb-resnet34-LM" "$REV_WESPEAKER" "wespeaker_resnet34" "onnx/model.onnx" ;;
    # OCR: PP-OCRv5 text detection + English recognition + charset (three-file vision bundle)
    ppocr-v5-en)
      dest="$DIR/ppocr_v5_en"
      mkdir -p "$dest"
      echo "  Provisioning ppocr_v5_en (det + rec + dict)..."
      fetch "$HF/monkt/paddleocr-onnx/resolve/$REV_PPOCR/detection/v5/det.onnx" "$dest/det.onnx"
      fetch "$HF/monkt/paddleocr-onnx/resolve/$REV_PPOCR/languages/english/rec.onnx" "$dest/rec.onnx"
      fetch "$HF/monkt/paddleocr-onnx/resolve/$REV_PPOCR/languages/english/dict.txt" "$dest/dict.txt"
      ;;
    parakeet-tdt-0.6b-int8)  dl_bundle "istupakov/parakeet-tdt-0.6b-v3-onnx" "$REV_PARAKEET_ISTUPAKOV" "parakeet-tdt-0.6b-int8" nemo128.onnx encoder-model.int8.onnx decoder_joint-model.int8.onnx vocab.txt ;;
    parakeet-tdt-0.6b-fp16)  dl_bundle "grikdotnet/parakeet-tdt-0.6b-fp16" "$REV_PARAKEET_FP16" "parakeet-tdt-0.6b-fp16" nemo128.onnx encoder-model.fp16.onnx decoder_joint-model.fp16.onnx vocab.txt ;;
    parakeet-tdt-0.6b-fp32)  dl_bundle "istupakov/parakeet-tdt-0.6b-v3-onnx" "$REV_PARAKEET_ISTUPAKOV" "parakeet-tdt-0.6b-fp32" nemo128.onnx encoder-model.onnx encoder-model.onnx.data decoder_joint-model.onnx vocab.txt ;;
    *)
      # Not a download model — hand off to the Python exporter (needs ultralytics).
      echo "  '$id' needs compiling; delegating to export_models.py ..."
      python3 "$SCRIPT_DIR/export_models.py" --dir "$DIR" "$id"
      ;;
  esac
}

# --- argument parsing ---
MODELS=""
while [ $# -gt 0 ]; do
  case "$1" in
    --dir) DIR="$2"; shift 2 ;;
    --list)
      echo "Download models (no dependencies, fetched by prepare_models.sh):"
      for m in $DOWNLOAD_IDS; do echo "    $m"; done
      echo
      echo "Compiled models (need Python + ultralytics, via export_models.py):"
      python3 "$SCRIPT_DIR/export_models.py" --list 2>/dev/null || echo "    (run: python3 export_models.py --list)"
      exit 0 ;;
    -h|--help) sed -n '2,22p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) MODELS="$MODELS $1"; shift ;;
  esac
done

if [ -z "$MODELS" ]; then
  echo "No models specified. Use --list to see options, or pass model ids." >&2
  exit 1
fi

echo "Output directory: $DIR"
for id in $MODELS; do provision_one "$id"; done
echo "Done."
