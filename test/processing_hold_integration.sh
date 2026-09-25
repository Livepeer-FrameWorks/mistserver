#!/bin/sh
# A process-controlled recording of a long source, fed at the unconstrained
# 32x ceiling (only an inconsequential Thumbs process runs), written through a
# sink slower than the feed that starts draining only 5 s after the recorder
# attached, so the recording stalls right behind its header while the feed ramps.
# The recording must still contain the whole source: the buffer may not evict
# keys the recorder has not written, and the feed has to wait for it.
set -eu

if [ "$#" -ne 10 ]; then
  echo "usage: $0 ffmpeg ffprobe MistController MistInMP4 MistInBuffer MistProcThumbs MistOutEBML MistUtilNuke timeout python3" >&2
  exit 2
fi
if [ "${MIST_RUN_MEDIA_TESTS:-}" != "1" ]; then
  echo "set MIST_RUN_MEDIA_TESTS=1 to run the processing buffer hold pipeline" >&2
  exit 77
fi

ffmpeg=$1
ffprobe=$2
controller=$3
input_mp4=$4
input_buffer=$5
process_thumbs=$6
output_ebml=$7
util_nuke=$8
timeout_program=$9
python=${10}
for program in "$ffmpeg" "$ffprobe" "$controller" "$input_mp4" "$input_buffer" "$process_thumbs" \
  "$output_ebml" "$util_nuke" "$timeout_program" "$python"; do
  if [ ! -x "$program" ]; then
    echo "required executable is unavailable: $program" >&2
    exit 77
  fi
done
if ! "$ffmpeg" -hide_banner -encoders 2>/dev/null | grep -q 'libx264'; then
  echo "ffmpeg lacks the libx264 encoder required for the fixture" >&2
  exit 77
fi

source_duration=${MIST_HOLD_SOURCE_DURATION:-180}
header_delay=${MIST_HOLD_HEADER_DELAY:-5}
# Sink throughput as a multiple of the source bitrate: well below the 32x feed.
sink_speed=${MIST_HOLD_SINK_SPEED:-4}

work=$(mktemp -d "${TMPDIR:-/tmp}/mist-processing-hold.XXXXXX")
ipc_root="$work/ipc"
mkdir -p "$ipc_root"
stream="prochold$$"
controller_pid=
input_pid=


cleanup() {
  status=$?
  trap - EXIT HUP INT TERM
  if [ "$status" -ne 0 ]; then
    echo "processing hold integration failed; logs follow:" >&2
    for log in "$work"/*.log; do
      if [ -f "$log" ]; then
        echo "Log: $log" >&2
        grep -E 'Processing (rate|feed)|FAIL|WARN|evict|Waiting for processing' "$log" | tail -40 >&2 || true
        tail -40 "$log" >&2
      fi
    done
  fi
  if [ -n "$controller_pid" ]; then
    TMP="$ipc_root" MIST_CONTROL=1 "$util_nuke" "$stream" >/dev/null 2>&1 || true
  fi
  if [ -n "$input_pid" ]; then
    kill -TERM "$input_pid" >/dev/null 2>&1 || true
    wait "$input_pid" >/dev/null 2>&1 || true
  fi
  if [ -n "$controller_pid" ]; then
    kill -INT "$controller_pid" >/dev/null 2>&1 || true
    wait "$controller_pid" >/dev/null 2>&1 || true
  fi
  rm -rf -- "/tmp/mist_thumbs/$stream"
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

source_media="$work/source.mp4"
"$ffmpeg" -hide_banner -loglevel error -y \
  -f lavfi -i testsrc2=size=320x180:rate=25:duration="$source_duration" \
  -f lavfi -i sine=frequency=997:sample_rate=48000:duration="$source_duration" \
  -c:v libx264 -pix_fmt yuv420p -preset veryfast -b:v 400k -g 50 -keyint_min 50 -bf 0 -sc_threshold 0 \
  -c:a aac -b:a 96k -movflags +faststart "$source_media"
source_bytes=$(wc -c <"$source_media" | tr -d ' ')
sink_rate=$((source_bytes / source_duration * sink_speed))

port=$((27000 + ($$ % 9000)))
config="$work/config.json"
printf '%s\n' \
  "{\"account\":{\"test\":{\"password\":\"098f6bcd4621d373cade4e832627b4f6\"}},\"auto_push\":null,\"bandwidth\":{\"exceptions\":[\"::1\",\"127.0.0.0/8\"]},\"config\":{\"accesslog\":\"LOG\",\"controller\":{\"interface\":\"127.0.0.1\",\"port\":$port,\"username\":null},\"debug\":4,\"defaultStream\":null,\"prometheus\":\"\",\"protocols\":[],\"serverid\":null,\"sessionInputMode\":15,\"sessionOutputMode\":15,\"sessionStreamInfoMode\":1,\"sessionUnspecifiedMode\":0,\"sessionViewerMode\":14,\"tknMode\":15,\"triggers\":null,\"trustedproxy\":[]},\"extwriters\":null,\"jwks\":null,\"push_settings\":{\"maxspeed\":0,\"wait\":3},\"streamkeys\":null,\"streams\":{\"$stream\":{\"name\":\"$stream\",\"source\":\"$source_media\",\"process_controlled_realtime\":true,\"processes\":[{\"process\":\"Thumbs\",\"track_select\":\"video=H264\",\"inconsequential\":true,\"thumb_width\":80,\"thumb_height\":80,\"grid_cols\":3,\"grid_rows\":2,\"jpeg_quality\":80,\"interval\":2000,\"source_mask\":4,\"target_mask\":3,\"restart_type\":\"fixed\"}]}},\"variables\":null}" \
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

TMP="$ipc_root" MIST_CONTROL=1 "$input_mp4" -r -s "$stream" "$source_media" >"$work/input.log" 2>&1 &
input_pid=$!
started=0
attempt=0
while [ "$attempt" -lt 300 ]; do
  if grep -q 'Input started' "$work/input.log" 2>/dev/null; then started=1; break; fi
  if ! kill -0 "$input_pid" 2>/dev/null; then break; fi
  attempt=$((attempt + 1))
  sleep 0.05
done
if [ "$started" -ne 1 ]; then
  echo "canonical input did not become ready" >&2
  exit 1
fi

# The recording goes to stdout. The sink reads nothing for header_delay
# seconds, then drains at sink_speed x the source bitrate, like an upload that
# is slow to start and cannot keep up with the 32x feed.
recording="$work/recording.mkv"
throttle="$work/throttle.py"
cat >"$throttle" <<'EOF'
import sys, time
dst, rate, delay = sys.argv[1], int(sys.argv[2]), float(sys.argv[3])
time.sleep(delay)
with open(dst, 'wb') as o:
    start = time.monotonic()
    n = 0
    while True:
        b = sys.stdin.buffer.read1(16384)
        if not b:
            break
        o.write(b)
        n += len(b)
        ahead = n / rate - (time.monotonic() - start)
        if ahead > 0:
            time.sleep(ahead)
EOF
"$timeout_program" 150 env TMP="$ipc_root" MIST_CONTROL=1 "$output_ebml" -s "$stream" - 2>"$work/output.log" | \
  "$python" "$throttle" "$recording" "$sink_rate" "$header_delay" || true

if [ ! -s "$recording" ]; then
  echo "recording is empty" >&2
  exit 1
fi
source_packets=$("$ffprobe" -v error -select_streams v:0 -count_packets \
  -show_entries stream=nb_read_packets -of default=nw=1:nk=1 "$source_media")
recorded=$("$ffprobe" -v error -select_streams v:0 -show_entries packet=pts_time -of csv=p=0 "$recording" | \
  awk 'NR == 1 { first = $1 } { last = $1; n++ } END { printf "%d %.3f %.3f", n, first, last }')
set -- $recorded
recorded_packets=$1
first_time=$2
last_time=$3
if [ "$recorded_packets" -ne "$source_packets" ]; then
  echo "recording holds $recorded_packets video packets ($first_time..$last_time s); source has $source_packets" >&2
  exit 1
fi
echo "processing recording kept all $recorded_packets video packets ($first_time..$last_time s) through a ${sink_speed}x sink"
