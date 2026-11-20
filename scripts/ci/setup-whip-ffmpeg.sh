#!/usr/bin/env bash
# The distro CLI can omit WHIP; keep this publisher separate from Mist's FFmpeg libraries.
set -euo pipefail
version=9.1
source_sha256=1a4d394ae63d190a526cd052eb4415c156b5c5e2df37efab41a7de0b8fde2433
prefix=
jobs=${MIST_GATE_JOBS:-2}
while (($#)); do
  case "$1" in
    --prefix) prefix=${2:?missing prefix}; shift 2 ;;
    --jobs) jobs=${2:?missing jobs}; shift 2 ;;
    *) printf 'Unknown argument: %s\n' "$1" >&2; exit 2 ;;
  esac
done
[[ "$prefix" = /* && "$jobs" =~ ^[1-9][0-9]*$ ]] || { echo 'An absolute --prefix and positive job count are required' >&2; exit 2; }
verify_publisher() {
  local muxers encoders
  muxers=$("$prefix/bin/ffmpeg" -hide_banner -muxers 2>/dev/null)
  encoders=$("$prefix/bin/ffmpeg" -hide_banner -encoders 2>/dev/null)
  grep -qw whip <<<"$muxers" && grep -qw libx264 <<<"$encoders" && grep -qw libopus <<<"$encoders"
}
if [[ -x "$prefix/bin/ffmpeg" && -f "$prefix/source.sha256" ]] &&
   [[ "$(cat "$prefix/source.sha256")" = "$source_sha256" ]] && verify_publisher; then
  printf 'Verified cached FFmpeg %s WHIP publisher\n' "$version"
  exit 0
fi
mkdir -p "$prefix/downloads" "$prefix/source" "$prefix/bin"
archive="$prefix/downloads/ffmpeg-$version.tar.xz"
if [[ ! -f "$archive" ]]; then
  curl --fail --location --retry 3 "https://ffmpeg.org/releases/ffmpeg-$version.tar.xz" --output "$archive"
fi
printf '%s  %s\n' "$source_sha256" "$archive" | sha256sum --check --status
tar -xJf "$archive" -C "$prefix/source" --strip-components=1
cd "$prefix/source"
./configure --prefix="$prefix" --disable-everything --disable-autodetect \
  --disable-doc --disable-debug --disable-ffplay --disable-ffprobe \
  --enable-ffmpeg --enable-gpl --enable-version3 --enable-libx264 --enable-libopus --enable-openssl \
  --enable-indev=lavfi --enable-filter=testsrc2,sine,format,aformat,aresample,scale,null,anull \
  --enable-decoder=wrapped_avframe,pcm_s16le --enable-encoder=libx264,libopus \
  --enable-muxer=whip --enable-protocol=file,pipe,http,https,tcp,tls,udp,dtls
make -j"$jobs" ffmpeg
install -m 755 ffmpeg "$prefix/bin/ffmpeg"
verify_publisher
printf '%s\n' "$source_sha256" > "$prefix/source.sha256"
printf 'Built pinned FFmpeg %s WHIP publisher\n' "$version"
