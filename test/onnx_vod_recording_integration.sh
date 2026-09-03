#!/bin/sh
set -eu

if [ "$#" -ne 13 ]; then
  echo "usage: $0 ffmpeg ffprobe MistController MistInEBML MistInBuffer MistProcAV MistProcONNX MistOutEBML MistSession MistUtilLog MistUtilNuke MistAnalyserEBML procstateprobe" >&2
  exit 2
fi

if [ "${MIST_RUN_MEDIA_TESTS:-}" != "1" ]; then
  echo "set MIST_RUN_MEDIA_TESTS=1 to run the model-backed ONNX recording pipeline" >&2
  exit 77
fi
if [ -z "${MIST_ONNX_TEST_MODEL:-}" ] || [ ! -f "$MIST_ONNX_TEST_MODEL" ]; then
  echo "set MIST_ONNX_TEST_MODEL to a local yolo26n ONNX model" >&2
  exit 77
fi

ffmpeg=$1
ffprobe=$2
controller=$3
input_ebml=$4
input_buffer=$5
process_av=$6
process_onnx=$7
output_ebml=$8
session=$9
util_log=${10}
util_nuke=${11}
analyser_ebml=${12}
proc_state_probe=${13}

for program in "$ffmpeg" "$ffprobe" "$controller" "$input_ebml" "$input_buffer" \
  "$process_av" "$process_onnx" "$output_ebml" "$session" "$util_log" "$util_nuke" "$analyser_ebml" \
  "$proc_state_probe"; do
  if [ ! -x "$program" ]; then
    echo "required executable is unavailable: $program" >&2
    exit 77
  fi
done

if ! "$ffmpeg" -hide_banner -encoders 2>/dev/null | grep -q 'libx264'; then
  echo "ffmpeg lacks the libx264 encoder required for the ONNX fixture" >&2
  exit 77
fi

work=$(mktemp -d "${TMPDIR:-/tmp}/mist-onnx-recording.XXXXXX")
ipc_root="$work/ipc"
mkdir -p "$ipc_root"
stream="onnxrecord$$"
controller_pid=
input_pid=

cleanup() {
  status=$?
  trap - EXIT HUP INT TERM
  if [ "$status" -ne 0 ]; then
    echo "model-backed ONNX recording integration failed; logs follow:" >&2
    for log in "$work"/*.log; do
      if [ -f "$log" ]; then
        echo "Log: $log" >&2
        tail -180 "$log" >&2
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
  -f lavfi -i sine=frequency=997:sample_rate=48000:duration=30 \
  -c:v libx264 -pix_fmt yuv420p -preset veryfast -g 20 -keyint_min 20 -bf 0 -sc_threshold 0 \
  -c:a aac -b:a 96k "$source_mkv"

port=$((26000 + ($$ % 10000)))
config="$work/config.json"
printf '%s\n' \
  "{\"account\":{\"test\":{\"password\":\"098f6bcd4621d373cade4e832627b4f6\"}},\"auto_push\":null,\"bandwidth\":{\"exceptions\":[\"::1\",\"127.0.0.0/8\"]},\"config\":{\"accesslog\":\"LOG\",\"controller\":{\"interface\":\"127.0.0.1\",\"port\":$port,\"username\":null},\"debug\":4,\"defaultStream\":null,\"prometheus\":\"\",\"protocols\":[],\"serverid\":null,\"sessionInputMode\":15,\"sessionOutputMode\":15,\"sessionStreamInfoMode\":1,\"sessionUnspecifiedMode\":0,\"sessionViewerMode\":14,\"tknMode\":15,\"triggers\":{},\"trustedproxy\":[]},\"extwriters\":null,\"jwks\":null,\"push_settings\":{\"maxspeed\":0,\"wait\":3},\"streamkeys\":null,\"streams\":{\"$stream\":{\"name\":\"$stream\",\"source\":\"$source_mkv\",\"process_controlled_realtime\":true,\"realtime_speed\":1,\"processes\":[{\"process\":\"AV\",\"x-LSP-kind\":\"video\",\"codec\":\"NV12\",\"track_select\":\"video=H264&audio=none\",\"target_mask\":4,\"restart_type\":\"disabled\"},{\"process\":\"ONNX\",\"model\":\"custom\",\"model_path\":\"$MIST_ONNX_TEST_MODEL\",\"model_type\":\"yolo-nms\",\"input_size\":640,\"process_every_nth\":2,\"track_select\":\"video=NV12&audio=none\",\"target_mask\":2,\"restart_type\":\"disabled\"}]}},\"variables\":null}" \
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

TMP="$ipc_root" MIST_CONTROL=1 "$input_ebml" -r -s "$stream" "$source_mkv" >"$work/input.log" 2>&1 &
input_pid=$!

started=0
attempt=0
while [ "$attempt" -lt 30 ]; do
  if grep -q 'Input started' "$work/input.log" 2>/dev/null; then
    started=1
    break
  fi
  if ! kill -0 "$input_pid" 2>/dev/null; then
    break
  fi
  attempt=$((attempt + 1))
  sleep 1
done
if [ "$started" -ne 1 ]; then
  echo "canonical input did not become ready" >&2
  exit 1
fi

recording="$work/recording.mkv"
TMP="$ipc_root" MIST_CONTROL=1 "$output_ebml" -s "$stream" "$recording?duration=8" >"$work/output.log" 2>&1

"$ffmpeg" -hide_banner -loglevel error -i "$recording" -map 0:v:0 -map 0:a:0 -f null - 2>"$work/decode.log"
if [ -s "$work/decode.log" ]; then
  echo "ONNX-enriched recording emitted decoder/demuxer diagnostics" >&2
  exit 1
fi

video_streams=$("$ffprobe" -v error -select_streams v -show_entries stream=index -of csv=p=0 "$recording" | wc -l | tr -d ' ')
audio_streams=$("$ffprobe" -v error -select_streams a -show_entries stream=index -of csv=p=0 "$recording" | wc -l | tr -d ' ')
if [ "$video_streams" -ne 1 ] || [ "$audio_streams" -ne 1 ]; then
  echo "recording has $video_streams video/$audio_streams audio streams; expected one of each" >&2
  exit 1
fi

"$analyser_ebml" -D 2 "$recording" >"$work/analyser.log" 2>&1
if ! grep -q 'CodecID.*M_JSON' "$work/analyser.log"; then
  echo "recording has no M_JSON ONNX metadata track" >&2
  exit 1
fi
last_video_time=$("$ffprobe" -v error -select_streams v:0 -show_entries packet=pts_time -of csv=p=0 "$recording" | tail -1)
data_packets=$(grep -c 'SimpleBlock.*track 4 @' "$work/analyser.log" || true)
last_data_time=$(awk '
  /\[Timecode\] =/ { cluster = $NF }
  /SimpleBlock.*track 4 @/ {
    for (i = 1; i <= NF; ++i) { if ($i == "@") { last = (cluster + $(i + 1)) / 1000 } }
  }
  END { printf "%.3f", last }
' "$work/analyser.log")
awk -v video="$last_video_time" -v data="$last_data_time" '
  BEGIN { if (video < 7.8 || data < 7.6) exit 1 }
' || {
  echo "recording tail is incomplete: last video=$last_video_time, last ONNX result=$last_data_time" >&2
  exit 1
}
if [ "$data_packets" -lt 20 ]; then
  echo "recording contains only $data_packets ONNX result packets" >&2
  exit 1
fi
if ! grep -q 'Waiting for processing tracks' "$work/output.log"; then
  echo "recording never observed the ONNX output-contract readiness gate" >&2
  exit 1
fi
if ! grep -q 'Waiting for processing process expectations' "$work/output.log"; then
  echo "recording did not start before the chained ONNX process contract resolved" >&2
  exit 1
fi
if ! grep -q 'ONNX model loaded successfully' "$work/input.log"; then
  echo "the production ONNX process did not load and run the model" >&2
  exit 1
fi
onnx_pid=$(sed -n 's/.*Started process \([0-9][0-9]*\): .*MistProcONNX.*/\1/p' "$work/input.log" | tail -1)
if [ -z "$onnx_pid" ]; then
  echo "could not identify the ONNX process for shared-state validation" >&2
  exit 1
fi
TMP="$ipc_root" "$proc_state_probe" "$onnx_pid" 20 1 1 >"$work/proc-state.log"

echo "model-backed ONNX recording captured $data_packets results through $last_data_time seconds with complete A/V"
