# ONNX Model Provisioning for MistServer

This directory holds the model-provisioning scripts and signed-off artifact manifest used
by `MistProcONNX`. Models are stored in the persistent platform cache described below.

**One entry point: `prepare_models.sh`.** It downloads pre-built models directly (no
dependencies) and delegates compiled models to `export_models.py` (which needs Python +
ultralytics). You only touch `export_models.py` directly if you want to.

## Quick Start

```bash
# List everything (downloads + compiled), see what's available
./prepare_models.sh --list

# Pre-built models — pure download, no dependencies
./prepare_models.sh depth-anything-v2-small scrfd-10g arcface-w600k-r50 rtmo-s
./prepare_models.sh parakeet-tdt-0.6b-int8          # speech-to-text (see below)

# Release default — pinned, checksum-verified ONNX; no Python needed
./prepare_models.sh yolo26n

# Developer exports not yet present in the verified release manifest
./prepare_models.sh yolo11n yolo26s

# Custom output directory (or set MIST_MODEL_DIR)
./prepare_models.sh --dir /opt/mist/models yolo11n
```

### Developer exports need ultralytics

The release-default `yolo26n` is fetched as a pinned, SHA-256-verified ONNX artifact and
needs no Python. Its split logits/box outputs use YOLO26's native end-to-end, NMS-free
postprocessing path. Other YOLO11 / YOLO26 / RT-DETR variants currently use the developer
export path, which needs the `ultralytics` package:

```bash
python3 -m venv venv && source venv/bin/activate
pip install ultralytics onnx onnxslim onnxruntime

./prepare_models.sh yolo11n          # via the single entry point
python3 export_models.py yolo11n     # or the exporter directly
python3 export_models.py --category detect   # all detection models
deactivate
```

## Models

| Category | Models | Method | Dependencies |
|----------|--------|--------|--------------|
| Detection | yolo26n | verified download | none |
| Detection | yolo11n/s/m/l/x, yolo26s/m/l/x, rtdetr-l/x | developer export | ultralytics |
| Segmentation | yolo11n/s/m/l/x-seg, yolo26n/s/m/l/x-seg | export | ultralytics |
| Pose | yolo11n/s/m/l/x-pose, yolo26n/s/m/l/x-pose | export | ultralytics |
| Classification | yolo11n/s/m/l/x-cls, yolo26n/s/m/l/x-cls | export | ultralytics |
| OBB | yolo11n/s/m/l/x-obb, yolo26n/s/m/l/x-obb | export | ultralytics |
| Depth | depth-anything-v2-small/base/large | download | none |
| Face Detection | scrfd-10g | download | none |
| Face Recognition | arcface-w600k-r50 | download | none |
| Multi-person Pose | rtmo-s/l | download | none |
| Content Moderation | nsfw-vit, violence-vit, nudenet-320n/640m | download (+ sidecars) | none |
| Image Embedding | clip-vitb32-vision (+ clip-vitb32-text for zero-shot) | download (+ sidecars) | none |
| Face Attributes | age-gender (secondary on scrfd-10g) | download (+ sidecars) | none |
| Audio Analysis | silero-vad, wav2vec2-emotion, ast-audioset, wespeaker-resnet34 | download (+ sidecars) | none |
| OCR | ppocr-v5-en | download (bundle) | none |
| Transcription | parakeet-tdt-0.6b-int8/fp16/fp32 | download (bundle) | none |

The verified release manifest currently covers the automatic `yolo26n` default and all
three Parakeet bundles. Other registry entries retain their pinned source/export recipes;
promote one to release status by adding every artifact, byte size, SHA-256, license and
source URL to `models.manifest.tsv`.

## Running computer vision (two-stage chain)

Model provisioning is automatic, but compressed media decoding remains an explicit,
shared Mist process. `MistProcONNX` accepts JPEG or raw YUYV/UYVY/NV12/I420 frames; it does
not embed a second H264/AV1 decoder. For a normal compressed stream, configure:

1. **MistProcAV** — `Input type = video`, `codec = NV12`, `track_select =
   video=H264&audio=none`, and `target_mask = 4` (processing tasks). NV12 is preferred over
   UYVY because its tightly packed 4:2:0 frames use 25% less inter-process bandwidth.
2. **MistProcONNX** — `model = yolo26n`. Its safe default selector accepts only
   `YUYV/UYVY/NV12/I420/JPEG`, so a compressed source cannot trigger a model-load/restart
   loop. It emits `mist.onnx.result/v1` detection packets on a JSON metadata track.

Equivalent process configuration:

```json
[
  {"process":"AV", "x-LSP-kind":"video", "codec":"NV12",
   "track_select":"video=H264&audio=none", "target_mask":4},
  {"process":"ONNX", "model":"yolo26n",
   "track_select":"video=NV12&audio=none"}
]
```

This separation keeps FFmpeg/LibAV out of `MistProcONNX`, avoids duplicate decode work, and
lets one decoded processing track feed multiple analysis processes. A release UI may wrap
the two entries in an “ONNX vision” preset, but must preserve this graph internally.

### Face age/gender

`age-gender` is a face-attribute head best run as a **secondary model** on SCRFD face
crops. Its output is `[age_years, gender_prob]`; each face detection gains `age` (int),
`gender` (`male`/`female`), and `gender_prob`. Chain it:

```
model = scrfd-10g
secondary_model_path = age-gender
secondary_model_type = age-gender
```

Gender polarity follows the UTKFace convention (`gender_prob` = P(female), so ≥ 0.5 →
`female`). If a labeled sample shows it inverted, swap the `gender_high_label` /
`gender_low_label` options (defaults `female` / `male`) — no rebuild. Apache-2.0.

### OCR (PP-OCRv5)

`ppocr-v5-en` is a **vision bundle** (detection + recognition + charset) that reads
on-screen text — lower thirds, scoreboards, slates. It runs on the normal video path
(honours `process_every_nth` since text changes slowly) and emits JSON on the meta
track: a `text` field (all lines joined) plus a `lines` array of `{text, confidence,
x, y, w, h}` in reading order. Detection input is dynamic (scaled so the long side
≤ 960, rounded to a multiple of 32); recognition is CTC-decoded against the bundled
`dict.txt`. Provision with `prepare_models.sh ppocr-v5-en` (or select it — missing
bundles auto-provision).

### Audio analysis models

Like transcription, these consume raw PCM — chain `MistProcAV` in front (Input
type=audio, codec=PCM, sample_rate=16000). Single-file models via the generic
`AudioModel` adapter:

- `silero-vad` — streaming voice activity detection (512-sample chunks, recurrent
  state). Emits `{"event": {"label": "speech", "state": "started"/"ended"}}` packets on
  the meta track (options: `event_enter`/`event_exit`/`event_min_ms`/`event_ema`) plus
  a smoothed score packet once per second.
- `wav2vec2-emotion` — pause-windowed speech emotion (SAD/ANGRY/DISGUST/FEAR/HAPPY/
  NEUTRAL), softmax scores per window.
- `ast-audioset` — pause-windowed audio event tagging over 527 AudioSet classes
  (gunshot, siren, applause, music, ...), multi-label sigmoid scores. Uses the built-in
  kaldi-style log-mel frontend (128 bins, hanning window, HF/AST normalization).
- `wespeaker-resnet34` — pause-windowed 256-d speaker embedding (80-bin fbank, povey
  window, per-utterance mean normalization); cosine distance between windows detects
  speaker changes.

### Content moderation notes

- `nsfw-vit` (Apache-2.0) classifies whole frames as `normal`/`nsfw`; `nudenet-320n/640m`
  (**AGPL-3.0** — review distribution requirements) localizes 18 body-part classes with
  bounding boxes. They complement each other: score with the ViT, localize with NudeNet.
- `violence-vit` (Apache-2.0) ships **without class labels** upstream — its two classes
  report as `class_0`/`class_1` until the order is empirically calibrated. Once
  calibrated, drop a `labels.txt` (one label per line, index = class id) next to the
  model to name them.

## Zero-shot tagging (CLIP)

`clip-vitb32-vision` emits 512-d image embeddings. To turn it into a **tagger** with no
retraining, precompute text embeddings for your label set with the CLIP text tower and
drop the result next to the vision model:

```bash
./prepare_models.sh clip-vitb32-vision clip-vitb32-text   # vision + text tower + tokenizer
pip install onnxruntime tokenizers numpy
python3 clip_text_embeddings.py \
  --text-dir "$MIST_MODEL_DIR/clip_vitb32_text" \
  --out-dir  "$MIST_MODEL_DIR/clip_vitb32" \
  --labels "nsfw,safe,violence,weapon"
```

This writes `clip_vitb32/text_embeddings.json`. At load time the model finds it and, per
frame, ranks the image embedding against the labels by cosine similarity — emitting a
`classification` block (top-K) on the meta track, so `event_class` works on zero-shot
tags too. The text tower lives in its **own** directory (`clip_vitb32_text/`) so its
`.onnx` doesn't trip the sole-model-directory sidecar rule for the vision model.

## Model sidecars (labels & preprocessing as data)

A model's class labels and preprocessing constants live in *sidecar files* next to the
`.onnx`, not in C++ tables. `MistProcONNX` looks these up at model load (first hit wins),
for a model at `/path/to/<stem>.onnx`:

- **labels**: `<stem>.labels.txt` (one label per line, index = class id) →
  `labels.txt` → `config.json` (HuggingFace `id2label`)
- **preprocessing**: `<stem>.preprocessor.json` → `preprocessor_config.json`
  (HuggingFace format: `image_mean`/`image_std` → mean/std normalization, `size` →
  native input size; `size.height/width` → direct resize, `size.shortest_edge` or
  `do_center_crop` → scale the short edge then center-crop, CLIP-style — never
  letterboxed)

`prepare_models.sh` downloads the HF sidecars into a per-model subdirectory
(`nsfw_vit/model.onnx` + `config.json` + `preprocessor_config.json`); the generic file
names are safe there. For your own models in a shared flat directory, use the
`<stem>`-prefixed names (`mymodel.labels.txt`, `mymodel.preprocessor.json`). Custom
labels take precedence over the built-in COCO/ImageNet/DOTA tables in every parser;
without any labels source, the built-ins still apply (YOLO → COCO, classifiers →
ImageNet).

## Speech-to-text (Parakeet TDT)

Transcription models are audio, not vision: NVIDIA **Parakeet TDT 0.6B v3** is a
four-file bundle (mel preprocessor + Conformer encoder + TDT decoder/joint + vocab),
pre-built on HuggingFace. Provision it like any other download model:

```bash
# Downloads into scripts/ONNX/parakeet-tdt-0.6b-<variant>/ (respects MIST_MODEL_DIR)
./prepare_models.sh parakeet-tdt-0.6b-int8    # ~670 MB, fastest on CPU
./prepare_models.sh parakeet-tdt-0.6b-fp16    # ~1.3 GB, balanced
./prepare_models.sh parakeet-tdt-0.6b-fp32    # ~2.5 GB, most accurate (external-data encoder)
```

### Running transcription (two-stage chain, UNIX-style)

`MistProcONNX` consumes **raw PCM** and does no audio decoding itself. Chain it after
`MistProcAV`, which decodes the compressed audio to a 16 kHz PCM track on the same stream:

1. **MistProcAV** — `Input type = audio`, `codec = PCM`, `sample_rate = 16000`, `sink =`
   the source stream. This adds a `PCM` audio track to the stream.
2. **MistProcONNX** — `source =` that stream, `model = parakeet-tdt-0.6b-int8`. It
   auto-selects the PCM audio track (`audio=PCM&video=none`), downmixes to mono, and
   emits transcription JSON on a `meta`/`JSON` output track.

The JSON packets look like:

```json
{ "schema": "mist.onnx.result/v1", "timestamp_ms": 1600,
  "model": { "name": "parakeet-tdt-0.6b-int8" },
  "kind": "transcription", "status": "ok", "transcription": "hello world",
  "segments": [ { "start_ms": 1600, "end_ms": 1680, "text": " hello", "confidence": 0.97 } ] }
```

## Where models are stored

`MistProcONNX` reads/writes model files in this **writable** directory:

1. `MIST_MODEL_DIR` if set
2. `XDG_CACHE_HOME/mistserver/onnx` when set
3. `%LOCALAPPDATA%/MistServer/onnx` on Windows, or `~/.cache/mistserver/onnx` on Unix
4. `<tmp>/mist/models/` only when no persistent user-cache location is available

For shared or container deployments, point `MIST_MODEL_DIR` at durable storage:

```bash
export MIST_MODEL_DIR=/opt/mist/models
```

## Auto-provisioning (no manual step needed)

When you select a curated model in the process and it isn't on disk yet, `MistProcONNX`
**runs `prepare_models.sh` itself** (into the model dir) and then loads the model. Manifest
artifacts are downloaded under a per-file lock, verified by byte size and SHA-256, and
atomically renamed. Invalid existing or partial files are never trusted. The default
`yolo26n` and Parakeet packs therefore need no Python. Non-manifest developer exports are
delegated to `export_models.py` and clearly report a missing Python/ultralytics toolchain.

The model artifacts retain terms separate from MistServer's base source license. The
manifest records their license and provenance. YOLO26-derived weights/exports are AGPL-3.0;
Parakeet artifacts are CC-BY-4.0. Keep the manifest and model-card attribution with every
redistributed pack. This project takes the open-source YOLO route and assumes no enterprise
grant: distributors of the default combined offering must follow `AGPL_DISTRIBUTION.md`,
including complete corresponding source and the network-source obligation.

The scripts are installed next to the binaries (`<prefix>/bin`, e.g. `/usr/local/bin`), so
`MistProcONNX` locates them relative to itself. Override their location with
`MIST_ONNX_SCRIPTS` if needed. In a dev build they're found under `scripts/ONNX/` relative
to the binary in `build/`.

For custom models not in the dropdown, select "Custom model path" and provide the full
filesystem path.

## Raw tensor mode

Set `input_mode=tensor` to bypass all curated media preprocessing and postprocessing. The
process consumes and emits `meta/ONNXTENSOR` packets and supports arbitrary named,
multi-input/multi-output tensor graphs. Inputs are checked against the model's port names,
dtypes, ranks, and fixed dimensions before inference. Outputs retain their model port names,
dtypes, shapes, and complete byte payloads.

The binary packet starts with `MSTT`, version `1`, three reserved bytes, and a big-endian
32-bit JSON-header length. The header uses schema `mist.onnx.tensor/v1` and contains each
tensor's `name`, `dtype`, `shape`, byte `offset`, and byte `length`; contiguous tensor bytes
follow the header. Packets are capped at 64 MiB. Numeric tensor payloads are little-endian.
Input and output use bounded, ordered FIFOs (depth 8 by default, configurable from 1 to 64);
when a consumer falls behind the oldest packet is dropped and counted in process statistics.

## Release builds

ONNX support is a strict Meson feature when explicitly enabled. Release artifacts use
one `ONNX_PROFILE` (`cpu`, `coreml`, `cuda`, `tensorrt`, or `openvino`); the profile
constrains which execution provider may be selected at runtime. Linux amd64 CPU packages a
static ONNX Runtime/OpenCV closure. Linux arm64 CPU uses Microsoft's checksum-pinned AArch64
runtime plus a bundled shared OpenCV closure. CoreML uses Microsoft's checksum-pinned macOS arm64 runtime plus the
exact shared OpenCV libraries in the signed native bundle. CUDA, TensorRT, and OpenVINO use
shared provider builds because their target SDK and driver runtimes are part of the deployment
environment.

Build the exact dependency commits and then make ONNX mandatory:

```sh
scripts/ONNX/build_dependencies.sh \
  --prefix "$PWD/.deps/onnx/cpu" \
  --work-dir "$PWD/.build-deps/onnx/cpu" \
  --profile cpu
export PKG_CONFIG_PATH="$PWD/.deps/onnx/cpu/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
export CMAKE_PREFIX_PATH="$PWD/.deps/onnx/cpu:${CMAKE_PREFIX_PATH:-}"
meson setup build -DONNX=enabled -DONNX_STATIC=true -DONNX_PROFILE=cpu
```

Use `coreml` on macOS arm64 with `ONNX_STATIC=false` and the locked
`onnxruntime-osx-arm64` distribution. For `cuda`, `tensorrt`, or `openvino`, start in the
matching SDK environment and configure Meson with `ONNX_STATIC=false`. CUDA/TensorRT use
`CUDA_HOME`, `CUDNN_HOME`, `ONNX_CUDA_ARCHITECTURES`, and (for TensorRT)
`TENSORRT_HOME`; OpenVINO uses the `OpenVINO_DIR` containing `OpenVINOConfig.cmake`. Explicit
CUDA architectures are mandatory so a headless CI builder never tries to discover a local GPU.
The builder fails before configuration when a required SDK location is absent.

Linux CPU release builds use `ONNX_STATIC=false` with the locked `onnxruntime-linux-x64` or
`onnxruntime-linux-aarch64` distribution. Their native bundles package and audit the shared
runtime closure with executable-relative rpaths. The release workflow builds Linux amd64 CUDA,
TensorRT, and OpenVINO images on
ordinary GitHub-hosted CPU runners. Linux arm64 GPU-provider artifacts are intentionally outside
the first release matrix because upstream publishes no equivalent generic ARM64 GPU archive.
`accelerator-images.json` pins the vendor build and runtime image digests, SDK paths,
and dependency strategy. A manual workflow can select one
architecture for non-publishing diagnosis; publishing requires the complete architecture set so
an incomplete multi-arch manifest cannot replace a release tag. Those jobs run the
ONNX unit, capability, dependency, loader, notice, and package checks without claiming that a
physical accelerator was exercised. The separately opt-in hardware jobs pull the exact produced
image and run the pinned YOLO26n image and raw-tensor probes on labelled `onnx-cuda`,
`onnx-tensorrt`, or `onnx-openvino` runners. macOS arm64 is the CoreML target: the main release
workflow builds and exercises it on GitHub's hosted Apple Silicon runner, emits the full native
tarball and deployment contract, and passes that artifact through signing/notarization for tags.
Its staged `lib/` directory is an executable-relative rpath closure, while Homebrew supplies the
declared general media libraries. There is no macOS container image because Docker images use a
Linux kernel.

Each provider image is a complete stock MistServer image, not a reduced ONNX appliance. It
contains the controller, normal inputs and outputs, MistProcAV, NDI/camera support, and every
other target installed by the regular full build; only the ONNX provider/runtime layer and base
image differ. NVIDIA artifact audits also require the FFmpeg NVENC/CUVID H.264 and AV1 codecs used
by MistProcAV. Provider packaging archives the complete Meson install tree; NVIDIA-specific tests
also verify those hardware codec registrations.

Published tags are `<release>-onnx-cuda`, `<release>-onnx-tensorrt`, and
`<release>-onnx-openvino`; all three currently target Linux amd64. Digest and pinned base-image
metadata are attached to the GitHub release. Every platform job also emits a native
`mistserver-linux-<arch>-onnx-<profile>-<release>.tar.gz` from the exact same image filesystem.
It contains the complete `bin/` and `lib/` trees plus `/opt/mist-onnx`.

Native bundles contain `share/mistserver/onnx/deployment-contract.json` and a captured
`runtime-linkage.txt`. The versioned contract tells an edge installer the target Ubuntu/glibc
baseline, apt prerequisites, install destinations, loader paths, provider runtime/version,
device probes, CPU-fallback status, and digest-pinned reference runtime image. Vendor userspace
runtimes and kernel drivers remain external host prerequisites; a deployer such as the Frameworks
monorepo CLI can preflight them, install a supported vendor package set, write the declared loader
configuration, run `ldconfig`, and reject an incompatible host before replacing a live binary.

Linux accelerator images are separate because the OS/architecture alone does not define the GPU
vendor or driver ABI. Each image includes its matching userspace SDK runtime and keeps the shared
provider closure in `/opt/mist-onnx/lib`; the target host supplies only a compatible kernel driver
and device. The loader path is registered through `/etc/ld.so.conf.d/mist-onnx.conf` without
discarding vendor paths inherited from the runtime image.

All production ONNX containers are built from `Dockerfile.mistserver-onnx`; a build fails if
`MistProcONNX` or the advertised profile is missing. The exact source revisions are recorded in
`dependencies.lock.tsv`, and accelerator base images in `accelerator-images.json`. See
`RELEASE_CHECKLIST.md` and run `verify_release.sh` against every candidate artifact.
