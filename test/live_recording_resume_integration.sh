#!/bin/sh
# A scheduled recording (duration) of a live stream in resume mode, across a publisher that
# disconnects and comes back with the same settings five seconds later. The buffer keeps the
# publisher's tracks for resume, so the recording waits for them and ends at its stop position
# with media from after the gap, instead of ending the tracks when the publisher left.
set -eu

if [ "$#" -ne 8 ]; then
  echo "usage: $0 ffmpeg ffprobe MistController MistInBuffer MistOutRTMP MistOutEBML MistUtilNuke timeout" >&2
  exit 2
fi
if [ "${MIST_RUN_MEDIA_TESTS:-}" != "1" ]; then
  echo "set MIST_RUN_MEDIA_TESTS=1 to run the live recording resume pipeline" >&2
  exit 77
fi

ffmpeg=$1
ffprobe=$2
controller=$3
input_buffer=$4
output_rtmp=$5
output_ebml=$6
util_nuke=$7
timeout_program=$8
for program in "$ffmpeg" "$ffprobe" "$controller" "$input_buffer" "$output_rtmp" "$output_ebml" "$util_nuke" \
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

work=$(mktemp -d "${TMPDIR:-/tmp}/mist-recording-resume.XXXXXX")
ipc_root="$work/ipc"
mkdir -p "$ipc_root"
stream="recresume$$"
controller_pid=
publisher_pid=
recording_pid=

cleanup() {
  status=$?
  trap - EXIT HUP INT TERM
  if [ "$status" -ne 0 ]; then
    echo "live recording resume integration failed; logs follow:" >&2
    for log in "$work"/*.log; do
      if [ -f "$log" ]; then
        echo "Log: $log" >&2
        grep -vE 'Type mismatch|Codec mismatch|bootmsoffset' "$log" | tail -60 >&2
      fi
    done
  fi
  for pid in $publisher_pid $recording_pid; do
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

api_port=$((22000 + ($$ % 9000)))
rtmp_port=$((api_port + 1))
config="$work/config.json"
printf '%s\n' \
  "{\"account\":{\"test\":{\"password\":\"098f6bcd4621d373cade4e832627b4f6\"}},\"auto_push\":null,\"bandwidth\":{\"exceptions\":[\"::1\",\"127.0.0.0/8\"]},\"config\":{\"accesslog\":\"LOG\",\"controller\":{\"interface\":\"127.0.0.1\",\"port\":$api_port,\"username\":null},\"debug\":4,\"defaultStream\":null,\"prometheus\":\"\",\"protocols\":[{\"connector\":\"RTMP\",\"interface\":\"127.0.0.1\",\"port\":$rtmp_port}],\"serverid\":null,\"sessionInputMode\":15,\"sessionOutputMode\":15,\"sessionStreamInfoMode\":1,\"sessionUnspecifiedMode\":0,\"sessionViewerMode\":14,\"tknMode\":15,\"triggers\":{},\"trustedproxy\":[]},\"extwriters\":null,\"jwks\":null,\"push_settings\":{\"maxspeed\":0,\"wait\":3},\"streamkeys\":null,\"streams\":{\"$stream\":{\"name\":\"$stream\",\"source\":\"push://\",\"resume\":1}},\"variables\":null}" \
  >"$config"

TMP="$ipc_root" MIST_CONTROL=1 "$controller" -c "$config" -C r -L "$work/controller.log" &
controller_pid=$!
wait_for 30 "the test controller and its RTMP connector" logged "$work/controller.log" 'Started connector'

# publish <log>: one publisher session; every session uses the same settings, so the buffer
# resumes the tracks a previous session left.
publish() {
  "$ffmpeg" -hide_banner -loglevel error -re \
    -f lavfi -i "testsrc2=size=320x180:rate=25:duration=120" \
    -f lavfi -i "sine=frequency=997:sample_rate=48000:duration=120" \
    -c:v libx264 -pix_fmt yuv420p -preset ultrafast -g 25 -keyint_min 25 -bf 0 \
    -c:a aac -b:a 96k -f flv "rtmp://127.0.0.1:$rtmp_port/live/$stream" >"$work/$1" 2>&1 &
  publisher_pid=$!
}
publish publisher1.log
wait_for 20 "the stream to become active" logged "$work/controller.log" "Stream $stream became active"
sleep 3

recording="$work/recording.mkv"
"$timeout_program" 90 env TMP="$ipc_root" MIST_CONTROL=1 "$output_ebml" -s "$stream" \
  "$recording?duration=24&video=all&audio=all" >"$work/recording.log" 2>&1 &
recording_pid=$!
sleep 6
kill -TERM "$publisher_pid"
wait "$publisher_pid" >/dev/null 2>&1 || true
publisher_pid=
sleep 5
publish publisher2.log
wait_for 60 "the recording to end" exited "$recording_pid"
recording_pid=

if grep -q 'no more data in range' "$work/recording.log"; then
  echo "the recording ended a track while its publisher could still resume it" >&2
  exit 1
fi
if ! grep -q 'planned stopping point reached' "$work/recording.log"; then
  echo "the recording did not end at its planned stop" >&2
  exit 1
fi
video_end=$("$ffprobe" -v error -select_streams v:0 -show_entries packet=pts_time -of csv=p=0 "$recording" | tail -n 1)
audio_end=$("$ffprobe" -v error -select_streams a:0 -show_entries packet=pts_time -of csv=p=0 "$recording" | tail -n 1)
if awk -v v="${video_end:-0}" -v a="${audio_end:-0}" 'BEGIN { exit !(v < 20 || a < 20) }'; then
  echo "the recording ends at ${video_end:-0}s (video) and ${audio_end:-0}s (audio); expected media up to its 24 s stop" >&2
  exit 1
fi
echo "the recording waited for the resumed publisher: video to ${video_end}s, audio to ${audio_end}s"
