#!/bin/sh
# Livepeer on a live RTMP stream, against the loopback broadcaster stub.
# MIST_LIVEPEER_LIVE_MODE selects the case:
#   backpressure  the gateway stalls every response for 40 s, so the source
#                 waits on full upload slots for well over 30 s; the process
#                 keeps running (its session is not ended under it) and the
#                 rendition continues afterwards.
#   stop-stall    the process is stopped (SIGTERM) while the gateway stalls its
#                 uploads; it exits within seconds instead of waiting for the
#                 stalled responses.
#   restart       once the stream is older than 30 s (when the buffer checks
#                 its configuration only every 5 s), the process is killed
#                 three times; the buffer restarts it within 1.5 s every time.
#   kill-resume   the process is killed three times; each restart continues
#                 the same rendition track (same index, no LIVE_TRACK_LIST
#                 change), and an MKV viewer reading through the kills gets
#                 the rendition with gaps.
#   stall-kill    the gateway stalls for 48 s, then the stalled process is
#                 killed; the rendition track outlives the idle timeout and the
#                 restarted process continues it.
#   resolution    the process is killed and its restart gets renditions in
#                 another resolution; the restarted process replaces the
#                 rendition track explicitly and the buffer removes the old one
#                 at once.
#   live-end      the publisher leaves a non-resumable stream; the process plays
#                 out the buffer and exits, the buffer does not restart it and
#                 stops right after it (no 30 s inactivity wait, no SIGKILL).
#   live-end-resume
#                 the publisher leaves a resumable stream and the process is
#                 killed; the buffer does not restart it while the publisher is
#                 away, and a returning publisher gets its process and
#                 renditions back.
#   stop-sessions the sessions of a resumable stream are stopped through the API
#                 while the publisher is live (as at a platform stream stop);
#                 the process exits by itself within seconds, and the buffer
#                 stops after its inactivity timeout without having to SIGKILL
#                 it.
set -eu

if [ "$#" -ne 9 ]; then
  echo "usage: $0 ffmpeg ffprobe MistController MistInBuffer MistOutRTMP MistProcLivepeer MistOutEBML MistUtilNuke broadcaster-stub" >&2
  exit 2
fi
if [ "${MIST_RUN_MEDIA_TESTS:-}" != "1" ]; then
  echo "set MIST_RUN_MEDIA_TESTS=1 to run the live Livepeer pipeline" >&2
  exit 77
fi

ffmpeg=$1
ffprobe=$2
controller=$3
input_buffer=$4
output_rtmp=$5
process_livepeer=$6
output_ebml=$7
util_nuke=$8
broadcaster_stub=$9
for program in "$ffmpeg" "$ffprobe" "$controller" "$input_buffer" "$output_rtmp" "$process_livepeer" "$output_ebml" \
  "$util_nuke" "$broadcaster_stub"; do
  if [ ! -x "$program" ]; then
    echo "required executable is unavailable: $program" >&2
    exit 77
  fi
done
if ! "$ffmpeg" -hide_banner -encoders 2>/dev/null | grep -q 'libx264'; then
  echo "ffmpeg lacks the libx264 encoder required for the publisher fixture" >&2
  exit 77
fi

mode=${MIST_LIVEPEER_LIVE_MODE:-backpressure}
case "$mode" in
  backpressure | stop-stall | restart | kill-resume | stall-kill | resolution | live-end | live-end-resume) ;;
  stop-sessions)
    if ! command -v curl >/dev/null 2>&1; then
      echo "curl is required to stop the stream's sessions through the API" >&2
      exit 77
    fi
    ;;
  *)
    echo "unknown MIST_LIVEPEER_LIVE_MODE $mode" >&2
    exit 2
    ;;
esac

work=$(mktemp -d "${TMPDIR:-/tmp}/mist-livepeer-live.XXXXXX")
ipc_root="$work/ipc"
mkdir -p "$ipc_root"
stream="lplive$$"
controller_pid=
publisher_pid=
stub_pid=
viewer_pid=

cleanup() {
  status=$?
  trap - EXIT HUP INT TERM
  if [ "$status" -ne 0 ]; then
    echo "live Livepeer integration ($mode) failed; logs follow:" >&2
    for log in "$work"/*.log; do
      if [ -f "$log" ]; then
        echo "Log: $log" >&2
        grep -E 'Livepeer|track|Track|signal|process|Process|held|responded' "$log" | \
          grep -vE 'Type mismatch|Codec mismatch|bootmsoffset' | tail -80 >&2 || true
      fi
    done
    tail -n 3 "$work/track-lists" >&2 || true
  fi
  for pid in $viewer_pid $publisher_pid; do
    kill -TERM "$pid" >/dev/null 2>&1 || true
    wait "$pid" >/dev/null 2>&1 || true
  done
  if [ -n "$controller_pid" ]; then
    TMP="$ipc_root" MIST_CONTROL=1 "$util_nuke" "$stream" >/dev/null 2>&1 || true
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
    sleep 0.1
  done
}
logged() { grep -qE -- "$2" "$1" 2>/dev/null; }

hold_file="$work/hold"
alt_flag="$work/alternate"
alt_segment="$work/alternate.ts"
"$ffmpeg" -hide_banner -loglevel error -y -f lavfi -i testsrc2=size=160x90:rate=25:duration=2 \
  -c:v libx264 -pix_fmt yuv420p -preset ultrafast -g 50 -keyint_min 50 -bf 0 -f mpegts "$alt_segment"
api_port=$((24000 + ($$ % 9000)))
rtmp_port=$((api_port + 1))
broadcaster_port=$((api_port + 2))
LIVEPEER_STUB_HOLD_FILE="$hold_file" LIVEPEER_STUB_ALT_FLAG="$alt_flag" LIVEPEER_STUB_ALT_FILE="$alt_segment" \
  "$broadcaster_stub" "$broadcaster_port" >"$work/broadcaster.log" 2>&1 &
stub_pid=$!
wait_for 10 "the loopback broadcaster" logged "$work/broadcaster.log" '^ready$'

track_lists="$work/track-lists"
handler="$work/track-list-handler.sh"
cat >"$handler" <<EOF
#!/bin/sh
printf 'LIST %s\n' "\$(tr '\n' ' ')" >>"$track_lists"
EOF
chmod +x "$handler"
: >"$track_lists"

config="$work/config.json"
resume_setting=
if [ "$mode" = live-end-resume ]; then resume_setting=',"resume":1'; fi
# As configured on the platform: a resumable stream that ends 12 s after its last activity.
if [ "$mode" = stop-sessions ]; then resume_setting=',"resume":1,"inputtimeout":12'; fi
printf '%s\n' \
  "{\"account\":{\"test\":{\"password\":\"098f6bcd4621d373cade4e832627b4f6\"}},\"auto_push\":null,\"bandwidth\":{\"exceptions\":[\"::1\",\"127.0.0.0/8\"]},\"config\":{\"accesslog\":\"LOG\",\"controller\":{\"interface\":\"127.0.0.1\",\"port\":$api_port,\"username\":null},\"debug\":4,\"defaultStream\":null,\"prometheus\":\"\",\"protocols\":[{\"connector\":\"RTMP\",\"interface\":\"127.0.0.1\",\"port\":$rtmp_port}],\"serverid\":null,\"sessionInputMode\":15,\"sessionOutputMode\":15,\"sessionStreamInfoMode\":1,\"sessionUnspecifiedMode\":0,\"sessionViewerMode\":14,\"tknMode\":15,\"triggers\":{\"LIVE_TRACK_LIST\":[{\"handler\":\"$handler\",\"sync\":false,\"streams\":[\"$stream\"]}]},\"trustedproxy\":[]},\"extwriters\":null,\"jwks\":null,\"push_settings\":{\"maxspeed\":0,\"wait\":3},\"streamkeys\":null,\"streams\":{\"$stream\":{\"name\":\"$stream\",\"source\":\"push://\"$resume_setting,\"processes\":[{\"process\":\"Livepeer\",\"hardcoded_broadcasters\":\"http://127.0.0.1:$broadcaster_port\",\"target_profiles\":[{\"name\":\"audit\",\"bitrate\":400000,\"width\":320,\"height\":180,\"fps\":25,\"gop\":\"2.0\"}],\"deadline_ms\":120000,\"target_mask\":3,\"restart_type\":\"fixed\"}]}},\"variables\":null}" \
  >"$config"

TMP="$ipc_root" MIST_CONTROL=1 "$controller" -c "$config" -C r -L "$work/controller.log" &
controller_pid=$!
wait_for 30 "the test controller and its RTMP connector" logged "$work/controller.log" 'Started connector'

start_publisher() {
  "$ffmpeg" -hide_banner -loglevel error -re \
    -f lavfi -i "testsrc2=size=320x180:rate=25:duration=400" \
    -f lavfi -i "sine=frequency=997:sample_rate=48000:duration=400" \
    -c:v libx264 -pix_fmt yuv420p -preset ultrafast -g 50 -keyint_min 50 -bf 0 \
    -c:a aac -b:a 96k -f flv "rtmp://127.0.0.1:$rtmp_port/live/$stream" \
    >>"$work/publisher.log" 2>&1 &
  publisher_pid=$!
}
stop_publisher() {
  kill -TERM "$publisher_pid"
  wait "$publisher_pid" >/dev/null 2>&1 || true
  publisher_pid=
}
start_publisher
wait_for 20 "the stream to become active" logged "$work/controller.log" "Stream $stream became active"

livepeer_pid() { pgrep -f "MistProcLivepeer.*$stream" | head -n 1; }
running_livepeer() { [ -n "$(livepeer_pid)" ]; }
livepeer_restarted() { [ -n "$(livepeer_pid)" ] && [ "$(livepeer_pid)" != "$1" ]; }
started_livepeers() { grep -cE 'Started process [0-9]+: [^ ]*MistProcLivepeer' "$work/controller.log" || true; }
buffer_running() { pgrep -f "MistInBuffer.*$stream" >/dev/null; }
responses() { grep -c '^responded ' "$work/broadcaster.log" || true; }
responded_at_least() { [ "$(responses)" -ge "$1" ]; }
lists() { grep -c '^LIST ' "$track_lists" || true; }
# The rendition track's index: the publisher's tracks come first, so it is the highest index in
# the latest track list.
rendition_index() {
  tail -n 1 "$track_lists" | grep -oE '"idx":[0-9]+' | cut -d: -f2 | sort -n | tail -n 1
}
# kill_and_time <pid>: kills the Livepeer process and prints how long the buffer took to start
# its replacement, in ms.
kill_and_time() {
  began=$(date +%s%N)
  kill -KILL "$1"
  wait_for 20 "the Livepeer process to be restarted" livepeer_restarted "$1"
  echo $((($(date +%s%N) - began) / 1000000))
}

wait_for 30 "the Livepeer process to start" running_livepeer
wait_for 60 "renditions to flow" responded_at_least 4
two_videos() { [ "$(tail -n 1 "$track_lists" | grep -o '"type":"video"' | wc -l)" -ge 2 ]; }
wait_for 20 "the rendition track in the track list" two_videos
sleep 2
rendition=$(rendition_index)

case "$mode" in
  backpressure)
    before=$(livepeer_pid)
    : >"$hold_file"
    sleep 40
    rm -f "$hold_file"
    count=$(responses)
    wait_for 30 "renditions to flow again after the stall" responded_at_least $((count + 3))
    if [ "$(livepeer_pid)" != "$before" ]; then
      echo "the Livepeer process (PID $before) did not survive 40 s of back-pressure" >&2
      exit 1
    fi
    if grep -E "MistProcLivepeer" "$work/controller.log" | grep -qE 'Received signal|Stopping Livepeer process'; then
      echo "the Livepeer process was signalled or stopped during back-pressure" >&2
      exit 1
    fi
    echo "backpressure: PID $before kept running through a 40 s gateway stall"
    ;;
  stop-stall)
    pid=$(livepeer_pid)
    : >"$hold_file"
    sleep 6
    began=$(date +%s)
    kill -TERM "$pid"
    gone() { ! kill -0 "$pid" 2>/dev/null; }
    wait_for 30 "the stopped Livepeer process to exit" gone
    took=$(($(date +%s) - began))
    rm -f "$hold_file"
    if [ "$took" -gt 5 ]; then
      echo "the stopped Livepeer process took ${took}s to exit while its uploads were stalled" >&2
      exit 1
    fi
    echo "stop-stall: the stopped process exited after ${took}s despite stalled uploads"
    ;;
  restart)
    sleep 30
    worst=0
    for kill_round in 1 2 3; do
      took=$(kill_and_time "$(livepeer_pid)")
      if [ "$took" -gt "$worst" ]; then worst=$took; fi
      sleep 2
    done
    if [ "$worst" -gt 1500 ]; then
      echo "the buffer restarted a killed Livepeer process only after ${worst} ms" >&2
      exit 1
    fi
    echo "restart: killed Livepeer processes were restarted within ${worst} ms"
    ;;
  kill-resume)
    viewer="$work/viewer.mkv"
    TMP="$ipc_root" MIST_CONTROL=1 "$output_ebml" -s "$stream" "$viewer?video=all&audio=all" >"$work/viewer.log" 2>&1 &
    viewer_pid=$!
    sleep 6
    lists_before=$(lists)
    for kill_round in 1 2 3; do
      kill_and_time "$(livepeer_pid)" >/dev/null
      count=$(responses)
      wait_for 30 "renditions after restart $kill_round" responded_at_least $((count + 2))
      sleep 3
    done
    kill -TERM "$viewer_pid" >/dev/null 2>&1 || true
    wait "$viewer_pid" >/dev/null 2>&1 || true
    viewer_pid=
    if [ "$(lists)" -ne "$lists_before" ]; then
      echo "the track list changed across Livepeer restarts" >&2
      exit 1
    fi
    resumed=$(grep -cE "Resuming track $rendition \(output " "$work/controller.log" || true)
    if [ "$resumed" -lt 3 ]; then
      echo "the rendition track $rendition was resumed $resumed times over three restarts" >&2
      exit 1
    fi
    rendition_packets=$("$ffprobe" -v error -select_streams v:1 -count_packets -show_entries stream=nb_read_packets \
      -of default=nw=1:nk=1 "$viewer" 2>/dev/null || echo 0)
    gap=$("$ffprobe" -v error -select_streams v:1 -show_entries packet=pts_time -of csv=p=0 "$viewer" 2>/dev/null | \
      awk 'NR > 1 && $1 - prev > gap { gap = $1 - prev } { prev = $1 } END { printf "%.1f", gap }')
    if [ "${rendition_packets:-0}" -lt 100 ] || awk -v g="${gap:-0}" 'BEGIN { exit !(g < 1.0) }'; then
      echo "the viewer got ${rendition_packets:-0} rendition packets with a largest gap of ${gap:-0}s" >&2
      exit 1
    fi
    source_end=$("$ffprobe" -v error -select_streams v:0 -show_entries packet=pts_time -of csv=p=0 "$viewer" | tail -n 1)
    rendition_end=$("$ffprobe" -v error -select_streams v:1 -show_entries packet=pts_time -of csv=p=0 "$viewer" | tail -n 1)
    if awk -v s="$source_end" -v r="$rendition_end" 'BEGIN { exit !(r < s - 8) }'; then
      echo "the viewer's rendition ends at ${rendition_end}s, the source at ${source_end}s" >&2
      exit 1
    fi
    echo "kill-resume: track $rendition resumed three times; the viewer kept it (${rendition_packets} packets, largest gap ${gap}s)"
    ;;
  stall-kill)
    : >"$hold_file"
    sleep 48
    kill_and_time "$(livepeer_pid)" >/dev/null
    rm -f "$hold_file"
    count=$(responses)
    wait_for 40 "renditions after the restart" responded_at_least $((count + 3))
    sleep 3
    if grep -qE "Erasing .*track $rendition " "$work/controller.log"; then
      echo "the stalled rendition track $rendition was erased while its process restarted" >&2
      exit 1
    fi
    if ! grep -qE "Resuming track $rendition \(output " "$work/controller.log"; then
      echo "the restarted process did not continue rendition track $rendition" >&2
      exit 1
    fi
    echo "stall-kill: track $rendition survived a 48 s stall and was resumed after the restart"
    ;;
  resolution)
    : >"$alt_flag"
    kill_and_time "$(livepeer_pid)" >/dev/null
    wait_for 40 "the replacement rendition" logged "$work/broadcaster.log" 'responded [0-9]+ alternate'
    wait_for 20 "the old rendition to be removed" logged "$work/controller.log" "Removing track $rendition: replaced by track"
    sleep 2
    if ! grep -qE "Replacing track $rendition \(output " "$work/controller.log"; then
      echo "the restarted process did not replace rendition track $rendition explicitly" >&2
      exit 1
    fi
    last=$(tail -n 1 "$track_lists")
    if printf '%s' "$last" | grep -qE "\"idx\":$rendition[,}]"; then
      echo "the replaced rendition track $rendition is still listed" >&2
      exit 1
    fi
    if ! printf '%s' "$last" | grep -q '"width":160'; then
      echo "the replacement rendition is not listed" >&2
      exit 1
    fi
    if grep -qE "Erasing .*track $rendition " "$work/controller.log"; then
      echo "the replaced track $rendition was left to the idle timeout" >&2
      exit 1
    fi
    echo "resolution: track $rendition was replaced explicitly when its restart changed resolution"
    ;;
  live-end)
    pid=$(livepeer_pid)
    started=$(started_livepeers)
    stop_publisher
    stopped_at=$(date +%s)
    gone() { ! kill -0 "$pid" 2>/dev/null; }
    wait_for 60 "the Livepeer process to finish the stream" gone
    buffer_gone() { ! buffer_running; }
    wait_for 90 "the buffer to stop" buffer_gone
    took=$(($(date +%s) - stopped_at))
    restarted=$(($(started_livepeers) - started))
    if [ "$restarted" -ne 0 ]; then
      echo "the buffer restarted the Livepeer process $restarted times after the publisher left" >&2
      exit 1
    fi
    if grep -q 'Sending SIGKILL' "$work/controller.log"; then
      echo "the stream's processes had to be killed when the stream ended" >&2
      exit 1
    fi
    # The process ends its read of the left publisher's tracks after its 25 s data wait; the
    # buffer stops right after it instead of waiting 30 s for a restarted process.
    if [ "$took" -gt 40 ]; then
      echo "the buffer stopped ${took}s after the publisher left" >&2
      exit 1
    fi
    echo "live-end: the buffer stopped ${took}s after the publisher left, without restarting its process"
    ;;
  live-end-resume)
    stop_publisher
    sleep 3
    pid=$(livepeer_pid)
    started=$(started_livepeers)
    kill -KILL "$pid"
    sleep 10
    restarted=$(($(started_livepeers) - started))
    if [ "$restarted" -ne 0 ]; then
      echo "the buffer restarted the Livepeer process $restarted times while the publisher was away" >&2
      exit 1
    fi
    if ! buffer_running; then
      echo "the resumable stream's buffer stopped while its publisher was away" >&2
      exit 1
    fi
    count=$(responses)
    start_publisher
    wait_for 30 "the Livepeer process for the returning publisher" running_livepeer
    wait_for 60 "renditions for the returning publisher" responded_at_least $((count + 3))
    echo "live-end-resume: no restart while the publisher was away; the returning publisher got its process back"
    ;;
  stop-sessions)
    pid=$(livepeer_pid)
    curl -s --max-time 5 "http://127.0.0.1:$api_port/api2" --data-urlencode "command={\"stop_sessions\":\"$stream\"}" \
      >/dev/null
    stopped_at=$(date +%s)
    gone() { ! kill -0 "$pid" 2>/dev/null; }
    wait_for 60 "the Livepeer process to exit" gone
    took=$(($(date +%s) - stopped_at))
    buffer_gone() { ! buffer_running; }
    wait_for 90 "the buffer to stop" buffer_gone
    if grep -q 'Sending SIGKILL' "$work/controller.log"; then
      echo "the Livepeer process had to be killed after its sessions were stopped" >&2
      exit 1
    fi
    if [ "$took" -gt 5 ]; then
      echo "the Livepeer process exited ${took}s after its sessions were stopped" >&2
      exit 1
    fi
    echo "stop-sessions: the Livepeer process exited ${took}s after its sessions were stopped, without SIGKILL"
    ;;
esac
