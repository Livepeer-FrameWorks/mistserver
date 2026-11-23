# Round-9 R1: MKV B-frame timestamps in TS output

Scope: implement R1 from the round-9 handoff on local master. Preserve unrelated commits and the existing evaluator PLAN. R4/R5/R6/F12 are done. Never push or tag.

Starting heads: Mist 900b8c275; monorepo 818ce3730. No monorepo changes.

## Implementation and evidence

- Added a real HTTP TS regression using x264 Matroska with three B-frames. It checks all 150 PES timestamps directly, strictly increasing DTS, PTS >= DTS, HLS segment clock consistency, audio-only clock consistency and A/V presentation alignment.
- Red before the fix: `TS video PTS must be at least DTS`, 72 of 150 packets; first failure packet 3: PTS 3600, DTS 10800. Strictly increasing DTS passed. Both native and isolated Linux runs reproduced it.
- The first native build failed on a stale Homebrew libsrtp path. Meson's dependency cache was refreshed; that build failure is not product-red evidence. After discovering another native controller session on the laptop, all further integration runs used a dedicated Linux container with private IPC.
- Shared TS output now uses the minimum video composition offset to calculate one stream-wide presentation delay. Video DTS and PCR keep their decode clock; all tracks' PTS move together. AAC receives the delay after sample-clock rounding. HLS variants and audio-only requests use the same stream clock. MP4/CMAF signed-offset handling is unchanged.
- Green on Linux: 4/4 focused timestamp tests and 16/16 EBML/MPEG-TS suite tests. The existing MP4 test retains all decoded pixels and A/V alignment. The TS regression covers PES timestamps and the demuxable A/V prefix; it does not claim complete decoded-frame retention (see finding below).
- Changed-line clang-format 21.1.8 and `git diff --check` passed.
- Full logs/receipts: `/private/tmp/frameworks-round9-20261121/R1-evidence.json`, `R1-red-v2.log`, `R1-remote-red.log`, `R1-remote-green-v6.log`, `R1-remote-suites.log`.
- Remote source/build/logs: `/srv/frameworks-dev/workspaces/stronk/codex-r1-20261122-1618/`. Test image: `codex-r1-tools:20261122-1618`. Build: WITH_AV=false, ONNX disabled, SSL/SRT/RIST disabled. These are focused Linux results, not full release qualification.

## Separate finding retained for follow-up

At default TS EOF, removing the finished video track causes `TSOutput::pidMapper` to reuse video PID 255 for remaining audio. The captured stream has 150 video PES starts but FFprobe returns 149 video packets. A focused reproducer fails with `PID changed media type before EOF`, PID 255, video -> audio. Evidence and reproducer: `/private/tmp/frameworks-round9-20261121/TS-EOF-PID-red.log`, `TS-EOF-PID-repro.py`, `R1-debug-default.ts`. No PID-mapping change is included in R1; its exact impact on the final decoded frame still needs separate qualification. Fixed-PID experiment also yielded 149 demuxed video packets, so fixed PIDs alone have not been established as a complete EOF fix.

Sign-off: R1 implementation and focused Linux checks complete. Full handoff sign-off remains incomplete; release/ONNX/ASan/SRT, platform/stack/Molecule and hardware gates are outstanding. Next handoff item: R3 (resampler).


## R1 review follow-up (2026-11-22)

Baseline: Mist dba2bd77c, monorepo 31ab98db2. Other sessions advanced both repositories and rewrote the prior local series; their current commits are preserved. This follow-up supersedes the earlier fixed-file-only sign-off above.

### Changes and actual red evidence

1. Live TS/HLS clock: an actual HTTP MKV push uses x264 adaptive B-frames with verified negative-offset depth changes (40 -> 80 -> 40 ms). The pre-fix continuous TS run failed with `live TS presentation clock jumped as reorder depth changed`: gaps [3600, 7200] instead of 3600 ticks. Live clock initialization now uses the codec's declared maximum reorder depth and frame period, independently of the DVR window. VOD scans its indexed offsets only once. `getValidTracks()` is behind the initialization guard, not allocated per packet. H264 and HEVC use their SPS reorder bounds; HEVC metadata now exposes that already-parsed field. Underlying source liveness remains available when a finite HLS/output limiter presents its view as VOD.
2. Livepeer return timing: the real source/TS/FFmpeg-gateway/TS/sink test passed for return bf=0 but failed for bf=3 with an 80 ms source/rendition PTS difference. Each prepared segment now retains the source video's decode and composition anchors; the sink aligns both rather than only the first returned DTS. Returned audio remains unpublished and is discarded without per-packet waits. Both return modes now retain all 200 source frame times across eight independently re-encoded segments.
3. RTP signed offsets: the ordinary 100-frame MKV -> SDP/RTP integration already passed before the change. The actual SDP sendNext call-path boundary regression failed at DTS=40 ms, offset=-80 ms: expected wrapped RTP timestamp 4294963696, actual 0. SDP and RTSP now scale signed composition time and explicitly wrap to the 32-bit RTP clock. Both the boundary call path and real MKV/RTP test pass.
4. R22: 3ac588824 already fixed the PID-at-EOF defect. Preserved that commit and reran its strengthened default-PID regression: all 150 demuxed/decoded frames are retained with identical pixels. This is completed R22 work, not parked follow-up.

### Green qualification

- Six initial targeted review tests passed, including fixed-file R1/R22, SDP integration and signed sendNext, Livepeer bf=0/bf=3 and adaptive live H264.
- Final live-window tests: H264 and HEVC each pass continuous video/audio PTS, independent and overlapping HLS request clock agreement, segment-boundary continuity, and an explicit assertion that the DVR first video timestamp advances by more than 8 seconds, retiring the earlier deep-B-frame interval. Final fixture: 20 seconds, short configured fragments and a bounded live window. HEVC uses its explicit HLS variant because browser defaults exclude it. The runs fetched 545/546 independently addressed segment versions and compared thousands of overlapping packets.
- Final Livepeer tests: bf=0, bf=3 and the existing 30-second audio/VOD/drain regression all pass.
- Existing MP4 B-frame pixel/A/V alignment regression passes.
- All 22 related EBML, MPEG-TS, Livepeer and SDP suite tests pass serially in a private IPC container.
- Changed-line clang-format 21.1.8, Python syntax and whitespace checks pass.

Full logs, commands, configuration and source hashes: `/private/tmp/frameworks-r1-review-20261122/evidence.json`. Remote source/build/logs: `/srv/frameworks-dev/workspaces/stronk/codex-r1-review-dba2-20261122/`. The initial readiness race, missing canonical Livepeer input, browser-default HEVC selection and insufficient fixture window retirement runs are fixture corrections, excluded from product-red evidence.

Configuration: Linux, WITH_AV=false, ONNX/SSL/SRT/RIST disabled; real system FFmpeg performs the fixture encodes and gateway transcodes. These are focused review checks, not full release qualification. No push, tag or rollout.

R3/CUDA work already exists in 0acdc92e1, with the actual decoder resampler regression in 4097475ed; preserved rather than redone. Subsequent handoff work must use this advanced baseline and retain those commits.

Sign-off: the five requested R1 review items are implemented and covered by the focused Linux checks. Full handoff/release sign-off remains incomplete; full gates remain scheduled for the end of the work list.


## Native HEVC open-GOP follow-up (2026-11-23)

The integrated native AV-enabled build exposed a real HEVC loss that the focused Linux fixture did not: 230 video PES packets over 18 seconds, while all 844 audio packets remained continuous. The reader joined in 0.45 seconds. Ingest logged out-of-order drops (80 < 120 ms). HEVC CRA keyframes were flushing the predictor before their leading pictures arrived, and the initial short GOP froze a zero reorder window.

The parser now retains HEVC's SPS-declared reorder window across keyframes, with a bounded 16-picture fallback when setup is unavailable. EOF still drains all buffered packets. A deterministic real toDTSC parser regression covers both actual x265 VPS/SPS/PPS and missing setup. Compiled with the old production parser, it fails at frame 2 (DTS 40 after 120); it passes with the fix. The unchanged real HEVC live TS/HLS test now captures 451 video packets over 18 seconds with continuous PTS/DTS and matching HLS clocks. Its H264 companion and all 11 EBML tests pass. All native build targets and changed-line clang-format21 checks pass.

Evidence: /private/tmp/fw-media-round8-20261120/round9/R1-hevc-diagnostic/, including retained red stream/metadata/timings, red-unit-run.log, unit-final-green.log, fix2-run.log, final-related-ebml.log and final-related-live.log. Linux normal/strict-ASan, exact native package and full stack qualification remain pending; this does not grant release sign-off.


## HTTP Matroska publisher EOF follow-up (2026-11-23)

Extending the live test through EOF exposed buffered source frames left behind at HTTP request completion. A non-resumable viewer first returned469/500, but immediate track removal confounded that result. A retained source separated the ingest counter from the ordinary live reader's deliberately held final packet. The exact strengthened regression, using the actual pre-fix fbe8 HTTP handler rebuilt with unchanged supporting objects and no boundary stubs, fails with source parts_end497 instead of500. Source audio has all939 packets. The first flush-onFinish-only attempt still failed because HTTP completion called publisher acceptance again before draining.

Completed PUT/POST requests now keep their existing source claim while finishing the parser and publishing its buffered packets, then acknowledge and close. An abrupt connection close also finishes the parser through onFinish. The existing packet-publication path is shared by normal callbacks and the final drain. A real shared-metadata probe proves500/500 source frames are ingested; the retained live reader separately verifies its499-frame continuous prefix while it waits for a possible resumed producer. HEVC and H264 canonical tests pass, as do all11 EBML tests and all882 native build steps. No playback-resume semantics were changed to make the test pass.

Evidence: /private/tmp/fw-media-round8-20261120/round9/R1-hevc-retained-tail/. canonical-old-handler-red.log and old-production/source-proof.json retain the real old-handler failure; canonical-source-counter-green.log, final-source-counter-green.log and final-ebml-green.log retain the green results. Earlier incomplete experiments and their failures remain in that directory. Linux/ASan/package/stack qualification remains pending; no release sign-off.


## Startup-failure fixture cleanup (2026-11-23)

A diagnostic setup attempt exited before the fixture recorded the controller's process group. close() called getpgid() on that dead PID and masked the original readiness failure with ProcessLookupError. A focused test reproduces the masking error, checks preservation of the controller log and original readiness exception, and forbids signaling an unrelated group. Suppress only the missing-process lookup during group discovery; all seven fixture IPC tests now pass. No production behavior changed. dead-controller-cleanup-red.log and dead-controller-cleanup-green.log retain both results.

The stronger old-handler reconstruction also passed setup after isolating its executable snapshots and reproduced parts_end497/500. All five production translation units and the handler header came from fbe8, with unchanged supporting headers/library and no boundary stubs. source-proof-v2.json and canonical-old-handler-v3-red.log retain that evidence; the failed setup run remains separately recorded.

## R1 input-clock correction (2026-11-23)

Baseline: Mist 33b3704d2; monorepo 31ab98db2. Preserve the HEVC open-GOP, HTTP publisher EOF and fixture-cleanup commits above. This supersedes the TS-wide presentation-delay approach and its sign-off.

### Actual red results and implementation

- Real RTMP x264/FLV publication -> HTTP TS, matching identical source slices: extra composition delay was 200 ms at 10 fps and 67 ms at 30 fps with bf=3. With bitstream_restriction removed from SPS, extra delay was 1600/534 ms. The assertion is TS PTS - DTS == the matching source PTS - DTS. All four failed before the fix (`rtmp-predictor-red-v2.log`); readiness-only failures in earlier attempts are excluded.
- Removed updatePresentationDelay, its TS fields, all video/audio/meta presentation additions and its now-unused source-liveness accessor. Signed RTP math and R22 PID allocation are preserved.
- Matroska's predictor now calculates a fixed millisecond reorder span from its buffered timestamps and depth. DTS trails the sorted presentation clock by that span, retaining each original PTS and nonnegative composition offsets. At the start of the source, DTS uses a short strictly increasing preroll inside the original source origin; it reaches the fixed lag without moving PTS or audio. This startup exception is deliberate: unsigned DTSC cannot put decode preroll before zero, and starting before a nonzero source origin broke existing MP4 edit alignment (observed A/V shifts -1.000 versus -0.779 seconds, `mp4-origin-before.log`).
- Strengthened the actual adaptive live H264 fixture to begin/end with no B-frames around its deep-B-frame interval. It failed at publisher EOF with 440/500 source frames (`live-zero-depth-red.log`). H264 now seeds its input reorder buffer from SPS, with the bounded 16-picture fallback when restriction is absent; it does not drain the initial GOP before the clock is initialized. The HEVC open-GOP behavior remains intact.
- The moved clock exposed two downstream failures, fixed with their real regressions. Progressive MP4 had every packet but decoded only 78/80 frames; its edit/movie duration now follows presentation while MDHD/STTS retain the decode span. The focused header assertion was MDHD/STTS=200 but TKHD=200 instead of 280 (`mp4-preroll-red.log`). Livepeer bf=0 returned 198/200 frames, with a missing 2000/2040 ms pair and dropped out-of-order packets at a segment boundary (`return-clock-diagnostic.log`). Returned encoders now keep their own decode lead and align their first PTS to the source PTS; unsigned startup preroll keeps the returned decode clock monotonic. Tests demand equal frame counts and every source/returned PTS, for bf=0 and bf=3.
- A nonzero seek initially restarted the decode clock: DTS=11000 instead of indexed 10880 (`seek-clock-red.log`). Predictor flush now retains its origin/span/depth. Matroska indexes store/restore these clocks; the actual cached-header reader regression failed before restoration (`restore-clock-red.log`). Index version advances from 4 to 5, rejecting old signed-offset indexes (`cache-red-v3.log`), so existing Matroska files reindex once before use.

### Qualification

Final qualification (`clock-qualification.log`): all 36 EBML/MPEG-TS/Livepeer/SDP/MP4 suite checks and all 18 targeted checks pass, with no skips. All four RTMP cases have zero added delay. Fixed-file TS retains 150 pixel-identical frames. Live H264/HEVC each ingest 500/500 frames, expose the expected 499-frame resumable-reader prefix, keep video/audio PTS continuous, retire the earlier live window and agree across 592/581 independent HLS requests. Both Livepeer return modes preserve all 200 source frame times across seven segments. Matroska -> MP4 at zero and nonzero source origins, and native MP4 -> MP4, retain every decoded pixel and A/V alignment. Seek/cache regressions, signed SDP and all changed-line clang-format21/Python/whitespace checks pass. SHA-256 confirms all 20 implementation/test files match the remote tested snapshot. Evidence directory: `/private/tmp/frameworks-r1-input-clock-20261123/`; remote snapshot/build/logs: `/srv/frameworks-dev/workspaces/stronk/codex-r1-input-clock-20261123/`. Intermediate logs retain failures and excluded fixture corrections; they are not final qualification.

Configuration remains focused Linux: WITH_AV=false, ONNX/SSL/SRT/RIST disabled; real FFmpeg encodes/transcodes exercise the integrations. Full hardware, sanitizer, package, stack and release gates remain outstanding at the end of the work list. No monorepo edits, push, tag or rollout.

Sign-off: this R1 input-clock follow-up is implemented and passes focused Linux qualification. Full handoff/release sign-off remains incomplete; the deferred gates above still apply.
