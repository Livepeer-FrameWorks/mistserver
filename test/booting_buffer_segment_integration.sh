#!/bin/sh
# A segmented TS recording started before its stream's buffer exists (the
# recording itself boots the stream) must wait for the booting buffer, start
# at the beginning of the stream, and deliver its first segment within one
# split interval plus boot slack.
set -eu

if [ "$#" -ne 8 ]; then
  echo "usage: $0 ffmpeg ffprobe MistController MistInEBML MistInBuffer MistOutHTTPTS MistUtilNuke timeout" >&2
  exit 2
fi
if [ "${MIST_RUN_MEDIA_TESTS:-}" != "1" ]; then
  echo "set MIST_RUN_MEDIA_TESTS=1 to run the booting buffer segment pipeline" >&2
  exit 77
fi

ffmpeg=$1
ffprobe=$2
controller=$3
input_ebml=$4
input_buffer=$5
output_ts=$6
util_nuke=$7
timeout_program=$8
for program in "$ffmpeg" "$ffprobe" "$controller" "$input_ebml" "$input_buffer" "$output_ts" "$util_nuke" \
  "$timeout_program"; do
  if [ ! -x "$program" ]; then
    echo "required executable is unavailable: $program" >&2
    exit 77
  fi
done
if ! "$ffmpeg" -hide_banner -encoders 2>/dev/null | grep -q 'libx264'; then
  echo "ffmpeg lacks the libx264 encoder required for the fixture" >&2
  exit 77
fi

split=${MIST_SEGMENT_SPLIT:-4}
work=$(mktemp -d "${TMPDIR:-/tmp}/mist-booting-segment.XXXXXX")
ipc_root="$work/ipc"
mkdir -p "$ipc_root" "$work/segments"
stream="bootseg$$"
controller_pid=
output_pid=

cleanup() {
  status=$?
  trap - EXIT HUP INT TERM
  if [ "$status" -ne 0 ]; then
    echo "booting buffer segment integration failed; logs follow:" >&2
    for log in "$work"/*.log; do
      if [ -f "$log" ]; then
        echo "Log: $log" >&2
        tail -60 "$log" >&2
      fi
    done
  fi
  if [ -n "$output_pid" ]; then
    kill -TERM "$output_pid" >/dev/null 2>&1 || true
    wait "$output_pid" >/dev/null 2>&1 || true
  fi
  if [ -n "$controller_pid" ]; then
    TMP="$ipc_root" MIST_CONTROL=1 "$util_nuke" "$stream" >/dev/null 2>&1 || true
    kill -INT "$controller_pid" >/dev/null 2>&1 || true
    wait "$controller_pid" >/dev/null 2>&1 || true
  fi
  if [ "${MIST_KEEP_TEST_ARTIFACTS:-}" = "1" ]; then
    echo "preserved test artifacts in $work" >&2
  else
    rm -rf -- "$work"
  fi
  exit "$status"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

source_mkv="$work/source.mkv"
"$ffmpeg" -hide_banner -loglevel error -y \
  -f lavfi -i testsrc2=size=320x180:rate=25:duration=30 \
  -f lavfi -i sine=frequency=997:sample_rate=48000:duration=30 \
  -c:v libx264 -pix_fmt yuv420p -preset veryfast -g 25 -keyint_min 25 -bf 0 -sc_threshold 0 \
  -c:a aac -b:a 96k "$source_mkv"

port=$((21000 + ($$ % 15000)))
config="$work/config.json"
printf '%s\n' \
  "{\"account\":{\"test\":{\"password\":\"098f6bcd4621d373cade4e832627b4f6\"}},\"auto_push\":null,\"bandwidth\":{\"exceptions\":[\"::1\",\"127.0.0.0/8\"]},\"config\":{\"accesslog\":\"LOG\",\"controller\":{\"interface\":\"127.0.0.1\",\"port\":$port,\"username\":null},\"debug\":4,\"defaultStream\":null,\"prometheus\":\"\",\"protocols\":[],\"serverid\":null,\"sessionInputMode\":15,\"sessionOutputMode\":15,\"sessionStreamInfoMode\":1,\"sessionUnspecifiedMode\":0,\"sessionViewerMode\":14,\"tknMode\":15,\"triggers\":{},\"trustedproxy\":[]},\"extwriters\":null,\"jwks\":null,\"push_settings\":{\"maxspeed\":0,\"wait\":3},\"streamkeys\":null,\"streams\":{\"$stream\":{\"name\":\"$stream\",\"source\":\"mkv-exec:$ffmpeg -hide_banner -loglevel error -re -i $source_mkv -map 0:v:0 -map 0:a:0 -c copy -f matroska -\"}},\"variables\":null}" \
  >"$config"

TMP="$ipc_root" MIST_CONTROL=1 "$controller" -c "$config" -C r -L "$work/controller.log" &
controller_pid=$!
ready=0
attempt=0
while [ "$attempt" -lt 100 ]; do
  if grep -q 'Controller started' "$work/controller.log" 2>/dev/null; then ready=1; break; fi
  if ! kill -0 "$controller_pid" 2>/dev/null; then break; fi
  attempt=$((attempt + 1))
  sleep 0.05
done
if [ "$ready" -ne 1 ]; then
  echo "test controller did not become ready" >&2
  exit 1
fi

# No buffer exists yet: this recording boots the stream.
started=$(date +%s)
TMP="$ipc_root" MIST_CONTROL=1 "$timeout_program" 60 "$output_ts" -s "$stream" \
  "$work/segments/seg_\$segmentCounter.ts?split=$split" >"$work/output.log" 2>&1 &
output_pid=$!

# The first segment is complete once the second one is opened.
deadline=$((started + split + 2))
first=
while [ "$(date +%s)" -le "$((deadline + 20))" ]; do
  count=$(find "$work/segments" -name 'seg_*.ts' | wc -l | tr -d ' ')
  if [ "$count" -ge 2 ]; then
    first=$(find "$work/segments" -name 'seg_*.ts' | sort -t_ -k2 -n | head -n 1)
    break
  fi
  if ! kill -0 "$output_pid" 2>/dev/null; then break; fi
  sleep 0.1
done
arrived=$(date +%s)
if [ -z "$first" ]; then
  echo "recording started before the buffer never completed a first segment" >&2
  exit 1
fi
if [ "$arrived" -gt "$deadline" ]; then
  echo "first segment completed $((arrived - started)) s after the recording started; expected within split+2 = $((split + 2)) s" >&2
  exit 1
fi
# Live timestamps carry the buffer's clock base, so "starts at the beginning"
# is checked by content: segment 0 exists and holds a whole split interval of
# the 25 fps source, not the tail of an interval the output joined late.
case "$(basename "$first")" in
  seg_0.ts) ;;
  *)
    echo "first completed segment is $(basename "$first"); expected seg_0.ts" >&2
    exit 1
    ;;
esac
first_packets=$("$ffprobe" -v error -select_streams v:0 -count_packets -show_entries stream=nb_read_packets \
  -of default=nw=1:nk=1 "$first" | head -n 1)
if [ "$first_packets" -lt $((split * 25 - 1)) ]; then
  echo "first segment holds $first_packets video packets; expected a whole ${split}s interval ($((split * 25)))" >&2
  exit 1
fi
echo "recording started before its buffer delivered seg_0.ts ($first_packets video packets) after $((arrived - started)) s"
