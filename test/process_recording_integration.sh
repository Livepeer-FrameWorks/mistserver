#!/bin/sh
set -eu

if [ "$#" -ne 11 ]; then
  echo "usage: $0 ffmpeg ffprobe MistController MistInMP4 MistInBuffer MistProcAV MistOutEBML MistSession MistUtilLog MistUtilNuke timeout" >&2
  exit 2
fi

if [ "${MIST_RUN_MEDIA_TESTS:-}" != "1" ]; then
  echo "set MIST_RUN_MEDIA_TESTS=1 to run the isolated processing recording pipeline" >&2
  exit 77
fi

ffmpeg=$1
ffprobe=$2
controller=$3
input_mp4=$4
input_buffer=$5
process_av=$6
output_ebml=$7
session=$8
util_log=$9
util_nuke=${10}
timeout_program=${11}
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
trigger_handler="$script_dir/capture_trigger.sh"

for program in "$ffmpeg" "$ffprobe" "$controller" "$input_mp4" "$input_buffer" \
  "$process_av" "$output_ebml" "$session" "$util_log" "$util_nuke" "$timeout_program"; do
  if [ ! -x "$program" ]; then
    echo "required executable is unavailable: $program" >&2
    exit 77
  fi
done
if [ ! -x "$trigger_handler" ]; then
  echo "required trigger capture helper is unavailable: $trigger_handler" >&2
  exit 77
fi

if ! "$ffmpeg" -hide_banner -encoders 2>/dev/null | grep -q 'libx264'; then
  echo "ffmpeg lacks the libx264 encoder required for the processing fixture" >&2
  exit 77
fi

work=$(mktemp -d "${TMPDIR:-/tmp}/mist-process-recording.XXXXXX")
ipc_root="$work/ipc"
mkdir -p "$ipc_root"
stream="procrecord$$"
trigger_base="$work/trigger"
recording_trigger_file="$trigger_base.RECORDING_END"
process_trigger_file="$trigger_base.PROCESS_EXIT"
export MIST_TEST_TRIGGER_OUTPUT="$trigger_base"
controller_pid=
input_pid=

cleanup() {
  status=$?
  trap - EXIT HUP INT TERM
  if [ "$status" -ne 0 ]; then
    echo "processing recording integration failed; logs follow:" >&2
    for log in "$work"/*.log; do
      if [ -f "$log" ]; then
        echo "Log: $log" >&2
        tail -150 "$log" >&2
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

source_media="$work/source.mp4"
source_duration=${MIST_PROCESS_SOURCE_DURATION:-30}
source_bframes=${MIST_PROCESS_SOURCE_BFRAMES:-0}
"$ffmpeg" -hide_banner -loglevel error -y \
  -f lavfi -i testsrc2=size=320x180:rate=25:duration="$source_duration" \
  -f lavfi -i sine=frequency=997:sample_rate=48000:duration="$source_duration" \
  -c:v libx264 -pix_fmt yuv420p -preset veryfast -g 50 -keyint_min 50 -bf "$source_bframes" -sc_threshold 0 \
  -c:a aac -b:a 96k -movflags +faststart "$source_media"

port=$((24000 + ($$ % 12000)))
config="$work/config.json"
printf '%s\n' \
  "{\"account\":{\"test\":{\"password\":\"098f6bcd4621d373cade4e832627b4f6\"}},\"auto_push\":null,\"bandwidth\":{\"exceptions\":[\"::1\",\"127.0.0.0/8\"]},\"config\":{\"accesslog\":\"LOG\",\"controller\":{\"interface\":\"127.0.0.1\",\"port\":$port,\"username\":null},\"debug\":4,\"defaultStream\":null,\"prometheus\":\"\",\"protocols\":[],\"serverid\":null,\"sessionInputMode\":15,\"sessionOutputMode\":15,\"sessionStreamInfoMode\":1,\"sessionUnspecifiedMode\":0,\"sessionViewerMode\":14,\"tknMode\":15,\"triggers\":{\"PROCESS_EXIT\":[{\"handler\":\"$trigger_handler\",\"sync\":false,\"streams\":[\"$stream\"]}],\"RECORDING_END\":[{\"handler\":\"$trigger_handler\",\"sync\":false,\"streams\":[\"$stream\"]}]},\"trustedproxy\":[]},\"extwriters\":null,\"jwks\":null,\"push_settings\":{\"maxspeed\":0,\"wait\":3},\"streamkeys\":null,\"streams\":{\"$stream\":{\"name\":\"$stream\",\"source\":\"$source_media\",\"process_controlled_realtime\":true,\"realtime_speed\":1,\"processes\":[{\"process\":\"AV\",\"x-LSP-kind\":\"video\",\"codec\":\"H264\",\"bitrate\":1000000,\"gopsize\":250,\"preset\":\"ultrafast\",\"tune\":\"zerolatency\",\"track_select\":\"video=H264&audio=none\",\"source_mask\":4,\"target_mask\":3,\"restart_type\":\"fixed\"}]}},\"variables\":null}" \
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

# Own the input explicitly so the recorder and processor cannot race each other
# into booting separate connector chains for the same stream.
TMP="$ipc_root" MIST_CONTROL=1 "$input_mp4" -r -s "$stream" "$source_media" >"$work/input.log" 2>&1 &
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
if [ "${MIST_PROCESS_EXPECT_EOF:-}" = "1" ]; then
  "$timeout_program" 60 env TMP="$ipc_root" MIST_CONTROL=1 "$output_ebml" -s "$stream" "$recording" \
    >"$work/output.log" 2>&1
  expected_span="$source_duration"
else
  TMP="$ipc_root" MIST_CONTROL=1 "$output_ebml" -s "$stream" "$recording?duration=4" >"$work/output.log" 2>&1
  expected_span=4
fi

"$ffmpeg" -hide_banner -loglevel error -i "$recording" -f null - 2>"$work/decode.log"
if [ -s "$work/decode.log" ]; then
  echo "process-controlled recording emitted decoder/demuxer diagnostics" >&2
  exit 1
fi

video_streams=$("$ffprobe" -v error -select_streams v -show_entries stream=index -of csv=p=0 "$recording" | wc -l | tr -d ' ')
audio_streams=$("$ffprobe" -v error -select_streams a -show_entries stream=index -of csv=p=0 "$recording" | wc -l | tr -d ' ')
if [ "$video_streams" -ne 1 ] || [ "$audio_streams" -ne 1 ]; then
  echo "recording has $video_streams video/$audio_streams audio streams; expected one of each" >&2
  exit 1
fi

video_timing=$("$ffprobe" -v error -select_streams v:0 \
  -show_entries packet=pts_time,duration_time,flags -of csv=p=0 "$recording" | \
  awk -F, '
    NR == 1 { first = $1; first_flags = $3 }
    { last_end = $1 + (($2 == "N/A") ? 0 : $2); packets++ }
    END { printf "%.6f %.6f %s %d", first, last_end, first_flags, packets }
  ')
set -- $video_timing
first_video_time=$1
last_video_end=$2
first_video_flags=$3
video_packets=$4
case "$first_video_flags" in
  K*) ;;
  *)
    echo "processed video begins with flags '$first_video_flags', not a keyframe" >&2
    exit 1
    ;;
esac
awk -v first="$first_video_time" -v last="$last_video_end" -v expected="$expected_span" '
  BEGIN { span = last - first; if (span < expected - 0.1 || span > expected + 0.1) exit 1 }
' || {
  echo "processed video packet span $first_video_time..$last_video_end is outside the requested interval" >&2
  exit 1
}
if [ "${MIST_PROCESS_EXPECT_EOF:-}" = "1" ]; then
  source_first_video_time=$("$ffprobe" -v error -select_streams v:0 \
    -show_entries packet=pts_time -of csv=p=0 "$source_media" | sed -n '1p')
  awk -v source="$source_first_video_time" -v processed="$first_video_time" '
    BEGIN { delta = source - processed; if (delta < 0) delta = -delta; if (delta > 0.001) exit 1 }
  ' || {
    echo "processed video starts at $first_video_time; source starts at $source_first_video_time" >&2
    exit 1
  }
  source_video_packets=$("$ffprobe" -v error -select_streams v:0 -count_packets \
    -show_entries stream=nb_read_packets -of default=nw=1:nk=1 "$source_media")
  if [ "$video_packets" -ne "$source_video_packets" ]; then
    echo "processed video ended with $video_packets packets; source contains $source_video_packets" >&2
    exit 1
  fi
  "$ffmpeg" -hide_banner -i "$source_media" -i "$recording" \
    -lavfi '[0:v]setpts=PTS-STARTPTS[src];[1:v]setpts=PTS-STARTPTS[processed];[src][processed]ssim' \
    -an -f null - >/dev/null 2>"$work/ssim.log"
  ssim_all=$(sed -n 's/.* All:\([0-9.]*\).*/\1/p' "$work/ssim.log" | tail -n 1)
  awk -v score="$ssim_all" 'BEGIN { if (score == "" || score < 0.98) exit 1 }' || {
    echo "processed video frame sequence SSIM is ${ssim_all:-unavailable}; expected at least 0.98" >&2
    exit 1
  }
fi

attempt=0
while [ "$attempt" -lt 50 ] && [ ! -s "$recording_trigger_file" ]; do
  attempt=$((attempt + 1))
  sleep 0.1
done
if [ ! -s "$recording_trigger_file" ]; then
  echo "RECORDING_END trigger was not captured" >&2
  exit 1
fi
if [ "$(sed -n '1p' "$recording_trigger_file")" != "RECORDING_END" ]; then
  echo "captured the wrong trigger type" >&2
  exit 1
fi
trigger_summary=$(sed -n '14p' "$recording_trigger_file")
case "$trigger_summary" in
  *'"speed"'*'"ticks"'*) ;;
  *)
    echo "RECORDING_END omitted processing speed diagnostics: $trigger_summary" >&2
    exit 1
    ;;
esac
if [ "${MIST_PROCESS_EXPECT_EOF:-}" = "1" ]; then
  case "$trigger_summary" in
    *'"drain_ms"'*) ;;
    *)
      echo "RECORDING_END omitted processing drain duration: $trigger_summary" >&2
      exit 1
      ;;
  esac
  drain_ms=$(printf '%s\n' "$trigger_summary" | sed -n 's/.*"drain_ms":\([0-9][0-9]*\).*/\1/p')
  if [ -z "$drain_ms" ] || [ "$drain_ms" -ge 15000 ]; then
    echo "processing drain took ${drain_ms:-an unknown duration}ms; expected completion before the 25s data timeout" >&2
    exit 1
  fi
fi

# A duration-bounded recording is only a consumer stop: its finite source and processor are
# expected to remain online. PROCESS_EXIT is required only when this fixture drives source EOF.
if [ "${MIST_PROCESS_EXPECT_EOF:-}" = "1" ]; then
  attempt=0
  while [ "$attempt" -lt 50 ] && [ ! -s "$process_trigger_file" ]; do
    attempt=$((attempt + 1))
    sleep 0.1
  done
  if [ ! -s "$process_trigger_file" ]; then
    echo "PROCESS_EXIT trigger was not captured after source EOF" >&2
    exit 1
  fi
  if [ "$(sed -n '1p' "$process_trigger_file")" != "PROCESS_EXIT" ] ||
     [ "$(sed -n '3p' "$process_trigger_file")" != "AV" ] ||
     [ "$(sed -n '8p' "$process_trigger_file")" != "clean" ]; then
    echo "PROCESS_EXIT did not report the clean AV processor lifecycle" >&2
    sed -n '1,10p' "$process_trigger_file" >&2
    exit 1
  fi
fi
started_processes=$(grep -c 'Started process .*MistProcAV' "$work/input.log" || true)
if [ "$started_processes" -ne 1 ]; then
  echo "processor restarted $started_processes times after source EOF; expected one initial start" >&2
  exit 1
fi
if ! grep -q 'Waiting for processing' "$work/output.log"; then
  echo "recording never observed the late processing-track readiness gate" >&2
  exit 1
fi

echo "process-controlled recording waited for its AV output and finalized $video_packets decodable video packets ($first_video_time..$last_video_end, keyframe first)"
