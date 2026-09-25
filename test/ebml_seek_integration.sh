#!/usr/bin/env bash
# ebml_seek_integration.sh <ffmpeg> <input_ebml_seek_test>
# Writes a Matroska file whose audio starts at 0 and whose video starts at 2 s
# (the shape a Livepeer rendition that lost its first segment produces) and
# checks that loading the audio from the start reads it from 0.
set -euo pipefail
ffmpeg=$1
probe=$2
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
"$ffmpeg" -hide_banner -loglevel error -y \
  -f lavfi -i "sine=frequency=440:sample_rate=48000:duration=8" \
  -f lavfi -i "testsrc2=size=160x90:rate=15:duration=6" \
  -filter_complex "[1:v]setpts=PTS+2/TB[v]" -map 0:a -map "[v]" \
  -c:a aac -b:a 64k -c:v libx264 -preset ultrafast -g 15 -pix_fmt yuv420p \
  -cluster_time_limit 500 "$work/audio-first.mkv"
exec "$probe" "$work/audio-first.mkv"
