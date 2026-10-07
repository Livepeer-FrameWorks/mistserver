#!/bin/sh
set -eu

# Processing streams whose processes are decided by the processing graph:
#   av-latency  an AV-only stream: the AV process, which reads a single track, starts reading once
#               that track is ready to play, not after the timeout of waiting for a second one
#   slow-onnx   AV -> ONNX where the ONNX process takes 4s to start and the recording drains
#               into a slow reader: the recording still holds the complete source and ONNX
#               results from its start through its end
#   kill-onnx   AV -> ONNX where the ONNX process is killed mid-stream: the feed never pauses,
#               the restarted process continues its results track by output key, and the
#               recording has results through its end

if [ "$#" -ne 14 ]; then
  echo "usage: $0 mode ffmpeg ffprobe MistController MistInEBML MistInBuffer MistProcAV MistProcONNX MistOutEBML MistSession MistUtilNuke MistAnalyserEBML perl streamfeedprobe" >&2
  exit 2
fi

mode=$1
ffmpeg=$2
ffprobe=$3
controller=$4
input_ebml=$5
input_buffer=$6
process_av=$7
process_onnx=$8
output_ebml=$9
session=${10}
util_nuke=${11}
analyser_ebml=${12}
perl_program=${13}
feed_probe=${14}

if [ "${MIST_RUN_MEDIA_TESTS:-}" != "1" ]; then
  echo "set MIST_RUN_MEDIA_TESTS=1 to run the processing graph integration" >&2
  exit 77
fi
if [ "$mode" != av-latency ] && { [ -z "${MIST_ONNX_TEST_MODEL:-}" ] || [ ! -f "$MIST_ONNX_TEST_MODEL" ]; }; then
  echo "set MIST_ONNX_TEST_MODEL to a local yolo26n ONNX model" >&2
  exit 77
fi
for program in "$ffmpeg" "$ffprobe" "$controller" "$input_ebml" "$input_buffer" "$process_av" "$process_onnx" \
  "$output_ebml" "$session" "$util_nuke" "$analyser_ebml" "$perl_program" "$feed_probe"; do
  if [ ! -x "$program" ]; then
    echo "required executable is unavailable: $program" >&2
    exit 77
  fi
done
if ! "$ffmpeg" -hide_banner -encoders 2>/dev/null | grep -q 'libx264'; then
  echo "ffmpeg lacks the libx264 encoder required for the fixture" >&2
  exit 77
fi

work=$(mktemp -d "${TMPDIR:-/tmp}/mist-processing-graph.XXXXXX")
ipc_root="$work/ipc"
bin="$work/bin"
mkdir -p "$ipc_root" "$bin"
stream="pgraph$$"
controller_pid=
input_pid=
stamp_pid=

cleanup() {
  status=$?
  trap - EXIT HUP INT TERM
  if [ "$status" -ne 0 ] && [ "$status" -ne 77 ]; then
    echo "processing graph integration ($mode) failed; logs follow:" >&2
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
  if [ -n "$stamp_pid" ]; then
    wait "$stamp_pid" >/dev/null 2>&1 || true
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

# Prefixes every log line with the time it was written.
stamp() {
  "$perl_program" -MTime::HiRes=time -ne 'BEGIN { $| = 1 } printf "%.3f %s", time, $_'
}
first_time() {
  awk -v pattern="$2" 'index($0, pattern) { print $1; exit }' "$1"
}
seconds_between() {
  awk -v a="$1" -v b="$2" 'BEGIN { printf "%.3f", b - a }'
}
exceeds() {
  awk -v value="$1" -v limit="$2" 'BEGIN { exit !(value > limit) }'
}

# The buffer starts processes next to itself, so the slow ONNX process is a wrapper in a copy of
# the binaries that starts the real one late. Describing its outputs is not delayed.
for program in "$controller" "$input_ebml" "$input_buffer" "$process_av" "$output_ebml" "$session" "$util_nuke"; do
  cp "$program" "$bin/"
done
if [ "$mode" = slow-onnx ]; then
  cp "$process_onnx" "$bin/MistProcONNX.real"
  printf '%s\n' '#!/bin/sh' \
    'if [ "$1" != --describe-outputs ]; then sleep 4; fi' \
    'exec "$(dirname "$0")/MistProcONNX.real" "$@"' >"$bin/MistProcONNX"
  chmod +x "$bin/MistProcONNX"
else
  cp "$process_onnx" "$bin/MistProcONNX"
fi

duration=20
recording_seconds=10
source_mkv="$work/source.mkv"
"$ffmpeg" -hide_banner -loglevel error -y \
  -f lavfi -i testsrc2=size=320x180:rate=10:duration=$duration \
  -f lavfi -i sine=frequency=997:sample_rate=48000:duration=$duration \
  -c:v libx264 -pix_fmt yuv420p -preset veryfast -g 20 -keyint_min 20 -bf 0 -sc_threshold 0 \
  -c:a aac -b:a 96k "$source_mkv"

av='{"process":"AV","x-LSP-kind":"video","codec":"NV12","track_select":"video=H264&audio=none","target_mask":4}'
if [ "$mode" = av-latency ]; then
  # Logs the readiness wait, so a start on its timeout shows.
  av='{"process":"AV","x-LSP-kind":"video","codec":"NV12","track_select":"video=H264&audio=none","target_mask":4,"debug":6}'
fi
onnx="{\"process\":\"ONNX\",\"model\":\"custom\",\"model_path\":\"$MIST_ONNX_TEST_MODEL\",\"model_type\":\"yolo-nms\",\"input_size\":640,\"process_every_nth\":2,\"track_select\":\"video=NV12&audio=none\",\"target_mask\":2}"
if [ "$mode" = av-latency ]; then
  processes="[$av]"
else
  processes="[$av,$onnx]"
fi

port=$((27000 + ($$ % 10000)))
printf '%s\n' \
  "{\"account\":{\"test\":{\"password\":\"098f6bcd4621d373cade4e832627b4f6\"}},\"auto_push\":null,\"bandwidth\":{\"exceptions\":[\"::1\",\"127.0.0.0/8\"]},\"config\":{\"accesslog\":\"LOG\",\"controller\":{\"interface\":\"127.0.0.1\",\"port\":$port,\"username\":null},\"debug\":4,\"defaultStream\":null,\"prometheus\":\"\",\"protocols\":[],\"serverid\":null,\"sessionInputMode\":15,\"sessionOutputMode\":15,\"sessionStreamInfoMode\":1,\"sessionUnspecifiedMode\":0,\"sessionViewerMode\":14,\"tknMode\":15,\"triggers\":{},\"trustedproxy\":[]},\"extwriters\":null,\"jwks\":null,\"push_settings\":{\"maxspeed\":0,\"wait\":3},\"streamkeys\":null,\"streams\":{\"$stream\":{\"name\":\"$stream\",\"source\":\"$source_mkv\",\"process_controlled_realtime\":true,\"realtime_speed\":1,\"processes\":$processes}},\"variables\":null}" \
  >"$work/config.json"

TMP="$ipc_root" MIST_CONTROL=1 "$bin/MistController" -c "$work/config.json" -C r -L "$work/controller.log" &
controller_pid=$!
attempt=0
until grep -q 'Controller started' "$work/controller.log" 2>/dev/null; do
  attempt=$((attempt + 1))
  if [ "$attempt" -gt 300 ] || ! kill -0 "$controller_pid" 2>/dev/null; then
    echo "test controller did not become ready" >&2
    exit 1
  fi
  sleep 0.1
done

mkfifo "$work/input.fifo"
stamp <"$work/input.fifo" >"$work/input.log" &
stamp_pid=$!
TMP="$ipc_root" MIST_CONTROL=1 "$bin/MistInEBML" -r -s "$stream" "$source_mkv" >"$work/input.fifo" 2>&1 &
input_pid=$!
attempt=0
until grep -q 'MistInBuffer.*Input started' "$work/input.log" 2>/dev/null; do
  attempt=$((attempt + 1))
  if [ "$attempt" -gt 300 ] || ! kill -0 "$input_pid" 2>/dev/null; then
    echo "the stream buffer did not start" >&2
    exit 1
  fi
  sleep 0.1
done
input_started=$(first_time "$work/input.log" 'Input started')

if [ "$mode" = av-latency ]; then
  attempt=0
  until grep -q 'MistProcAV.*starting at buffer head' "$work/input.log" 2>/dev/null; do
    attempt=$((attempt + 1))
    if [ "$attempt" -gt 100 ]; then
      echo "the AV process did not start reading within 10s" >&2
      exit 1
    fi
    sleep 0.1
  done
  av_reading=$(awk 'index($0, "MistProcAV") && index($0, "starting at buffer head") { print $1; exit }' "$work/input.log")
  latency=$(seconds_between "$input_started" "$av_reading")
  echo "AV started reading ${latency}s after the input"
  if grep -q 'isReadyForPlay timed out waiting for tracks' "$work/input.log"; then
    echo "AV started reading on the readiness timeout instead of on its one track being ready" >&2
    exit 1
  fi
  # A live reader is ready once a track it selects holds two keyframes or more than 500ms of
  # media, checked every 500ms. The fixture's source arrives in real time with a keyframe every
  # 2s, so its one video track is ready at most one GOP plus one re-check after the input started.
  if exceeds "$latency" 2.5; then
    echo "AV started reading ${latency}s after the input; its track is ready within one GOP (2s) plus one 500ms re-check" >&2
    exit 1
  fi
  exit 0
fi

recording="$work/recording.mkv"
if [ "$mode" = slow-onnx ]; then
  # The recording drains into a reader that takes at most 64 KiB per 100 ms.
  mkfifo "$work/recording.fifo"
  "$perl_program" -e 'binmode STDIN; binmode STDOUT; $| = 1;
    while (read(STDIN, my $chunk, 65536)) { print $chunk; select(undef, undef, undef, 0.1) }' \
    <"$work/recording.fifo" >"$recording" &
  drain_pid=$!
  target="$work/recording.fifo"
else
  target="$recording"
fi
(
  TMP="$ipc_root" MIST_CONTROL=1 "$bin/MistOutEBML" -s "$stream" "$target?duration=$recording_seconds" 2>&1
  echo "recorder exited with status $?"
) | stamp >"$work/output.log" &
recorder_pid=$!

if [ "$mode" = kill-onnx ]; then
  attempt=0
  until grep -q 'Recording header:' "$work/output.log" 2>/dev/null; do
    attempt=$((attempt + 1))
    if [ "$attempt" -gt 300 ]; then
      echo "the recording header was never written" >&2
      exit 1
    fi
    sleep 0.1
  done
  sleep 3
  onnx_pid=$(sed -n 's/.*Started process \([0-9][0-9]*\): .*MistProcONNX.*/\1/p' "$work/input.log" | tail -1)
  if [ -z "$onnx_pid" ]; then
    echo "could not identify the ONNX process" >&2
    exit 1
  fi
  TMP="$ipc_root" "$feed_probe" "$stream" 4000 >"$work/feed-paused" 2>"$work/feed-probe.log" &
  probe_pid=$!
  sleep 0.2
  killed_at=$("$perl_program" -MTime::HiRes=time -e 'printf "%.3f", time')
  kill -KILL "$onnx_pid"
  wait "$probe_pid"
  paused_ms=$(cat "$work/feed-paused")
  echo "the feed was paused for ${paused_ms}ms of the 4s around the ONNX kill"
  if [ "$paused_ms" -ne 0 ]; then
    echo "the feed paused for ${paused_ms}ms while the killed ONNX process restarted" >&2
    exit 1
  fi
fi

wait "$recorder_pid" || true
if [ "$mode" = slow-onnx ]; then
  wait "$drain_pid" || true
fi
if ! grep -q 'recorder exited with status 0' "$work/output.log"; then
  echo "the recorder failed" >&2
  exit 1
fi

if grep -q 'which this process did not declare' "$work/input.log"; then
  echo "a process registered an output it did not declare with --describe-outputs" >&2
  exit 1
fi
# The ONNX process ends on its own once its source ended, and reports that as a clean exit.
if [ "$mode" != av-latency ]; then
  attempt=0
  while [ "$attempt" -lt 100 ] && ! grep -q 'MistProcONNX.*Stop sink thread' "$work/input.log"; do
    attempt=$((attempt + 1))
    sleep 0.1
  done
  if ! grep -q 'MistProcONNX.*Stop sink thread' "$work/input.log"; then
    echo "the ONNX process did not end after its source ended" >&2
    exit 1
  fi
  if grep 'MistProcONNX' "$work/input.log" | grep -q 'Logging unclean exit reason'; then
    echo "the ONNX process reported an unclean exit after its source ended:" >&2
    grep 'MistProcONNX.*Logging unclean exit reason' "$work/input.log" >&2
    exit 1
  fi
fi
"$ffmpeg" -hide_banner -loglevel error -i "$recording" -map 0:v:0 -map 0:a:0 -f null - 2>"$work/decode.log"
if [ -s "$work/decode.log" ]; then
  echo "the recording emitted decoder/demuxer diagnostics" >&2
  exit 1
fi
first_video=$("$ffprobe" -v error -select_streams v:0 -show_entries packet=pts_time -of csv=p=0 "$recording" | head -1)
last_video=$("$ffprobe" -v error -select_streams v:0 -show_entries packet=pts_time -of csv=p=0 "$recording" | tail -1)
last_audio=$("$ffprobe" -v error -select_streams a:0 -show_entries packet=pts_time -of csv=p=0 "$recording" | tail -1)
if exceeds "$first_video" 0.05 || ! exceeds "$last_video" "$((recording_seconds - 1)).75" ||
   ! exceeds "$last_audio" "$((recording_seconds - 1)).75"; then
  echo "the recording does not hold the complete source: video $first_video-$last_video, audio until $last_audio" >&2
  exit 1
fi

"$analyser_ebml" -D 2 "$recording" >"$work/analyser.log" 2>&1
json_tracks=$(grep -c 'CodecID.*M_JSON' "$work/analyser.log" || true)
if [ "$json_tracks" -ne 1 ]; then
  echo "the recording has $json_tracks ONNX results tracks; expected exactly one" >&2
  exit 1
fi
results=$(awk '
  /TrackNumber/ { number = $NF }
  /CodecID.*M_JSON/ { print number; exit }
' "$work/analyser.log")
awk -v track="$results" '
  /\[Timecode\] =/ { cluster = $NF }
  $0 ~ ("SimpleBlock.*track " track " @") {
    for (i = 1; i <= NF; ++i) { if ($i == "@") { print (cluster + $(i + 1)) / 1000 } }
  }
' "$work/analyser.log" >"$work/results.times"
result_count=$(wc -l <"$work/results.times" | tr -d ' ')
first_result=$(sort -n "$work/results.times" | head -1)
last_result=$(sort -n "$work/results.times" | tail -1)
echo "$mode: $result_count ONNX results from ${first_result}s to ${last_result}s"
if [ "$result_count" -lt 10 ] || ! exceeds "$last_result" "$((recording_seconds - 1)).5"; then
  echo "ONNX results do not cover the end of the recording: $result_count results from $first_result to $last_result" >&2
  exit 1
fi
# Every second frame is processed (process_every_nth=2 at 10fps), starting with the first: the
# results start with the recording's first frame, however late the ONNX process started reading.
if exceeds "$first_result" 0.05; then
  echo "ONNX results do not cover the start of the recording: the first is at ${first_result}s" >&2
  exit 1
fi
# The AV process hands ONNX every frame it decoded, the first one included, with its picture.
if grep -q 'ProcessSource got video packet with no data' "$work/input.log"; then
  echo "the AV process published an empty NV12 frame" >&2
  exit 1
fi

if grep -q 'Processing feed paused' "$work/input.log"; then
  if [ "$mode" = kill-onnx ]; then
    echo "the feed paused although only a process was restarting" >&2
    exit 1
  fi
fi

if [ "$mode" = kill-onnx ]; then
  starts=$(grep -c 'Started process .*MistProcONNX' "$work/input.log" || true)
  if [ "$starts" -lt 2 ]; then
    echo "the killed ONNX process was not restarted" >&2
    exit 1
  fi
  if ! grep -q 'MistProcONNX.*Resuming track .*(output .*/default/results)' "$work/input.log"; then
    echo "the restarted ONNX process did not continue its results track by output key" >&2
    exit 1
  fi
  restarted=$(awk 'index($0, "Started process") && index($0, "MistProcONNX") { t = $1 } END { print t }' "$work/input.log")
  echo "ONNX restarted $(seconds_between "$killed_at" "$restarted")s after it was killed"
fi
echo "processing graph $mode passed"
