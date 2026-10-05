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
stub2_pid=
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
  for pid in $stub_pid $stub2_pid; do
    kill -TERM "$pid" >/dev/null 2>&1 || true
    wait "$pid" >/dev/null 2>&1 || true
  done
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
broadcasters="\"http://127.0.0.1:$broadcaster_port\""
stub_logs="$work/broadcaster.log"
if [ "${LIVEPEER_TEST_FAILING_GATEWAY:-}" = "1" ]; then
  # Two gateways; whichever receives the first upload rejects every segment
  # after a second, so both upload threads fail on it at overlapping times.
  LIVEPEER_STUB_CLAIM_FILE="$work/failing-gateway"
  LIVEPEER_STUB_REJECT_DELAY_MS=1000
  export LIVEPEER_STUB_CLAIM_FILE LIVEPEER_STUB_REJECT_DELAY_MS
  broadcaster2_port=$((broadcaster_port + 1))
  "$broadcaster_stub" "$broadcaster2_port" >"$work/broadcaster2.log" 2>&1 &
  stub2_pid=$!
  stub_logs="$stub_logs $work/broadcaster2.log"
  broadcasters="\"[\\\"http://127.0.0.1:$broadcaster_port\\\",\\\"http://127.0.0.1:$broadcaster2_port\\\"]\""
fi
triggers="{}"
livepeer_options=
if [ "${LIVEPEER_TEST_HEADER_BUFFER_KILLED:-}" = "1" ]; then
  # Every transcode is held back (within a long segment budget), so the recording waits for its
  # header until the buffer dies.
  livepeer_options=",\"deadline_ms\":120000"
  : >"$work/hold"
  LIVEPEER_STUB_HOLD_FILE="$work/hold"
  export LIVEPEER_STUB_HOLD_FILE
  MIST_TEST_TRIGGER_OUTPUT="$work/trigger"
  export MIST_TEST_TRIGGER_OUTPUT
  script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
  triggers="{\"RECORDING_END\":[{\"handler\":\"$script_dir/capture_trigger.sh\",\"sync\":false,\"streams\":[\"$stream\"]}]}"
fi
extra_processes=
output_timeout=90
if [ "${LIVEPEER_TEST_SLOW_REPLACE:-}" = "1" ]; then
  # While the recording waits for its header (every transcode is held back), a second process
  # fails for good and the buffer asks a PROCESS_REPLACE endpoint that takes 12 s to answer. The
  # buffer is busy that long, not stopped: the recording must keep waiting.
  if ! command -v python3 >/dev/null 2>&1; then
    echo "python3 is required for the slow PROCESS_REPLACE endpoint" >&2
    exit 77
  fi
  livepeer_options=",\"deadline_ms\":120000"
  : >"$work/hold"
  LIVEPEER_STUB_HOLD_FILE="$work/hold"
  export LIVEPEER_STUB_HOLD_FILE
  MIST_TEST_TRIGGER_OUTPUT="$work/trigger"
  export MIST_TEST_TRIGGER_OUTPUT
  script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
  replace_port=$((broadcaster_port + 2))
  dead_port=$((broadcaster_port + 3))
  # Answers after 12 s, sending a little of its (empty) answer every 3 s so the request does not
  # time out before that.
  python3 -c '
import socket, sys, time
srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("127.0.0.1", int(sys.argv[1])))
srv.listen(4)
print("ready", flush=True)
while True:
    conn, _ = srv.accept()
    request = b""
    while b"\r\n\r\n" not in request:
        request += conn.recv(65536)
    head, body = request.split(b"\r\n\r\n", 1)
    length = 0
    for line in head.split(b"\r\n"):
        if line.lower().startswith(b"content-length:"):
            length = int(line.split(b":")[1])
    while len(body) < length:
        body += conn.recv(65536)
    open(sys.argv[2], "w").write("%.3f\n" % time.time())
    chunks = 4
    conn.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: %d\r\nConnection: close\r\n\r\n" % (chunks * 26000))
    for _ in range(chunks):
        time.sleep(3)
        conn.sendall(b" " * 26000)
    conn.close()
' "$replace_port" "$work/replace-requested" >"$work/replace-endpoint.log" 2>&1 &
  stub2_pid=$!
  stub_logs="$stub_logs $work/replace-endpoint.log"
  triggers="{\"RECORDING_END\":[{\"handler\":\"$script_dir/capture_trigger.sh\",\"sync\":false,\"streams\":[\"$stream\"]}],\"PROCESS_REPLACE\":[{\"handler\":\"http://127.0.0.1:$replace_port/\",\"sync\":true,\"streams\":[\"$stream\"]}]}"
  # Uploads to a gateway that is not there: the process exits unrecoverably on its first segment.
  extra_processes=",{\"process\":\"Livepeer\",\"hardcoded_broadcasters\":\"http://127.0.0.1:$dead_port\",\"target_profiles\":[{\"name\":\"spare\",\"bitrate\":200000,\"width\":160,\"height\":90,\"fps\":10,\"gop\":\"2.0\"}],\"target_mask\":2,\"source_mask\":4,\"restart_type\":\"disabled\"}"
  output_timeout=90
fi
"$broadcaster_stub" "$broadcaster_port" >"$work/broadcaster.log" 2>&1 &
stub_pid=$!
for log in $stub_logs; do
  attempt=0
  while [ "$attempt" -lt 50 ] && ! grep -q '^ready$' "$log" 2>/dev/null; do
    attempt=$((attempt + 1))
    sleep 0.1
  done
  if ! grep -q '^ready$' "$log"; then
    echo "loopback Livepeer broadcaster did not become ready ($log)" >&2
    exit 1
  fi
done

restart_type=disabled
if [ "${LIVEPEER_TEST_KILL_MIDWAY:-}" = "1" ]; then restart_type=fixed; fi
config="$work/config.json"
printf '%s\n' \
  "{\"account\":{\"test\":{\"password\":\"098f6bcd4621d373cade4e832627b4f6\"}},\"auto_push\":null,\"bandwidth\":{\"exceptions\":[\"::1\",\"127.0.0.0/8\"]},\"config\":{\"accesslog\":\"LOG\",\"controller\":{\"interface\":\"127.0.0.1\",\"port\":$controller_port,\"username\":null},\"debug\":4,\"defaultStream\":null,\"prometheus\":\"\",\"protocols\":[],\"serverid\":null,\"sessionInputMode\":15,\"sessionOutputMode\":15,\"sessionStreamInfoMode\":1,\"sessionUnspecifiedMode\":0,\"sessionViewerMode\":14,\"tknMode\":15,\"triggers\":$triggers,\"trustedproxy\":[]},\"extwriters\":null,\"jwks\":null,\"push_settings\":{\"maxspeed\":0,\"wait\":3},\"streamkeys\":null,\"streams\":{\"$stream\":{\"name\":\"$stream\",\"source\":\"$source_mkv\",\"process_controlled_realtime\":true,\"realtime_speed\":4,\"processes\":[{\"process\":\"Livepeer\",\"hardcoded_broadcasters\":$broadcasters,\"target_profiles\":[{\"name\":\"audit\",\"bitrate\":500000,\"width\":320,\"height\":180,\"fps\":10,\"gop\":\"2.0\"}],\"target_mask\":2,\"source_mask\":4,\"restart_type\":\"$restart_type\"$livepeer_options}$extra_processes]}},\"variables\":null}" \
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
"$timeout_program" "$output_timeout" env TMP="$ipc_root" MIST_CONTROL=1 "$output_ebml" -s "$stream" "$recording?stop=29500" \
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
# Without a single transcoded segment (header-buffer-killed, slow-replace) the snapshot is not required.
if [ "$proc_state_read" -ne 1 ] && [ "${LIVEPEER_TEST_HEADER_BUFFER_KILLED:-}" != "1" ] &&
   [ "${LIVEPEER_TEST_SLOW_REPLACE:-}" != "1" ]; then
  echo "Livepeer did not publish a readable ProcState snapshot while running" >&2
  exit 1
fi
if [ "${LIVEPEER_TEST_HEADER_BUFFER_KILLED:-}" = "1" ]; then
  # The recording waits for the rendition before writing its header; the buffer dies under it.
  attempt=0
  while [ "$attempt" -lt 300 ] && ! grep -q 'Waiting for processing tracks before recording header' "$work/output.log"; do
    attempt=$((attempt + 1))
    sleep 0.1
  done
  if ! grep -q 'Waiting for processing tracks before recording header' "$work/output.log"; then
    echo "the recording never waited for its header" >&2
    exit 1
  fi
  pkill -KILL -f "MistInBuffer.*$stream" || true
  attempt=0
  while [ "$attempt" -lt 600 ] && kill -0 "$output_pid" 2>/dev/null; do
    attempt=$((attempt + 1))
    sleep 0.1
  done
  if kill -0 "$output_pid" 2>/dev/null; then
    echo "the recording was still waiting for its header 60 s after its buffer was killed" >&2
    exit 1
  fi
  wait "$output_pid" || true
  output_pid=
  attempt=0
  while [ "$attempt" -lt 100 ] && [ ! -s "$work/trigger.RECORDING_END" ]; do
    attempt=$((attempt + 1))
    sleep 0.1
  done
  reason=$(sed -n '12p' "$work/trigger.RECORDING_END" 2>/dev/null || true)
  if [ "$reason" != "SHM_LOST" ]; then
    echo "a recording whose buffer was killed before its header reported '$reason'; expected SHM_LOST" >&2
    exit 1
  fi
  rm -f "$work/hold"
  echo "a recording waiting for its header reported SHM_LOST when its buffer was killed"
  exit 0
fi
if [ "${LIVEPEER_TEST_SLOW_REPLACE:-}" = "1" ]; then
  attempt=0
  while [ "$attempt" -lt 300 ] && [ ! -s "$work/replace-requested" ]; do
    attempt=$((attempt + 1))
    sleep 0.1
  done
  if [ ! -s "$work/replace-requested" ]; then
    echo "the failing process never made the buffer ask for its replacement" >&2
    exit 1
  fi
  # The buffer waits for the endpoint 12 s; the recording must still wait for its header after that.
  sleep 14
  if ! kill -0 "$output_pid" 2>/dev/null || grep -q 'SHM_LOST\|buffer stopped' "$work/output.log"; then
    echo "the recording gave up while its buffer was busy asking for a replacement" >&2
    exit 1
  fi
  if ! grep -q 'Waiting for processing tracks before recording header' "$work/output.log"; then
    echo "the recording never waited for its header" >&2
    exit 1
  fi
  rm -f "$work/hold"
  attempt=0
  while [ "$attempt" -lt 1000 ] && kill -0 "$output_pid" 2>/dev/null; do
    attempt=$((attempt + 1))
    sleep 0.1
  done
  wait "$output_pid" || true
  output_pid=
  attempt=0
  while [ "$attempt" -lt 100 ] && [ ! -s "$work/trigger.RECORDING_END" ]; do
    attempt=$((attempt + 1))
    sleep 0.1
  done
  reason=$(sed -n '12p' "$work/trigger.RECORDING_END" 2>/dev/null || true)
  if [ -z "$reason" ] || [ "$reason" = "SHM_LOST" ]; then
    echo "a recording whose buffer was busy for 12 s ended with '$reason'" >&2
    exit 1
  fi
  echo "a recording waiting for its header kept waiting through a 12 s PROCESS_REPLACE call and ended with $reason"
  exit 0
fi
if [ "${LIVEPEER_TEST_KILL_MIDWAY:-}" = "1" ]; then
  # Kill the process once about a third of the source is transcoded; its restart continues the
  # rendition after what was produced instead of transcoding the source again from the start.
  attempt=0
  while [ "$attempt" -lt 300 ] && [ "$(cat $stub_logs | grep -c '^responded ')" -lt 5 ]; do
    attempt=$((attempt + 1))
    sleep 0.1
  done
  kill -KILL "$livepeer_pid"
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
first_response=$(cat $stub_logs | grep '^responded ' | head -1 | cut -d' ' -f2)
if [ "$first_response" != "1" ]; then
  echo "broadcaster did not complete segment 1 before segment 0; ordering path was not exercised" >&2
  exit 1
fi
# Live uploads carry a budget of their segment duration (2 s here, shorter for
# the tail) plus one second.
deadlines=$(cat $stub_logs | sed -n 's/^deadline [0-9]* //p')
if [ -z "$deadlines" ]; then
  echo "broadcaster saw no uploads" >&2
  exit 1
fi
for deadline in $deadlines; do
  if [ "$deadline" -le 1000 ] || [ "$deadline" -gt 3500 ]; then
    echo "an upload carried deadlineMs=$deadline; expected its segment duration plus 1000 ms" >&2
    exit 1
  fi
done
if [ "${LIVEPEER_STUB_REJECT_FIRST:-}" = "1" ]; then
  # Every segment was rejected once; each must be re-sent to the same
  # broadcaster and transcoded rather than skipped.
  rejected=$(grep -c '^rejected ' "$work/broadcaster.log" || true)
  if [ "$rejected" -lt 10 ]; then
    echo "broadcaster rejected only $rejected segments; the 422 path was not exercised" >&2
    exit 1
  fi
  if ! grep -q 'Re-sending rejected seg' "$work/input.log"; then
    echo "Livepeer did not re-send a rejected segment to the same broadcaster" >&2
    exit 1
  fi
  if grep -q 'Segment could not be transcoded\|consecutive segment rejections\|Livepeer rejected segment' "$work/input.log"; then
    echo "Livepeer gave up a segment the broadcaster accepts on re-send" >&2
    exit 1
  fi
fi
if [ "${LIVEPEER_STUB_NO_RESULT_FIRST:-}" = "1" ]; then
  # Every segment first got a 503 (no result yet); each must be re-posted to
  # the same gateway within its budget instead of stopping Livepeer.
  no_result=$(grep -c '^rejected ' "$work/broadcaster.log" || true)
  if [ "$no_result" -lt 10 ]; then
    echo "broadcaster answered 503 for only $no_result segments; the no-result path was not exercised" >&2
    exit 1
  fi
  if ! grep -q 'No result for seg' "$work/input.log"; then
    echo "Livepeer did not re-post a segment the gateway had no result for" >&2
    exit 1
  fi
  if grep -q 'fatal HTTP status\|had no result for segment\|Switched to new broadcaster' "$work/input.log"; then
    echo "Livepeer stopped or switched on a 503 within the segment's budget" >&2
    exit 1
  fi
fi
if [ "${LIVEPEER_TEST_FAILING_GATEWAY:-}" = "1" ]; then
  # Both upload threads fail on the first gateway; one switch moves the stream
  # to the other and the second thread follows it instead of switching back.
  rejected=$(cat $stub_logs | grep -c '^rejected ' || true)
  if [ "$rejected" -lt 6 ]; then
    echo "the failing gateway rejected only $rejected uploads; both upload threads did not fail on it" >&2
    exit 1
  fi
  switches=$(grep -c 'Switched to new broadcaster' "$work/input.log" || true)
  if [ "$switches" -ne 1 ]; then
    echo "Livepeer switched broadcasters $switches times; expected exactly once" >&2
    exit 1
  fi
  if ! grep -q 'follows the concurrent switch' "$work/input.log"; then
    echo "the second upload thread did not reach its switch after the first one switched; the race was not exercised" >&2
    exit 1
  fi
  if grep -q 'Segment could not be transcoded\|consecutive segment rejections\|Livepeer rejected segment' "$work/input.log"; then
    echo "Livepeer gave up a segment the other gateway transcodes" >&2
    exit 1
  fi
fi
reserved=$(sed -n 's/.*Reserved track \([0-9][0-9]*\) for output .*\/audit.*/\1/p' "$work/input.log" | head -n 1)
if [ -z "$reserved" ] || ! grep -q "Claimed reserved track $reserved (output .*/audit)" "$work/input.log"; then
  echo "the Livepeer rendition did not take the track the buffer reserved for it (${reserved:-none})" >&2
  exit 1
fi
if [ "${LIVEPEER_TEST_KILL_MIDWAY:-}" = "1" ]; then
  if ! grep -q 'Resuming track [0-9]* (output ' "$work/input.log"; then
    echo "the restarted Livepeer process did not continue its rendition track" >&2
    exit 1
  fi
  if ! grep -q 'Continuing renditions that a previous run produced' "$work/input.log"; then
    echo "the restarted Livepeer process did not skip what the killed run produced" >&2
    exit 1
  fi
  uploads=$(cat $stub_logs | grep -c '^deadline ' || true)
  if [ "$uploads" -gt 19 ]; then
    echo "Livepeer uploaded $uploads segments of a 30 s source; the restart transcoded the start again" >&2
    exit 1
  fi
fi
echo "Livepeer loopback recording retained the processed video tail through $video_tail seconds"
