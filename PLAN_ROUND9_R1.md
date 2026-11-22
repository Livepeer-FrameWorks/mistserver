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
