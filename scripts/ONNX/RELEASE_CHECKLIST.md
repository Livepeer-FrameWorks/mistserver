# ONNX release checklist

The first supported release target is the statically packaged CPU profile on Linux and
macOS. Accelerator builds are separate artifacts because their runtime and driver
requirements differ. Windows remains outside the first release gate.

## Build contract

- Build the dependency revisions in `dependencies.lock.tsv` as static, position-independent
  libraries. Only the OpenCV `core`, `imgproc`, `imgcodecs`, `video`, and `geometry` modules
  are required; do not bundle video I/O, DNN, GUI, Python, Java, examples, or tests.
- The ONNX Runtime package must provide a complete static `libonnxruntime` pkg-config link
  closure. Accelerator packages must include only the execution provider named by their
  release profile.
- Configure MistServer with `-DONNX=enabled -DONNX_STATIC=true
  -DONNX_PROFILE=cpu` (or the matching accelerator profile). An explicitly enabled build
  must fail configuration when either dependency is missing.
- Run `verify_release.sh --build-dir <dir> --model <yolo26n.onnx> --image <image>`.
  Do not use `--allow-shared` for a distributable artifact. The verifier rejects dynamic
  ONNX Runtime/OpenCV linkage and checks the capability, unit tests, curated adapter, raw
  tensor path, metadata-only default, annotation opt-in, and 64-bit timestamps.
- Archive `MistProcONNX`, its Mist libraries, `prepare_models.sh`,
  `models.manifest.tsv`, `dependencies.lock.tsv`, this checklist, and the applicable
  notices/SBOM. Models are cached data and are not embedded in the executable.

## Required release matrix

| Gate | Linux CPU | macOS CPU | macOS CoreML | CUDA/TensorRT | OpenVINO |
|---|---:|---:|---:|---:|---:|
| Compile and ONNX unit tests | required | required | required | required | required |
| Static dependency audit | required | required | required | required | required |
| YOLO26n image result | required | required | required | required | required |
| Raw ONNXTENSOR round trip | required | required | required | required | required |
| Parakeet INT8 real audio | required | required | optional | required | required |
| Ten-minute stream soak | required | required | required | required | required |

Each accelerator artifact must fail clearly when a user explicitly requests an execution
provider that was not packaged. CPU fallback within the selected profile is acceptable;
silently loading a different accelerator is not.

## Runtime acceptance

- Default YOLO26n: metadata only, maximum 5 inference FPS, one inference thread, tracking,
  scene-change detection, Kalman filtering, embedding payloads, hot spinning, elevated
  priority, and JPEG annotation all disabled.
- Annotations enabled: valid JPEG at quality 80 with boxes aligned to the emitted normalized
  `bbox`; disabling annotations must emit no JPEG track or JPEG bytes.
- Results: `mist.onnx.result/v1`, unsigned 64-bit `timestamp_ms`, stable output-track identity,
  bounded latest-frame queues, and visible receive/drop/skip/inference/timing counters.
- Tensor mode: named multi-input and multi-output tensors round-trip without truncation;
  malformed, oversized, wrong-dtype, and wrong-shape packets are rejected without a crash.
  Ordered input/output FIFOs remain bounded and expose their depth and drop counters.
- Parakeet INT8: 16 kHz PCM through MistProcAV, 5 s target / 1.5 s minimum / 10 s maximum
  chunks, 30 s bounded backlog, correct timestamps, final-tail flush, and real-time factor
  below 1.0 on the declared reference CPU.
- Soak: ingest a file containing people and continuous speech for at least ten minutes.
  Record CPU, peak RSS, queue drops, inference latency, result cadence, ASR real-time factor,
  and output-track count. Memory must plateau and track count must remain stable across a
  process restart.
- Compressed vision input: H264/AV1 is decoded once by MistProcAV to tightly packed NV12
  (processing-only mask) before ONNX. Direct compressed input must remain idle rather than
  repeatedly loading and restarting the model process.

## Model distribution

- Only artifacts in `models.manifest.tsv` are release packs. Every URL is immutable and every
  file is verified by byte size and SHA-256 before atomic cache promotion.
- Preserve model provenance, licenses, model-card attribution, and the manifest in any
  redistributed pack. YOLO26n is the seamless no-Python default; other YOLO entries remain
  developer exports until their exact artifacts are promoted into the manifest.
- The default YOLO offering uses the open-source AGPL lane—no enterprise grant. Follow
  `AGPL_DISTRIBUTION.md`, ship the complete AGPL-3.0 text, publish complete corresponding
  source for the combined offering, and provide network users access to source for the
  running version. Parakeet packs retain their CC-BY-4.0 attribution.

## Current blockers before tagging

- Produce and verify the static Linux and macOS dependency packages from the locked commits.
- Add the ONNX release artifact to the official container/release pipeline; the generic
  Alpine image does not currently contain ONNX Runtime or OpenCV.
- Run the Parakeet real-audio gate on each static release artifact and run the ten-minute
  mixed-media soak gate.
- Add an “ONNX vision” UI preset that creates the MistProcAV NV12 + MistProcONNX graph, or
  explicitly accept the documented two-stage process as the first-release UX. Do not add a
  second compressed-video decoder to MistProcONNX.
- Repeat the full-tree build without the audit directory's temporary global OpenCV 5 header
  include; that workaround exposes the `cv` namespace to unrelated tests and is not a valid
  release configuration.
