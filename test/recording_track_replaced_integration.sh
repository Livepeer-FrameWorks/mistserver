#!/bin/sh
# A segmented recording of a live stream across a publisher reconnect. The
# buffer keeps (resumes) a track whose settings the new publisher repeats and
# replaces one whose settings changed. A file holds one track set, so:
#   same settings      -> the recording carries on across the reconnect;
#   different settings -> the recording ends cleanly where the replaced track
#                         ended, with a final segment and exit reason
#                         "selected track replaced", and no segment announces
#                         the replacement track.
# MIST_REPUBLISH_RESOLUTION selects the second publisher's video size; the
# first publisher always sends 320x180.
set -eu

if [ "$#" -ne 8 ]; then
  echo "usage: $0 ffmpeg ffprobe MistController MistInBuffer MistOutRTMP MistOutHTTPTS MistUtilNuke timeout" >&2
  exit 2
fi
if [ "${MIST_RUN_MEDIA_TESTS:-}" != "1" ]; then
  echo "set MIST_RUN_MEDIA_TESTS=1 to run the live recording reconnect pipeline" >&2
  exit 77
fi

ffmpeg=$1
ffprobe=$2
controller=$3
input_buffer=$4
output_rtmp=$5
output_ts=$6
util_nuke=$7
timeout_program=$8
for program in "$ffmpeg" "$ffprobe" "$controller" "$input_buffer" "$output_rtmp" "$output_ts" "$util_nuke" \
  "$timeout_program"; do
  if [ ! -x "$program" ]; then
    echo "required executable is unavailable: $program" >&2
    exit 77
  fi
done
if ! "$ffmpeg" -hide_banner -encoders 2>/dev/null | grep -q 'libx264'; then
  echo "ffmpeg lacks the libx264 encoder required for the publisher fixture" >&2
  exit 77
fi

first_size=320x180
second_size=${MIST_REPUBLISH_RESOLUTION:-$first_size}
if [ "$second_size" = "$first_size" ]; then replaced=0; else replaced=1; fi

work=$(mktemp -d "${TMPDIR:-/tmp}/mist-track-replaced.XXXXXX")
ipc_root="$work/ipc"
mkdir -p "$ipc_root" "$work/out/segments"
stream="trackreplaced$$"
controller_pid=
recording_pid=
publisher_pids=

cleanup() {
  status=$?
  trap - EXIT HUP INT TERM
  if [ "$status" -ne 0 ]; then
    echo "recording track replacement integration failed; logs follow:" >&2
    for log in "$work"/*.log "$work/out/rec.m3u8"; do
      if [ -f "$log" ]; then
        echo "Log: $log" >&2
        grep -vE 'Type mismatch|Codec mismatch|bootmsoffset' "$log" | tail -80 >&2
      fi
    done
  fi
  for pid in $publisher_pids $recording_pid; do
    kill -TERM "$pid" >/dev/null 2>&1 || true
    wait "$pid" >/dev/null 2>&1 || true
  done
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

api_port=$((24000 + ($$ % 12000)))
rtmp_port=$((api_port + 1))
config="$work/config.json"
printf '%s\n' \
  "{\"account\":{\"test\":{\"password\":\"098f6bcd4621d373cade4e832627b4f6\"}},\"auto_push\":null,\"bandwidth\":{\"exceptions\":[\"::1\",\"127.0.0.0/8\"]},\"config\":{\"accesslog\":\"LOG\",\"controller\":{\"interface\":\"127.0.0.1\",\"port\":$api_port,\"username\":null},\"debug\":4,\"defaultStream\":null,\"prometheus\":\"\",\"protocols\":[{\"connector\":\"RTMP\",\"interface\":\"127.0.0.1\",\"port\":$rtmp_port}],\"serverid\":null,\"sessionInputMode\":15,\"sessionOutputMode\":15,\"sessionStreamInfoMode\":1,\"sessionUnspecifiedMode\":0,\"sessionViewerMode\":14,\"tknMode\":15,\"triggers\":{},\"trustedproxy\":[]},\"extwriters\":null,\"jwks\":null,\"push_settings\":{\"maxspeed\":0,\"wait\":3},\"streamkeys\":null,\"streams\":{\"$stream\":{\"name\":\"$stream\",\"source\":\"push://\",\"resume\":1,\"inputtimeout\":6}},\"variables\":null}" \
  >"$config"

TMP="$ipc_root" MIST_CONTROL=1 "$controller" -c "$config" -C r -L "$work/controller.log" &
controller_pid=$!

# wait_for <seconds> <description> <command...>: polls until the command succeeds.
wait_for() {
  seconds=$1
  description=$2
  shift 2
  deadline=$(($(date +%s) + seconds))
  until "$@"; do
    if [ "$(date +%s)" -gt "$deadline" ]; then
      echo "timed out waiting for $description" >&2
      exit 1
    fi
    sleep 0.2
  done
}
logged() { grep -qE -- "$2" "$1" 2>/dev/null; }
exited() { ! kill -0 "$1" 2>/dev/null; }
wait_for 30 "the test controller and its RTMP connector" logged "$work/controller.log" 'Started connector'

publish() {
  name=$1
  size=$2
  duration=$3
  "$timeout_program" $((duration + 20)) "$ffmpeg" -hide_banner -loglevel error -re \
    -f lavfi -i "testsrc2=size=$size:rate=25:duration=$duration" \
    -f lavfi -i "sine=frequency=997:sample_rate=48000:duration=$duration" \
    -c:v libx264 -pix_fmt yuv420p -preset ultrafast -g 25 -keyint_min 25 -bf 0 \
    -c:a aac -b:a 96k -f flv "rtmp://127.0.0.1:$rtmp_port/live/$stream" \
    >"$work/$name.log" 2>&1 &
  publisher_pids="$publisher_pids $!"
  last_publisher=$!
}

publish publisher1 "$first_size" 10
publisher1_pid=$last_publisher
wait_for 20 "the first publisher's stream to become active" logged "$work/controller.log" "Stream $stream became active"

TMP="$ipc_root" MIST_CONTROL=1 "$timeout_program" 90 "$output_ts" -s "$stream" \
  "$work/out/segments/\$segmentCounter.ts?m3u8=../rec.m3u8&audio=source&video=source&split=2&append=1&noendlist=1&nounlink=1" \
  >"$work/recording.log" 2>&1 &
recording_pid=$!
wait_for 20 "the recording to start" logged "$work/recording.log" 'Recording will start'

wait "$publisher1_pid" || true
# The gap stays well inside inputtimeout, so the buffer keeps the first
# session's tracks for the second publisher to resume or replace.
sleep 3
publish publisher2 "$second_size" 12
publisher2_pid=$last_publisher

if [ "$replaced" -eq 1 ]; then
  wait_for 20 "the buffer to replace the first session's video track" \
    logged "$work/controller.log" 'Removing track [0-9]+ retained from the previous publisher session'
  # Bounded by the second session plus inputtimeout, so a recording that does
  # not stop on the replacement still ends and its segments get checked.
  wait_for 40 "the recording to end" exited "$recording_pid"
  recording_pid=
else
  wait "$publisher2_pid" || true
  wait_for 30 "the recording to end after the stream went offline" exited "$recording_pid"
  recording_pid=
  if logged "$work/controller.log" 'Removing track [0-9]+ retained from the previous publisher session'; then
    echo "the buffer replaced a track although the second publisher repeated the first one's settings" >&2
    exit 1
  fi
  if logged "$work/recording.log" 'selected track replaced'; then
    echo "the recording ended on a track replacement although every track resumed" >&2
    exit 1
  fi
fi

playlist="$work/out/rec.m3u8"
if [ ! -s "$playlist" ]; then
  echo "the recording wrote no playlist" >&2
  exit 1
fi
segments=$(grep -v '^#' "$playlist" | grep -c '\.ts$' || true)
files=$(find "$work/out/segments" -name '*.ts' | wc -l | tr -d ' ')
if [ "$segments" -lt 2 ] || [ "$segments" -ne "$files" ]; then
  echo "the playlist lists $segments segments but the recording wrote $files" >&2
  exit 1
fi

# Every segment holds exactly one video stream with real dimensions: a second
# (empty, 0x0) video stream means a replacement track was announced mid-file.
seen_second=0
for entry in $(grep -v '^#' "$playlist"); do
  segment="$work/out/$entry"
  # TS streams are listed once per program and once on their own; the index
  # folds the duplicates.
  dims=$("$ffprobe" -v error -select_streams v -show_entries stream=index,width,height -of csv=p=0 "$segment" \
    2>/dev/null | grep . | sort -u | cut -d, -f2-)
  count=$(printf '%s\n' "$dims" | grep -c . || true)
  if [ "$count" -ne 1 ]; then
    echo "segment $entry holds $count video streams ($(printf '%s' "$dims" | tr '\n' ' ')); expected exactly one" >&2
    exit 1
  fi
  case "$dims" in
    320,180) ;;
    "$(printf '%s' "$second_size" | tr x ,)") seen_second=1 ;;
    *)
      echo "segment $entry has a video stream of $dims; expected $first_size or $second_size" >&2
      exit 1
      ;;
  esac
done

total=$(awk -F'[:,]' '/^#EXTINF:/ { sum += $2 } END { printf "%d", sum }' "$playlist")
if [ "$replaced" -eq 1 ]; then
  if [ "$seen_second" -ne 0 ]; then
    echo "the recording kept writing after its video track was replaced ($second_size segments present)" >&2
    exit 1
  fi
  if ! logged "$work/recording.log" 'Logging clean exit reason: selected track replaced'; then
    echo "the recording did not end with exit reason 'selected track replaced'" >&2
    exit 1
  fi
  if ! logged "$work/recording.log" 'Adding final segment'; then
    echo "the recording ended without writing its final segment to the playlist" >&2
    exit 1
  fi
  echo "the recording ended with 'selected track replaced' after $segments segments (${total}s), all $first_size"
else
  # The recording starts a few seconds into the first 10 s session and must
  # also hold most of the second 12 s session.
  if [ "$total" -lt 14 ]; then
    echo "the recording holds ${total}s; expected it to continue through the second session" >&2
    exit 1
  fi
  echo "the recording continued across the reconnect: $segments segments (${total}s) in one playlist"
fi
