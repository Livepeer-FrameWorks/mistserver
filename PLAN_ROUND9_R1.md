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
