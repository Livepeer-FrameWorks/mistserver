#!/bin/sh
set -eu

if [ "$#" -ne 9 ]; then
  echo "usage: $0 ffmpeg ffprobe MistController MistInMP4 MistOutMP4 MistSession MistUtilLog MistUtilNuke timeout" >&2
  exit 2
fi

if [ "${MIST_RUN_MEDIA_TESTS:-}" != "1" ]; then
  echo "set MIST_RUN_MEDIA_TESTS=1 to run the isolated stale-DTSH pipeline" >&2
  exit 77
fi

ffmpeg=$1
ffprobe=$2
controller=$3
input_mp4=$4
output_mp4=$5
session=$6
util_log=$7
util_nuke=$8
timeout_program=$9

for program in "$ffmpeg" "$ffprobe" "$controller" "$input_mp4" "$output_mp4" "$session" "$util_log" "$util_nuke" "$timeout_program"; do
  if [ ! -x "$program" ]; then
    echo "required executable is unavailable: $program" >&2
    exit 77
  fi
done

if ! "$ffmpeg" -hide_banner -encoders 2>/dev/null | grep -q 'libx264'; then
  echo "ffmpeg lacks the libx264 encoder required for the stale-DTSH fixture" >&2
  exit 77
fi

work=$(mktemp -d "${TMPDIR:-/tmp}/mist-stale-dtsh.XXXXXX")
ipc_root="$work/ipc"
mkdir -p "$ipc_root"
stream="staledtsh$$"
controller_pid=

stop_controller() {
  if [ -n "$controller_pid" ]; then
    TMP="$ipc_root" MIST_CONTROL=1 "$util_nuke" "$stream" >/dev/null 2>&1 || true
    kill -INT "$controller_pid" >/dev/null 2>&1 || true
    wait "$controller_pid" >/dev/null 2>&1 || true
    controller_pid=
  fi
}

cleanup() {
  status=$?
  stop_controller
  if [ "$status" -ne 0 ]; then
    echo "stale-DTSH integration failed; logs and artifacts preserved in $work" >&2
  else
    rm -rf -- "$work"
  fi
  exit "$status"
}
trap cleanup EXIT HUP INT TERM

source_mp4="$work/source.mp4"
replacement_mp4="$work/replacement.mp4"
"$ffmpeg" -hide_banner -loglevel error -y \
  -f lavfi -i testsrc2=size=320x180:rate=25:duration=8 \
  -c:v libx264 -pix_fmt yuv420p -preset veryfast -g 50 -keyint_min 50 -bf 2 -sc_threshold 0 \
  -movflags +faststart "$source_mp4"
"$ffmpeg" -hide_banner -loglevel error -y \
  -f lavfi -i smptebars=size=176x144:rate=25:duration=1 \
  -c:v libx264 -pix_fmt yuv420p -preset veryfast -g 10 -keyint_min 10 -bf 2 -sc_threshold 0 \
  -movflags +faststart "$replacement_mp4"
# The generic cache freshness check requires a sidecar to be at least fifteen
# seconds newer than its source before it will be reused.
touch -t 202001010000 "$source_mp4"

port=$((26000 + ($$ % 12000)))
config="$work/config.json"
printf '%s\n' \
  "{\"account\":{\"test\":{\"password\":\"098f6bcd4621d373cade4e832627b4f6\"}},\"auto_push\":null,\"bandwidth\":{\"exceptions\":[\"::1\",\"127.0.0.0/8\"]},\"config\":{\"accesslog\":\"LOG\",\"controller\":{\"interface\":\"127.0.0.1\",\"port\":$port,\"username\":null},\"debug\":4,\"defaultStream\":null,\"prometheus\":\"\",\"protocols\":[],\"serverid\":null,\"sessionInputMode\":15,\"sessionOutputMode\":15,\"sessionStreamInfoMode\":1,\"sessionUnspecifiedMode\":0,\"sessionViewerMode\":14,\"tknMode\":15,\"triggers\":{},\"trustedproxy\":[]},\"extwriters\":null,\"jwks\":null,\"push_settings\":{\"maxspeed\":0,\"wait\":3},\"streamkeys\":null,\"streams\":{\"$stream\":{\"name\":\"$stream\",\"source\":\"$source_mp4\"}},\"variables\":null}" \
  >"$config"

start_controller() {
  run=$1
  TMP="$ipc_root" MIST_CONTROL=1 "$controller" -c "$config" -C r -L "$work/controller-$run.log" \
    >"$work/process-$run.log" 2>&1 &
  controller_pid=$!
  attempt=0
  while [ "$attempt" -lt 30 ]; do
    if grep -q 'Controller started' "$work/controller-$run.log" 2>/dev/null; then
      return 0
    fi
    if ! kill -0 "$controller_pid" 2>/dev/null; then
      return 1
    fi
    attempt=$((attempt + 1))
    sleep 1
  done
  return 1
}

if ! start_controller cache; then
  echo "cache-generation controller did not become ready" >&2
  exit 1
fi
warm_clip="$work/warm.mp4"
TMP="$ipc_root" MIST_CONTROL=1 "$output_mp4" -s "$stream" "$warm_clip?duration=1" \
  >"$work/output-cache.log" 2>&1
sidecar="$source_mp4.dtsh"
if [ ! -s "$sidecar" ]; then
  echo "initial MP4 input did not generate a DTSH sidecar" >&2
  exit 1
fi
stale_checksum=$(cksum "$sidecar")
stop_controller

# Preserve the file attributes recorded in the cached header so startup cannot
# reject the sidecar through its normal size/mtime freshness check. Pad the
# shorter replacement with a valid MP4 `free` box; raw trailing zeroes would be
# parsed as a zero-length box and turn this fixture into an unrelated parser
# stall.
source_size=$(wc -c <"$source_mp4" | tr -d ' ')
replacement_size=$(wc -c <"$replacement_mp4" | tr -d ' ')
padding_size=$((source_size - replacement_size))
if [ "$padding_size" -lt 8 ] || [ "$padding_size" -gt 4294967295 ]; then
  echo "fixture cannot represent $padding_size bytes of MP4 free-box padding" >&2
  exit 1
fi
append_byte() {
  octal=$(printf '%03o' "$1")
  printf '%b' "\\$octal"
}
{
  append_byte $(((padding_size >> 24) & 255))
  append_byte $(((padding_size >> 16) & 255))
  append_byte $(((padding_size >> 8) & 255))
  append_byte $((padding_size & 255))
  printf 'free'
  dd if=/dev/zero bs=1 count=$((padding_size - 8)) 2>/dev/null
} >>"$replacement_mp4"
touch -r "$source_mp4" "$replacement_mp4"
mv "$replacement_mp4" "$source_mp4"
if ! start_controller recovery; then
  echo "recovery controller did not become ready" >&2
  exit 1
fi

stale_clip="$work/stale-attempt.mp4"
"$timeout_program" 12 env TMP="$ipc_root" MIST_CONTROL=1 "$output_mp4" -s "$stream" \
  "$stale_clip?duration=1" >"$work/output-stale.log" 2>&1 || true
if ! grep -q 'cached header disagrees with file content' "$work/output-stale.log"; then
  echo "stale DTSH recovery was not diagnosed" >&2
  exit 1
fi
if [ -s "$stale_clip" ] && "$ffmpeg" -hide_banner -loglevel error -i "$stale_clip" -f null - >/dev/null 2>&1; then
  echo "stale DTSH attempt unexpectedly produced a valid recording" >&2
  exit 1
fi

recovered_clip="$work/recovered.mp4"
recovered=0
attempt=0
while [ "$attempt" -lt 5 ]; do
  if "$timeout_program" 8 env TMP="$ipc_root" MIST_CONTROL=1 "$output_mp4" -s "$stream" \
    "$recovered_clip?duration=1" >>"$work/output-recovery.log" 2>&1; then
    if "$ffmpeg" -hide_banner -loglevel error -i "$recovered_clip" -f null - >/dev/null 2>&1; then
      recovered=1
      break
    fi
  fi
  attempt=$((attempt + 1))
  sleep 1
done
if [ "$recovered" -ne 1 ]; then
  echo "stream did not recover after its stale sidecar was removed" >&2
  exit 1
fi
"$ffmpeg" -hide_banner -loglevel error -i "$recovered_clip" -f null -
dimensions=$("$ffprobe" -v error -select_streams v:0 -show_entries stream=width,height -of csv=p=0:s=x "$recovered_clip")
if [ "$dimensions" != "176x144" ]; then
  echo "recovered recording kept stale dimensions $dimensions instead of 176x144" >&2
  exit 1
fi
if [ ! -s "$sidecar" ]; then
  echo "stale sidecar was removed but not regenerated" >&2
  exit 1
fi
fresh_checksum=$(cksum "$sidecar")
if [ "$fresh_checksum" = "$stale_checksum" ]; then
  echo "stale DTSH sidecar was not replaced" >&2
  exit 1
fi
echo "stale DTSH request was rejected and the next request regenerated a decodable 176x144 recording"
