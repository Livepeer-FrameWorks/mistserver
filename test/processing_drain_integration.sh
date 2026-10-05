#!/bin/sh
# A process-controlled recording may end only once it played out the whole
# buffer; the buffer stays up while its recorder makes progress, and a
# recording the buffer ends early reports a retryable failure (SHM_LOST).
# MIST_DRAIN_MODE selects the case:
#   empty-original  the source declares a video track without any frame; the
#                   recording still writes its header at source EOF and holds
#                   the whole source.
#   slow-sink       a 90 s source written through a sink at half the source
#                   rate, so the recorder drains for over a minute after source
#                   EOF; the recording holds the whole source.
#   stuck-sink      a sink that never reads; the buffer exits once the recorder
#                   stopped progressing for the stale window, and the recording
#                   reports SHM_LOST.
#   buffer-killed   the buffer is stopped while the recorder still drains; the
#                   recording reports SHM_LOST.
#   late-audio      a 1.5 s source whose audio starts 0.8 s after its video; both
#                   tracks are registered before any data and recorded.
#   one-frame       a source with a single video frame and 40 ms of audio; both
#                   tracks are registered and recorded.
set -eu

if [ "$#" -ne 11 ]; then
  echo "usage: $0 ffmpeg ffprobe MistController MistInMP4 MistInEBML MistInBuffer MistProcThumbs MistOutEBML MistUtilNuke timeout python3" >&2
  exit 2
fi
if [ "${MIST_RUN_MEDIA_TESTS:-}" != "1" ]; then
  echo "set MIST_RUN_MEDIA_TESTS=1 to run the processing drain pipeline" >&2
  exit 77
fi

ffmpeg=$1
ffprobe=$2
controller=$3
input_mp4=$4
input_ebml=$5
input_buffer=$6
process_thumbs=$7
output_ebml=$8
util_nuke=$9
timeout_program=${10}
python=${11}
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
trigger_handler="$script_dir/capture_trigger.sh"
for program in "$ffmpeg" "$ffprobe" "$controller" "$input_mp4" "$input_ebml" "$input_buffer" "$process_thumbs" \
  "$output_ebml" "$util_nuke" "$timeout_program" "$python" "$trigger_handler"; do
  if [ ! -x "$program" ]; then
    echo "required executable is unavailable: $program" >&2
    exit 77
  fi
done
if ! "$ffmpeg" -hide_banner -encoders 2>/dev/null | grep -q 'libx264'; then
  echo "ffmpeg lacks the libx264 encoder required for the fixture" >&2
  exit 77
fi

mode=${MIST_DRAIN_MODE:-empty-original}
case "$mode" in
  empty-original) source_duration=20 ;;
  slow-sink) source_duration=90 ;;
  stuck-sink) source_duration=20 ;;
  buffer-killed) source_duration=60 ;;
  late-audio | one-frame) source_duration=2 ;;
  *)
    echo "unknown MIST_DRAIN_MODE $mode" >&2
    exit 2
    ;;
esac

work=$(mktemp -d "${TMPDIR:-/tmp}/mist-processing-drain.XXXXXX")
ipc_root="$work/ipc"
mkdir -p "$ipc_root"
stream="procdrain$$"
trigger_base="$work/trigger"
recording_trigger_file="$trigger_base.RECORDING_END"
export MIST_TEST_TRIGGER_OUTPUT="$trigger_base"
controller_pid=
input_pid=
sink_pid=

cleanup() {
  status=$?
  trap - EXIT HUP INT TERM
  if [ "$status" -ne 0 ]; then
    echo "processing drain integration ($mode) failed; logs follow:" >&2
    for log in "$work"/*.log; do
      if [ -f "$log" ]; then
        echo "Log: $log" >&2
        grep -E 'Recording header|Waiting for processing|exit reason|Erasing|no activity|Processing feed|signal|FAIL|WARN' \
          "$log" | tail -40 >&2 || true
        tail -30 "$log" >&2
      fi
    done
  fi
  if [ -n "$sink_pid" ]; then kill -KILL "$sink_pid" >/dev/null 2>&1 || true; fi
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

media="$work/media.mp4"
"$ffmpeg" -hide_banner -loglevel error -y \
  -f lavfi -i testsrc2=size=320x180:rate=25:duration="$source_duration" \
  -f lavfi -i sine=frequency=997:sample_rate=48000:duration="$source_duration" \
  -c:v libx264 -pix_fmt yuv420p -preset veryfast -b:v 400k -g 50 -keyint_min 50 -bf 0 -sc_threshold 0 \
  -c:a aac -b:a 96k -movflags +faststart "$media"
input_program=$input_mp4
source_media=$media
if [ "$mode" = "empty-original" ]; then
  # A second H264 track whose packets are all dropped: the Matroska header
  # declares it, but it carries no frame, like a cut over a rendition whose
  # producer had stopped.
  "$ffmpeg" -hide_banner -loglevel error -y -f lavfi -i testsrc2=size=160x90:rate=25:duration=1 \
    -c:v libx264 -pix_fmt yuv420p -preset veryfast -g 25 -bf 0 "$work/short.mp4"
  source_media="$work/source.mkv"
  "$ffmpeg" -hide_banner -loglevel error -y -i "$media" -i "$work/short.mp4" \
    -map 0:v -map 0:a -map 1:v -c copy -bsf:2 noise=drop=1 "$source_media"
  declared=$("$ffprobe" -v error -select_streams v -show_entries stream=index -of csv=p=0 "$source_media" | wc -l | tr -d ' ')
  empty=$("$ffprobe" -v error -select_streams v:1 -count_packets -show_entries stream=nb_read_packets \
    -of default=nw=1:nk=1 "$source_media")
  case "$empty" in
    N/A | 0) empty_ok=1 ;;
    *) empty_ok=0 ;;
  esac
  if [ "$declared" -ne 2 ] || [ "$empty_ok" -ne 1 ]; then
    echo "fixture declares $declared video tracks with $empty packets in the second; expected 2 and none" >&2
    exit 1
  fi
  input_program=$input_ebml
fi
if [ "$mode" = "late-audio" ]; then
  media="$work/late-audio.mkv"
  "$ffmpeg" -hide_banner -loglevel error -y \
    -f lavfi -i testsrc2=size=320x180:rate=25:duration=1.5 \
    -itsoffset 0.8 -f lavfi -i sine=frequency=997:sample_rate=48000:duration=0.7 \
    -map 0:v -map 1:a -c:v libx264 -pix_fmt yuv420p -preset veryfast -g 50 -bf 0 -c:a aac -b:a 96k "$media"
  source_media=$media
  input_program=$input_ebml
fi
if [ "$mode" = "one-frame" ]; then
  media="$work/one-frame.mkv"
  "$ffmpeg" -hide_banner -loglevel error -y \
    -f lavfi -i testsrc2=size=320x180:rate=25 -f lavfi -i sine=frequency=997:sample_rate=48000 \
    -map 0:v -map 1:a -frames:v 1 -t 0.04 -c:v libx264 -pix_fmt yuv420p -preset veryfast -bf 0 \
    -c:a aac -b:a 96k "$media"
  frames=$("$ffprobe" -v error -select_streams v -count_packets -show_entries stream=nb_read_packets \
    -of default=nw=1:nk=1 "$media")
  if [ "$frames" != "1" ]; then
    echo "the one-frame fixture has $frames video frames" >&2
    exit 1
  fi
  source_media=$media
  input_program=$input_ebml
fi
source_bytes=$(wc -c <"$media" | tr -d ' ')
source_packets=$("$ffprobe" -v error -select_streams v:0 -count_packets \
  -show_entries stream=nb_read_packets -of default=nw=1:nk=1 "$media")

port=$((25000 + ($$ % 9000)))
config="$work/config.json"
printf '%s\n' \
  "{\"account\":{\"test\":{\"password\":\"098f6bcd4621d373cade4e832627b4f6\"}},\"auto_push\":null,\"bandwidth\":{\"exceptions\":[\"::1\",\"127.0.0.0/8\"]},\"config\":{\"accesslog\":\"LOG\",\"controller\":{\"interface\":\"127.0.0.1\",\"port\":$port,\"username\":null},\"debug\":4,\"defaultStream\":null,\"prometheus\":\"\",\"protocols\":[],\"serverid\":null,\"sessionInputMode\":15,\"sessionOutputMode\":15,\"sessionStreamInfoMode\":1,\"sessionUnspecifiedMode\":0,\"sessionViewerMode\":14,\"tknMode\":15,\"triggers\":{\"RECORDING_END\":[{\"handler\":\"$trigger_handler\",\"sync\":false,\"streams\":[\"$stream\"]}]},\"trustedproxy\":[]},\"extwriters\":null,\"jwks\":null,\"push_settings\":{\"maxspeed\":0,\"wait\":3},\"streamkeys\":null,\"streams\":{\"$stream\":{\"name\":\"$stream\",\"source\":\"$source_media\",\"process_controlled_realtime\":true,\"processes\":[{\"process\":\"Thumbs\",\"track_select\":\"video=maxbps\",\"inconsequential\":true,\"thumb_width\":80,\"thumb_height\":80,\"grid_cols\":3,\"grid_rows\":2,\"jpeg_quality\":80,\"interval\":2000,\"source_mask\":4,\"target_mask\":3,\"restart_type\":\"fixed\"}]}},\"variables\":null}" \
  >"$config"

TMP="$ipc_root" MIST_CONTROL=1 "$controller" -c "$config" -C r -L "$work/controller.log" &
controller_pid=$!
wait_for 30 "the test controller" logged "$work/controller.log" 'Controller started'

TMP="$ipc_root" MIST_CONTROL=1 "$input_program" -r -s "$stream" "$source_media" >"$work/input.log" 2>&1 &
input_pid=$!
wait_for 30 "the processing input" logged "$work/input.log" 'Input started'

recording="$work/recording.mkv"
throttle="$work/throttle.py"
cat >"$throttle" <<'EOF'
import sys, time
dst, rate = sys.argv[1], int(sys.argv[2])
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

start_time=$(date +%s)
case "$mode" in
  empty-original | late-audio | one-frame)
    "$timeout_program" 90 env TMP="$ipc_root" MIST_CONTROL=1 "$output_ebml" -s "$stream" "$recording" \
      >"$work/output.log" 2>&1 || true
    ;;
  slow-sink)
    sink_rate=$((source_bytes / source_duration / 2))
    "$timeout_program" 400 env TMP="$ipc_root" MIST_CONTROL=1 "$output_ebml" -s "$stream" - 2>"$work/output.log" | \
      "$python" "$throttle" "$recording" "$sink_rate" || true
    ;;
  buffer-killed)
    sink_rate=$((source_bytes / source_duration / 2))
    ( "$timeout_program" 300 env TMP="$ipc_root" MIST_CONTROL=1 "$output_ebml" -s "$stream" - 2>"$work/output.log" | \
      "$python" "$throttle" "$recording" "$sink_rate" ) &
    sink_pid=$!
    wait_for 120 "the processing source to reach EOF" exited "$input_pid"
    input_pid=
    sleep 3
    pkill -TERM -f "MistInBuffer.*$stream" || true
    wait_for 120 "the recording to stop after its buffer stopped" exited "$sink_pid"
    sink_pid=
    ;;
  stuck-sink)
    # The pipe is held open and never read, so the recorder blocks on its first full write.
    ( "$timeout_program" 400 env TMP="$ipc_root" MIST_CONTROL=1 "$output_ebml" -s "$stream" - 2>"$work/output.log" | \
      sleep 400 ) &
    sink_pid=$!
    wait_for 300 "the buffer to give up on the stuck recorder" logged "$work/input.log" 'no activity for'
    gave_up=$(($(date +%s) - start_time))
    # Unblock the recorder's write so it can exit and report.
    pkill -KILL -f "sleep 400" || true
    wait_for 60 "the stuck recorder to exit" exited "$sink_pid"
    sink_pid=
    if [ "$gave_up" -lt 60 ]; then
      echo "the buffer gave up on its recorder after ${gave_up}s, before the 60 s stale window" >&2
      exit 1
    fi
    ;;
esac
elapsed=$(($(date +%s) - start_time))

wait_for 10 "the RECORDING_END trigger" test -s "$recording_trigger_file"
reason=$(sed -n '12p' "$recording_trigger_file")
case "$mode" in
  stuck-sink | buffer-killed)
    if [ "$reason" != "SHM_LOST" ]; then
      echo "a recording its buffer ended early reported '$reason' ($(sed -n '13p' "$recording_trigger_file")); expected SHM_LOST" >&2
      exit 1
    fi
    echo "$mode: the recording reported SHM_LOST after ${elapsed}s"
    exit 0
    ;;
esac

case "$reason" in
  CLEAN*) ;;
  *)
    echo "the recording ended with '$reason' ($(sed -n '13p' "$recording_trigger_file"))" >&2
    exit 1
    ;;
esac
if [ ! -s "$recording" ]; then
  echo "the recording wrote no bytes" >&2
  exit 1
fi
if ! grep -q 'Recording header:' "$work/output.log"; then
  echo "the recording never released its header gate" >&2
  exit 1
fi
recorded=$("$ffprobe" -v error -select_streams v -show_entries packet=stream_index -of csv=p=0 "$recording" | \
  sort | uniq -c | sort -rn | awk 'NR == 1 { print $1 }')
if [ "${recorded:-0}" -ne "$source_packets" ]; then
  echo "the recording holds ${recorded:-0} video packets; the source has $source_packets" >&2
  exit 1
fi
case "$mode" in
  empty-original)
    if ! grep -q 'Not registering source track .*: it has no frames' "$work/input.log"; then
      echo "the processing input registered the empty declared track" >&2
      exit 1
    fi
    ;;
  late-audio | one-frame)
    audio_packets=$("$ffprobe" -v error -select_streams a -count_packets -show_entries stream=nb_read_packets \
      -of default=nw=1:nk=1 "$recording")
    case "$audio_packets" in '' | N/A | 0)
      echo "the recording lost the source's audio track" >&2
      exit 1
      ;;
    esac
    ;;
esac
echo "$mode: the recording holds all $source_packets video packets after ${elapsed}s"
