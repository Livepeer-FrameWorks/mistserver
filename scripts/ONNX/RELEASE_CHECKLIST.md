# ONNX release checklist

The portable release target is the CPU profile on Linux: static on amd64 and a bundled,
checksum-pinned shared closure on arm64. macOS arm64 is a
first-class native CoreML package with CPU fallback, deployment contract, signing, and
notarization. Linux amd64 CUDA, TensorRT, and OpenVINO builds emit both container images and native
shared-provider artifacts because their runtime and driver requirements differ. Windows remains
outside the first release gate.

## Build contract

- Build the dependency revisions in `dependencies.lock.tsv` with
  `build_dependencies.sh`. Only the OpenCV `core`, `imgproc`, `imgcodecs`, `video`, and
  `geometry` modules are required; do not bundle video I/O, DNN, GUI, Python, Java,
  examples, or tests.
- Linux amd64 CPU packages must provide the generated complete static `libonnxruntime`
  pkg-config closure. Linux arm64 CPU packages use Microsoft's checksum-pinned official
  AArch64 archive and bundle its shared runtime plus the exact shared OpenCV closure.
  CoreML packages use the checksum-pinned official macOS arm64 ONNX Runtime archive and include
  the exact shared ONNX Runtime/OpenCV dylib closure with an executable-relative rpath. CUDA,
  TensorRT, and OpenVINO packages use shared ONNX Runtime/OpenCV and expose only their declared
  accelerator set plus CPU fallback (TensorRT also includes its CUDA fallback). CUDA/TensorRT
  builds require `CUDA_HOME`, `CUDNN_HOME`, and (for TensorRT)
  `TENSORRT_HOME`; headless CUDA builds also require an explicit
  `ONNX_CUDA_ARCHITECTURES` list. OpenVINO requires `OpenVINO_DIR` pointing at the directory
  containing `OpenVINOConfig.cmake`. Release CI takes these values from the digest-pinned
  `accelerator-images.json` matrix.
- Configure MistServer with `-DONNX=enabled`, the exact `ONNX_PROFILE`, and
  `ONNX_STATIC=false` for Linux CPU, CoreML, and Linux accelerator release profiles. Linux CPU
  bundles use the matching checksum-pinned x64 or aarch64 runtime distribution and must package
  its audited shared-library closure.
  An explicitly enabled build must fail configuration when either dependency or its static
  closure is missing.
- Accelerator images must remain complete MistServer distributions. Package the complete Meson
  install tree; changing the ONNX provider must not replace it with a reduced ONNX appliance.
  NVIDIA images must also expose the H.264/AV1 NVENC encoders and CUVID decoders that
  `MistProcAV` selects for accelerated transcoding.
- Emit a native tarball from each exact platform image. It must contain the full MistServer
  `bin/` and `lib/` trees, `/opt/mist-onnx`, notices, `runtime-linkage.txt`, and the versioned
  `deployment-contract.json`. Bare-metal installers must verify the declared OS/architecture,
  system packages, provider runtime, devices, and loader paths before installation.
- Run `verify_release.sh --build-dir <dir> --model <yolo26n.onnx> --image <image>`.
  Pass `--expected-profile` and `--provider`. Use `--allow-shared` only for Linux arm64 CPU,
  the bundled CoreML closure, and SDK-bound Linux accelerator bundles. The verifier checks the advertised
  profile, explicitly loads the requested provider, and exercises the unit tests, curated
  adapter, raw tensor path, metadata-only default, annotation opt-in, and 64-bit timestamps.
- Archive the complete MistServer binary/library set, `MistProcONNX`, `prepare_models.sh`,
  `models.manifest.tsv`, `dependencies.lock.tsv`, this checklist, and the applicable
  notices/SBOM. Models are cached data and are not embedded in the executable.

## Required release matrix

| Gate | Linux CPU | macOS CoreML + CPU fallback | CUDA/TensorRT | OpenVINO |
|---|---:|---:|---:|---:|
| Compile and ONNX unit tests | required | required | required | required |
| Dependency/linkage audit | static amd64 / bundled arm64 | bundled dylib closure | shared SDK closure | shared SDK closure |
| Explicit provider load + YOLO26n image | CPU | CoreML | requested EP | OpenVINO |
| Raw ONNXTENSOR round trip | required | required | required | required |
| Parakeet INT8 real audio | required | optional | required | required |
| Ten-minute stream soak | required | required | required | required |

Each accelerator artifact must fail clearly when a user explicitly requests an execution
provider that was not packaged. CPU fallback within the selected profile is acceptable;
silently loading a different accelerator is not.

The compile, unit, capability, linkage, notice, and image-package gates run on ordinary
GitHub-hosted CPU runners and are sufficient to publish the digest-addressed platform image.
Physical-device execution is a separate status: the optional hardware jobs pull that exact image
and add provider-load, YOLO26n, and raw-tensor evidence without changing what was released.

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

## Remaining runtime qualification

- Accelerator platform images may be built, tagged, and published after their GitHub-hosted build
  and package gates pass. Complete the provider-load/model/soak gates on labelled CUDA, TensorRT,
  and OpenVINO hardware before marking a digest as hardware-qualified or recommending it for a
  production device class; source configuration alone is not hardware evidence.
- Run the Parakeet real-audio gate on each static release artifact and run the ten-minute
  mixed-media soak gate.
- Add an “ONNX vision” UI preset that creates the MistProcAV NV12 + MistProcONNX graph, or
  explicitly accept the documented two-stage process as the first-release UX. Do not add a
  second compressed-video decoder to MistProcONNX.
