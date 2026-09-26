#!/bin/sh
set -eu

# A live stream whose first publisher is replaced: the buffer removes that
# publisher's retained tracks, track 0 included, and restarts the audio AV
# processor against the next publisher's tracks. The processor must transcode
# them without dying on the missing track 0, and must not crash-loop.

if [ "$#" -ne 8 ]; then
  echo "usage: $0 ffmpeg MistController MistInBuffer MistOutRTMP MistProcAV MistSession MistUtilNuke timeout" >&2
  exit 2
fi

if [ "${MIST_RUN_MEDIA_TESTS:-}" != "1" ]; then
  echo "set MIST_RUN_MEDIA_TESTS=1 to run the live republish audio processing pipeline" >&2
  exit 77
fi

ffmpeg=$1
controller=$2
input_buffer=$3
output_rtmp=$4
process_av=$5
session=$6
util_nuke=$7
timeout_program=$8

for program in "$ffmpeg" "$controller" "$input_buffer" "$output_rtmp" "$process_av" "$session" \
  "$util_nuke" "$timeout_program"; do
  if [ ! -x "$program" ]; then
    echo "required executable is unavailable: $program" >&2
    exit 77
  fi
done
if ! "$ffmpeg" -hide_banner -encoders 2>/dev/null | grep -q 'libx264'; then
  echo "ffmpeg lacks the libx264 encoder required for the publisher fixture" >&2
  exit 77
fi

work=$(mktemp -d "${TMPDIR:-/tmp}/mist-av-republish.XXXXXX")
ipc_root="$work/ipc"
mkdir -p "$ipc_root"
stream="avrepublish$$"
exit_log="$work/process_exit.txt"
handler="$work/process_exit.sh"
printf '%s\n' '#!/bin/sh' "{ sed ''; printf '\\n---\\n'; } >>'$exit_log'" >"$handler"
chmod +x "$handler"
controller_pid=
publisher_pids=

cleanup() {
  status=$?
  trap - EXIT HUP INT TERM
  if [ "$status" -ne 0 ]; then
    echo "live republish audio processing integration failed; logs follow:" >&2
    for log in "$work"/*.log "$exit_log"; do
      if [ -f "$log" ]; then
        echo "Log: $log" >&2
        grep -vE 'Type mismatch|Codec mismatch|bootmsoffset' "$log" | tail -150 >&2
      fi
    done
  fi
  for pid in $publisher_pids; do
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
# No bitrate on the process: the tier configs leave it unset for audio.
printf '%s\n' \
  "{\"account\":{\"test\":{\"password\":\"098f6bcd4621d373cade4e832627b4f6\"}},\"auto_push\":null,\"bandwidth\":{\"exceptions\":[\"::1\",\"127.0.0.0/8\"]},\"config\":{\"accesslog\":\"LOG\",\"controller\":{\"interface\":\"127.0.0.1\",\"port\":$api_port,\"username\":null},\"debug\":4,\"defaultStream\":null,\"prometheus\":\"\",\"protocols\":[{\"connector\":\"RTMP\",\"interface\":\"127.0.0.1\",\"port\":$rtmp_port}],\"serverid\":null,\"sessionInputMode\":15,\"sessionOutputMode\":15,\"sessionStreamInfoMode\":1,\"sessionUnspecifiedMode\":0,\"sessionViewerMode\":14,\"tknMode\":15,\"triggers\":{\"PROCESS_EXIT\":[{\"handler\":\"$handler\",\"sync\":false,\"streams\":[\"$stream\"]}]},\"trustedproxy\":[]},\"extwriters\":null,\"jwks\":null,\"push_settings\":{\"maxspeed\":0,\"wait\":3},\"streamkeys\":null,\"streams\":{\"$stream\":{\"name\":\"$stream\",\"source\":\"push://\",\"processes\":[{\"process\":\"AV\",\"codec\":\"opus\",\"track_inhibit\":\"audio=opus\",\"track_select\":\"audio=all&video=none&subtitle=none&meta=none\"}]}},\"variables\":null}" \
  >"$config"

TMP="$ipc_root" MIST_CONTROL=1 "$controller" -c "$config" -C r -L "$work/controller.log" &
controller_pid=$!

log_count() {
  grep -cE -- "$1" "$work/controller.log" 2>/dev/null || true
}

wait_for_log() {
  pattern=$1
  count=$2
  limit=$3
  attempt=0
  while [ "$attempt" -lt "$limit" ]; do
    if [ "$(log_count "$pattern")" -ge "$count" ]; then
      return 0
    fi
    if ! kill -0 "$controller_pid" 2>/dev/null; then
      return 1
    fi
    attempt=$((attempt + 1))
    sleep 0.5
  done
  return 1
}

if ! wait_for_log 'Controller started' 1 60 || ! wait_for_log 'Started connector' 1 60; then
  echo "test controller or its RTMP connector did not become ready" >&2
  exit 1
fi

# Each session uses its own resolution and sample rate, so it cannot resume
# another session's tracks. Without onMetaData no JSON meta track takes index 0.
publish() {
  name=$1
  size=$2
  rate=$3
  duration=$4
  "$timeout_program" $((duration + 20)) "$ffmpeg" -hide_banner -loglevel error -re \
    -f lavfi -i "testsrc2=size=$size:rate=25:duration=$duration" \
    -f lavfi -i "sine=frequency=997:sample_rate=$rate:duration=$duration" \
    -c:v libx264 -pix_fmt yuv420p -preset ultrafast -g 25 -keyint_min 25 -bf 0 \
    -c:a aac -b:a 96k -flvflags no_metadata -f flv "rtmp://127.0.0.1:$rtmp_port/live/$stream" \
    >"$work/$name.log" 2>&1 &
  publisher_pids="$publisher_pids $!"
  last_publisher=$!
}

# Session 1 owns tracks 0 (video) and 1 (audio); the processor adds opus.
publish publisher1 320x180 44100 10
publisher1_pid=$last_publisher
if ! wait_for_log 'opus track index is' 1 30; then
  echo "the first publisher session never produced an opus output track" >&2
  exit 1
fi

# Session 2 overlaps session 1, so the buffer accepts it while media still flows.
publish publisher2 640x360 48000 40
wait "$publisher1_pid" || true
sleep 1

# Session 3 registers after session 1 left: session 1's retained tracks are
# removed, and the processor reading them restarts without a track 0.
publish publisher3 480x270 32000 30
if ! wait_for_log 'Removing track 0 retained from the previous publisher session' 1 30; then
  echo "track 0 was not removed; the fixture no longer exercises a replaced publisher" >&2
  exit 1
fi
if ! wait_for_log 'Started process .*MistProcAV' 2 60; then
  echo "the audio processor was not restarted after its source track was removed" >&2
  exit 1
fi
restarted_at=$(log_count 'opus track index is')
if ! wait_for_log 'opus track index is' $((restarted_at + 1)) 30; then
  echo "the restarted audio processor produced no opus output" >&2
  exit 1
fi
# Leave time for a crash to be reaped and for its restart to be reported.
sleep 8

# PROCESS_EXIT line 5 is the exit code: negative when a signal killed the
# processor, 2 when it gave up. Only clean exits are expected here.
if [ -f "$exit_log" ] && awk 'BEGIN { RS = "\n---\n" } { split($0, l, "\n"); if (l[2] == "AV" && l[5] != "0") bad = 1 } END { exit bad ? 0 : 1 }' "$exit_log"; then
  echo "the audio processor failed after track 0 was removed" >&2
  exit 1
fi
av_starts=$(log_count 'Started process .*MistProcAV')
if [ "$av_starts" -gt 2 ]; then
  echo "the audio processor started $av_starts times; expected the initial start and one restart" >&2
  exit 1
fi
if grep -q 'Opus does not support a bitrate' "$work/controller.log"; then
  echo "the audio processor was configured with a bitrate Opus cannot use" >&2
  exit 1
fi

echo "the audio processor kept transcoding to opus after track 0 was removed ($av_starts starts)"
