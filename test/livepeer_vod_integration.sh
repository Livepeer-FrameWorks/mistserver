#!/bin/sh
set -eu

if [ "$#" -ne 13 ]; then
  echo "usage: $0 ffmpeg ffprobe MistController MistInEBML MistInBuffer MistProcLivepeer MistOutEBML MistSession MistUtilLog MistUtilNuke broadcaster-stub timeout proc-state-probe" >&2
  exit 2
fi
if [ "${MIST_RUN_MEDIA_TESTS:-}" != "1" ]; then
  echo "set MIST_RUN_MEDIA_TESTS=1 to run the Livepeer VOD pipeline" >&2
  exit 77
fi

ffmpeg=$1
ffprobe=$2
controller=$3
input_ebml=$4
input_buffer=$5
process_livepeer=$6
output_ebml=$7
session=$8
util_log=$9
util_nuke=${10}
broadcaster_stub=${11}
timeout_program=${12}
proc_state_probe=${13}

for program in "$ffmpeg" "$ffprobe" "$controller" "$input_ebml" "$input_buffer" \
  "$process_livepeer" "$output_ebml" "$session" "$util_log" "$util_nuke" \
  "$broadcaster_stub" "$timeout_program" "$proc_state_probe"; do
  if [ ! -x "$program" ]; then
    echo "required executable is unavailable: $program" >&2
    exit 77
  fi
done
if ! "$ffmpeg" -hide_banner -encoders 2>/dev/null | grep -q 'libx264'; then
  echo "ffmpeg lacks the libx264 encoder required for the Livepeer fixture" >&2
  exit 77
fi

work=$(mktemp -d "${TMPDIR:-/tmp}/mist-livepeer-vod.XXXXXX")
ipc_root="$work/ipc"
mkdir -p "$ipc_root"
stream="livepeervod$$"
controller_pid=
input_pid=
stub_pid=
output_pid=

cleanup() {
  status=$?
  trap - EXIT HUP INT TERM
  if [ "$status" -ne 0 ]; then
    echo "Livepeer VOD integration failed; logs follow:" >&2
    for log in "$work"/*.log; do
      if [ -f "$log" ]; then
        echo "Log: $log" >&2
        tail -180 "$log" >&2
      fi
    done
  fi
  if [ -n "$output_pid" ]; then
    kill -TERM "$output_pid" >/dev/null 2>&1 || true
    wait "$output_pid" >/dev/null 2>&1 || true
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
  if [ -n "$stub_pid" ]; then
    kill -TERM "$stub_pid" >/dev/null 2>&1 || true
    wait "$stub_pid" >/dev/null 2>&1 || true
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
  -f lavfi -i testsrc2=size=320x180:rate=10:duration=30 \
  -f lavfi -i sine=frequency=701:sample_rate=48000:duration=30 \
  -c:v libx264 -pix_fmt yuv420p -preset veryfast -g 20 -keyint_min 20 -bf 0 -sc_threshold 0 \
  -c:a aac -b:a 96k "$source_mkv"

controller_port=$((28000 + ($$ % 8000)))
broadcaster_port=$((38000 + ($$ % 8000)))
"$broadcaster_stub" "$broadcaster_port" >"$work/broadcaster.log" 2>&1 &
stub_pid=$!
attempt=0
while [ "$attempt" -lt 50 ] && ! grep -q '^ready$' "$work/broadcaster.log" 2>/dev/null; do
  if ! kill -0 "$stub_pid" 2>/dev/null; then break; fi
  attempt=$((attempt + 1))
  sleep 0.1
done
if ! grep -q '^ready$' "$work/broadcaster.log"; then
  echo "loopback Livepeer broadcaster did not become ready" >&2
  exit 1
fi

config="$work/config.json"
printf '%s\n' \
  "{\"account\":{\"test\":{\"password\":\"098f6bcd4621d373cade4e832627b4f6\"}},\"auto_push\":null,\"bandwidth\":{\"exceptions\":[\"::1\",\"127.0.0.0/8\"]},\"config\":{\"accesslog\":\"LOG\",\"controller\":{\"interface\":\"127.0.0.1\",\"port\":$controller_port,\"username\":null},\"debug\":4,\"defaultStream\":null,\"prometheus\":\"\",\"protocols\":[],\"serverid\":null,\"sessionInputMode\":15,\"sessionOutputMode\":15,\"sessionStreamInfoMode\":1,\"sessionUnspecifiedMode\":0,\"sessionViewerMode\":14,\"tknMode\":15,\"triggers\":{},\"trustedproxy\":[]},\"extwriters\":null,\"jwks\":null,\"push_settings\":{\"maxspeed\":0,\"wait\":3},\"streamkeys\":null,\"streams\":{\"$stream\":{\"name\":\"$stream\",\"source\":\"$source_mkv\",\"process_controlled_realtime\":true,\"realtime_speed\":4,\"processes\":[{\"process\":\"Livepeer\",\"hardcoded_broadcasters\":\"http://127.0.0.1:$broadcaster_port\",\"target_profiles\":[{\"name\":\"audit\",\"bitrate\":500000,\"width\":320,\"height\":180,\"fps\":10,\"gop\":\"2.0\"}],\"target_mask\":2,\"restart_type\":\"disabled\"}]}},\"variables\":null}" \
  >"$config"

TMP="$ipc_root" MIST_CONTROL=1 "$controller" -c "$config" -C r -L "$work/controller.log" &
controller_pid=$!
attempt=0
while [ "$attempt" -lt 30 ] && ! grep -q 'Controller started' "$work/controller.log" 2>/dev/null; do
  if ! kill -0 "$controller_pid" 2>/dev/null; then break; fi
  attempt=$((attempt + 1))
  sleep 1
done
if ! grep -q 'Controller started' "$work/controller.log"; then
  echo "test controller did not become ready" >&2
  exit 1
fi

TMP="$ipc_root" MIST_CONTROL=1 "$input_ebml" -r -s "$stream" "$source_mkv" >"$work/input.log" 2>&1 &
input_pid=$!
attempt=0
while [ "$attempt" -lt 30 ] && ! grep -q 'Input started' "$work/input.log" 2>/dev/null; do
  if ! kill -0 "$input_pid" 2>/dev/null; then break; fi
  attempt=$((attempt + 1))
  sleep 1
done
if ! grep -q 'Input started' "$work/input.log"; then
  echo "canonical input did not become ready" >&2
  exit 1
fi

recording="$work/recording.mkv"
"$timeout_program" 90 env TMP="$ipc_root" MIST_CONTROL=1 "$output_ebml" -s "$stream" "$recording?stop=29500" \
  >"$work/output.log" 2>&1 &
output_pid=$!

# ProcState is PID-scoped and intentionally disappears with its writer. Sample it while the
# processor is alive rather than racing process shutdown after the completed recording.
livepeer_pid=
proc_state_read=0
attempt=0
while [ "$attempt" -lt 150 ]; do
  livepeer_pid=$(sed -n 's/.*Started process \([0-9][0-9]*\): .*MistProcLivepeer.*/\1/p' "$work/input.log" | tail -1)
  if [ -n "$livepeer_pid" ] &&
     TMP="$ipc_root" "$proc_state_probe" "$livepeer_pid" >"$work/proc-state.log" 2>/dev/null; then
    proc_state_read=1
    break
  fi
  if ! kill -0 "$output_pid" 2>/dev/null; then
    break
  fi
  attempt=$((attempt + 1))
  sleep 0.1
done
if [ "$proc_state_read" -ne 1 ]; then
  echo "Livepeer did not publish a readable ProcState snapshot while running" >&2
  exit 1
fi

wait "$output_pid"
output_pid=

"$ffmpeg" -hide_banner -loglevel error -i "$recording" -map 0 -f null - 2>"$work/decode.log"
if [ -s "$work/decode.log" ]; then
  echo "Livepeer recording emitted decoder/demuxer diagnostics" >&2
  exit 1
fi
video_streams=$("$ffprobe" -v error -select_streams v -show_entries stream=index -of csv=p=0 "$recording" | wc -l | tr -d ' ')
audio_streams=$("$ffprobe" -v error -select_streams a -show_entries stream=index -of csv=p=0 "$recording" | wc -l | tr -d ' ')
if [ "$video_streams" -ne 1 ] || [ "$audio_streams" -ne 1 ]; then
  echo "recording has $video_streams video/$audio_streams audio streams; expected one selected Livepeer video and source audio" >&2
  exit 1
fi
video_tail=$("$ffprobe" -v error -select_streams v:0 -show_entries packet=pts_time -of csv=p=0 "$recording" | tail -1)
awk -v video="$video_tail" 'BEGIN { if (video < 29.35) exit 1 }' || {
  echo "Livepeer recording tail is incomplete: rendition=$video_tail" >&2
  exit 1
}
tail_finalizations=$(grep -c 'Finalizing Livepeer tail segment' "$work/input.log" || true)
if [ "$tail_finalizations" -ne 1 ]; then
  echo "Livepeer finalized its EOF tail $tail_finalizations times; expected exactly once" >&2
  exit 1
fi
if ! grep -q 'Stripping target options: audio=none&video=maxbps' "$work/input.log"; then
  echo "Livepeer source did not apply its video-only TS selector" >&2
  exit 1
fi
if grep -q 'Creating new (delayed) track .*: AAC audio' "$work/input.log"; then
  echo "Livepeer unexpectedly uploaded and returned a derived AAC track" >&2
  exit 1
fi
if ! grep -q 'Clean shutdown; joining threads' "$work/input.log"; then
  echo "Livepeer process did not reach deterministic thread shutdown" >&2
  exit 1
fi
first_response=$(grep '^responded ' "$work/broadcaster.log" | head -1 | cut -d' ' -f2)
if [ "$first_response" != "1" ]; then
  echo "broadcaster did not complete segment 1 before segment 0; ordering path was not exercised" >&2
  exit 1
fi
echo "Livepeer loopback recording retained the processed video tail through $video_tail seconds"
