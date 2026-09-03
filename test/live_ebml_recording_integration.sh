#!/bin/sh
set -eu

if [ "$#" -ne 9 ]; then
  echo "usage: $0 ffmpeg ffprobe MistController MistInEBML MistInBuffer MistOutEBML MistSession MistUtilLog MistUtilNuke" >&2
  exit 2
fi

if [ "${MIST_RUN_MEDIA_TESTS:-}" != "1" ]; then
  echo "set MIST_RUN_MEDIA_TESTS=1 to run the isolated live EBML recording pipeline" >&2
  exit 77
fi

ffmpeg=$1
ffprobe=$2
controller=$3
input_ebml=$4
input_buffer=$5
output_ebml=$6
session=$7
util_log=$8
util_nuke=$9

for program in "$ffmpeg" "$ffprobe" "$controller" "$input_ebml" "$input_buffer" \
  "$output_ebml" "$session" "$util_log" "$util_nuke"; do
  if [ ! -x "$program" ]; then
    echo "required executable is unavailable: $program" >&2
    exit 77
  fi
done

if ! "$ffmpeg" -hide_banner -encoders 2>/dev/null | grep -q 'libx264'; then
  echo "ffmpeg lacks the libx264 encoder required for the live EBML fixture" >&2
  exit 77
fi

work=$(mktemp -d "${TMPDIR:-/tmp}/mist-live-ebml.XXXXXX")
ipc_root="$work/ipc"
mkdir -p "$ipc_root"
stream="liveebml$$"
controller_pid=

cleanup() {
  status=$?
  trap - EXIT HUP INT TERM
  if [ "$status" -ne 0 ]; then
    echo "live EBML recording integration failed; logs follow:" >&2
    for log in "$work"/*.log; do
      if [ -f "$log" ]; then
        echo "Log: $log" >&2
        tail -100 "$log" >&2
      fi
    done
  fi
  if [ -n "$controller_pid" ]; then
    TMP="$ipc_root" MIST_CONTROL=1 "$util_nuke" "$stream" >/dev/null 2>&1 || true
    kill -INT "$controller_pid" >/dev/null 2>&1 || true
    wait "$controller_pid" >/dev/null 2>&1 || true
  fi
  rm -rf -- "$work"
  exit "$status"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

source_mkv="$work/source.mkv"
"$ffmpeg" -hide_banner -loglevel error -y \
  -f lavfi -i testsrc2=size=320x180:rate=25:duration=8 \
  -f lavfi -i sine=frequency=997:sample_rate=48000:duration=8 \
  -c:v libx264 -pix_fmt yuv420p -preset veryfast -g 50 -keyint_min 50 -bf 2 -sc_threshold 0 \
  -c:a aac -b:a 96k "$source_mkv"

port=$((22000 + ($$ % 15000)))
config="$work/config.json"
printf '%s\n' \
  "{\"account\":{\"test\":{\"password\":\"098f6bcd4621d373cade4e832627b4f6\"}},\"auto_push\":null,\"bandwidth\":{\"exceptions\":[\"::1\",\"127.0.0.0/8\"]},\"config\":{\"accesslog\":\"LOG\",\"controller\":{\"interface\":\"127.0.0.1\",\"port\":$port,\"username\":null},\"debug\":4,\"defaultStream\":null,\"prometheus\":\"\",\"protocols\":[],\"serverid\":null,\"sessionInputMode\":15,\"sessionOutputMode\":15,\"sessionStreamInfoMode\":1,\"sessionUnspecifiedMode\":0,\"sessionViewerMode\":14,\"tknMode\":15,\"triggers\":{},\"trustedproxy\":[]},\"extwriters\":null,\"jwks\":null,\"push_settings\":{\"maxspeed\":0,\"wait\":3},\"streamkeys\":null,\"streams\":{\"$stream\":{\"name\":\"$stream\",\"source\":\"mkv-exec:$ffmpeg -hide_banner -loglevel error -re -stream_loop -1 -i $source_mkv -map 0:v:0 -map 0:a:0 -c copy -f matroska -\"}},\"variables\":null}" \
  >"$config"

TMP="$ipc_root" MIST_CONTROL=1 "$controller" -c "$config" -C r -L "$work/controller.log" &
controller_pid=$!

ready=0
attempt=0
while [ "$attempt" -lt 30 ]; do
  if grep -q 'Controller started' "$work/controller.log" 2>/dev/null; then
    ready=1
    break
  fi
  if ! kill -0 "$controller_pid" 2>/dev/null; then
    break
  fi
  attempt=$((attempt + 1))
  sleep 1
done
if [ "$ready" -ne 1 ]; then
  echo "test controller did not become ready" >&2
  exit 1
fi

recording="$work/recording.mkv"
TMP="$ipc_root" MIST_CONTROL=1 "$output_ebml" -s "$stream" \
  "$recording?duration=3" >"$work/output.log" 2>&1

"$ffmpeg" -hide_banner -loglevel error -i "$recording" -f null - 2>"$work/decode.log"
if [ -s "$work/decode.log" ]; then
  echo "recorded Matroska emitted decoder/demuxer diagnostics" >&2
  exit 1
fi

video_packets=$("$ffprobe" -v error -select_streams v:0 -count_packets \
  -show_entries stream=nb_read_packets -of csv=p=0 "$recording")
audio_packets=$("$ffprobe" -v error -select_streams a:0 -count_packets \
  -show_entries stream=nb_read_packets -of csv=p=0 "$recording")
if [ "$video_packets" -ne 76 ] || [ "$audio_packets" -ne 141 ]; then
  echo "finalized live recording has $video_packets video/$audio_packets audio packets; expected 76/141" >&2
  exit 1
fi

echo "live EBML recording finalized all 76 video and 141 audio packets without diagnostics"
