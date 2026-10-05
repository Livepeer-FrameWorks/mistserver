#!/bin/sh
# A bounded cut (start and duration) of a live stream over a rendition whose
# process was killed. The rendition track stays in the buffer, unclaimed, with
# data up to the kill. Each cut reaches past the live point, so it reads the
# live buffer: it keeps every rendition packet it has in range and ends at its
# stop position instead of waiting for data no producer can deliver.
set -eu

if [ "$#" -ne 9 ]; then
  echo "usage: $0 ffmpeg ffprobe MistController MistInBuffer MistOutRTMP MistProcAV MistOutEBML MistUtilNuke timeout" >&2
  exit 2
fi
if [ "${MIST_RUN_MEDIA_TESTS:-}" != "1" ]; then
  echo "set MIST_RUN_MEDIA_TESTS=1 to run the live bounded cut pipeline" >&2
  exit 77
fi

ffmpeg=$1
ffprobe=$2
controller=$3
input_buffer=$4
output_rtmp=$5
process_av=$6
output_ebml=$7
util_nuke=$8
timeout_program=$9
for program in "$ffmpeg" "$ffprobe" "$controller" "$input_buffer" "$output_rtmp" "$process_av" "$output_ebml" \
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

work=$(mktemp -d "${TMPDIR:-/tmp}/mist-bounded-cut.XXXXXX")
ipc_root="$work/ipc"
mkdir -p "$ipc_root"
stream="boundedcut$$"
controller_pid=
publisher_pid=

cleanup() {
  status=$?
  trap - EXIT HUP INT TERM
  if [ "$status" -ne 0 ]; then
    echo "bounded cut integration failed; logs follow:" >&2
    for log in "$work"/*.log; do
      if [ -f "$log" ]; then
        echo "Log: $log" >&2
        grep -vE 'Type mismatch|Codec mismatch|bootmsoffset' "$log" | tail -60 >&2
      fi
    done
  fi
  if [ -n "$publisher_pid" ]; then
    kill -TERM "$publisher_pid" >/dev/null 2>&1 || true
    wait "$publisher_pid" >/dev/null 2>&1 || true
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

api_port=$((26000 + ($$ % 9000)))
rtmp_port=$((api_port + 1))
config="$work/config.json"
printf '%s\n' \
  "{\"account\":{\"test\":{\"password\":\"098f6bcd4621d373cade4e832627b4f6\"}},\"auto_push\":null,\"bandwidth\":{\"exceptions\":[\"::1\",\"127.0.0.0/8\"]},\"config\":{\"accesslog\":\"LOG\",\"controller\":{\"interface\":\"127.0.0.1\",\"port\":$api_port,\"username\":null},\"debug\":4,\"defaultStream\":null,\"prometheus\":\"\",\"protocols\":[{\"connector\":\"RTMP\",\"interface\":\"127.0.0.1\",\"port\":$rtmp_port}],\"serverid\":null,\"sessionInputMode\":15,\"sessionOutputMode\":15,\"sessionStreamInfoMode\":1,\"sessionUnspecifiedMode\":0,\"sessionViewerMode\":14,\"tknMode\":15,\"triggers\":{},\"trustedproxy\":[]},\"extwriters\":null,\"jwks\":null,\"push_settings\":{\"maxspeed\":0,\"wait\":3},\"streamkeys\":null,\"streams\":{\"$stream\":{\"name\":\"$stream\",\"source\":\"push://\",\"processes\":[{\"process\":\"AV\",\"x-LSP-kind\":\"video\",\"codec\":\"H264\",\"bitrate\":300000,\"gopsize\":25,\"preset\":\"ultrafast\",\"tune\":\"zerolatency\",\"track_select\":\"video=H264&audio=none\",\"restart_type\":\"disabled\"}]}},\"variables\":null}" \
  >"$config"

TMP="$ipc_root" MIST_CONTROL=1 "$controller" -c "$config" -C r -L "$work/controller.log" &
controller_pid=$!
wait_for 30 "the test controller and its RTMP connector" logged "$work/controller.log" 'Started connector'

"$timeout_program" 220 "$ffmpeg" -hide_banner -loglevel error -re \
  -f lavfi -i "testsrc2=size=320x180:rate=25:duration=200" \
  -f lavfi -i "sine=frequency=997:sample_rate=48000:duration=200" \
  -c:v libx264 -pix_fmt yuv420p -preset ultrafast -g 25 -keyint_min 25 -bf 0 \
  -c:a aac -b:a 96k -f flv "rtmp://127.0.0.1:$rtmp_port/live/$stream" \
  >"$work/publisher.log" 2>&1 &
publisher_pid=$!
wait_for 20 "the stream to become active" logged "$work/controller.log" "Stream $stream became active"

av_pid() { pgrep -f "MistProcAV.*$stream" | head -n 1; }
running_av() { [ -n "$(av_pid)" ]; }
wait_for 20 "the rendition process to start" running_av
# Let the rendition produce a few seconds, then kill its process: its track
# stays in the buffer, unclaimed, ending at the kill.
sleep 8
kill -KILL "$(av_pid)"
sleep 1

# cut <name> <seconds back> <duration>: a cut that ends past the current live
# point, so it reads the live buffer up to its stop position. Prints how long
# it took.
cut() {
  began=$(date +%s)
  "$timeout_program" 90 env TMP="$ipc_root" MIST_CONTROL=1 "$output_ebml" -s "$stream" \
    "$work/$1.mkv?startunix=-$2&duration=$3&video=all&audio=all&rate=0" >"$work/$1.log" 2>&1 || true
  took=$(($(date +%s) - began))
  if ! grep -q 'planned stopping point reached' "$work/$1.log"; then
    echo "cut $1 did not end at its planned stop" >&2
    exit 1
  fi
  streams=$("$ffprobe" -v error -select_streams v -show_entries stream=index -of csv=p=0 "$work/$1.mkv" | wc -l | tr -d ' ')
  if [ "$streams" -ne 2 ]; then
    echo "cut $1 declares $streams video tracks; expected the source and the rendition" >&2
    exit 1
  fi
  # The range reaches about 4 s past the live point; anything much longer waited on the rendition.
  if [ "$took" -gt 10 ]; then
    echo "cut $1 took ${took}s; it waited on rendition data that cannot arrive" >&2
    exit 1
  fi
}
packets() {
  count=$("$ffprobe" -v error -select_streams "v:$2" -count_packets -show_entries stream=nb_read_packets \
    -of default=nw=1:nk=1 "$work/$1.mkv")
  case "$count" in '' | N/A) echo 0 ;; *) echo "$count" ;; esac
}

# Spanning the kill: the rendition's packets from before it stay in the cut.
cut spanning 6 10
spanning_took=$took
spanning_rendition=$(packets spanning 1)
if [ "$spanning_rendition" -lt 25 ]; then
  echo "the spanning cut kept $spanning_rendition rendition packets from before the kill; expected the ones in range" >&2
  exit 1
fi
# Entirely after the kill: the seek position lies past the rendition's last
# packet, and nothing can arrive to reach it.
cut after 1 5
echo "bounded cuts over a stopped rendition: spanning ${spanning_took}s ($spanning_rendition rendition packets), after ${took}s ($(packets after 1) rendition packets)"
