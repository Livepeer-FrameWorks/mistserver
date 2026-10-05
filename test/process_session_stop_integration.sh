#!/bin/sh
# Stream processes (AV opus-from-AAC, Thumbs, Livepeer against the loopback broadcaster stub) of
# a live RTMP stream, configured as on the platform (resumable, 12 s inactivity timeout).
# MIST_SESSION_STOP_MODE selects the case:
#   stop          the stream's sessions are stopped through the API while the publisher is live;
#                 every process reports PROCESS_EXIT status "stopped" and none is started again.
#   stop-return   as stop, then a publisher returns; it gets its processes back.
#   live-end      the publisher of a non-resumable stream leaves; the processes play out the
#                 buffer, report a clean end of stream (CLEAN_EOF) and are not started again.
#   expire        the AV process stalls until its reading session ends for lack of updates, and
#                 resumes as that session closes; that is no stop by the controller, so the
#                 process reports no "stopped" and is started again. Linux only (reads the
#                 session's page in /dev/shm).
set -eu

if [ "$#" -ne 10 ]; then
  echo "usage: $0 ffmpeg curl MistController MistInBuffer MistOutRTMP MistProcAV MistProcThumbs MistProcLivepeer MistUtilNuke broadcaster-stub" >&2
  exit 2
fi
if [ "${MIST_RUN_MEDIA_TESTS:-}" != "1" ]; then
  echo "set MIST_RUN_MEDIA_TESTS=1 to run the live process pipeline" >&2
  exit 77
fi

ffmpeg=$1
curl=$2
controller=$3
input_buffer=$4
output_rtmp=$5
process_av=$6
process_thumbs=$7
process_livepeer=$8
util_nuke=$9
broadcaster_stub=${10}
for program in "$ffmpeg" "$curl" "$controller" "$input_buffer" "$output_rtmp" "$process_av" "$process_thumbs" \
  "$process_livepeer" "$util_nuke" "$broadcaster_stub"; do
  if [ ! -x "$program" ]; then
    echo "required executable is unavailable: $program" >&2
    exit 77
  fi
done
if ! "$ffmpeg" -hide_banner -encoders 2>/dev/null | grep -q 'libx264'; then
  echo "ffmpeg lacks the libx264 encoder required for the publisher fixture" >&2
  exit 77
fi

mode=${MIST_SESSION_STOP_MODE:-stop}
case "$mode" in
  stop | stop-return | live-end) ;;
  expire)
    if [ ! -d /dev/shm ]; then
      echo "the expire mode reads session pages in /dev/shm" >&2
      exit 77
    fi
    ;;
  *)
    echo "unknown MIST_SESSION_STOP_MODE $mode" >&2
    exit 2
    ;;
esac

work=$(mktemp -d "${TMPDIR:-/tmp}/mist-session-stop.XXXXXX")
ipc_root="$work/ipc"
mkdir -p "$ipc_root"
stream="sessstop$$"
controller_pid=
publisher_pid=
stub_pid=

cleanup() {
  status=$?
  trap - EXIT HUP INT TERM
  if [ "$status" -ne 0 ]; then
    echo "process session stop integration ($mode) failed; logs follow:" >&2
    grep -E 'Started process|exit reason|was stopped|New publisher|SIGKILL|no activity' "$work/controller.log" 2>/dev/null |
      tail -60 >&2 || true
    cat "$work/exits" >&2 2>/dev/null || true
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

api_port=$((24000 + ($$ % 9000)))
rtmp_port=$((api_port + 1))
broadcaster_port=$((api_port + 2))
"$broadcaster_stub" "$broadcaster_port" >"$work/broadcaster.log" 2>&1 &
stub_pid=$!
wait_for 10 "the loopback broadcaster" logged "$work/broadcaster.log" '^ready$'

exits="$work/exits"
handler="$work/process-exit-handler.sh"
cat >"$handler" <<EOF
#!/bin/sh
printf 'EXIT %s\n' "\$(tr '\n' '|')" >>"$exits"
EOF
chmod +x "$handler"
: >"$exits"

stream_settings=',"resume":1,"inputtimeout":12'
if [ "$mode" = live-end ]; then stream_settings=; fi
config="$work/config.json"
printf '%s\n' \
  "{\"account\":{\"test\":{\"password\":\"098f6bcd4621d373cade4e832627b4f6\"}},\"bandwidth\":{\"exceptions\":[\"::1\",\"127.0.0.0/8\"]},\"config\":{\"accesslog\":\"LOG\",\"controller\":{\"interface\":\"127.0.0.1\",\"port\":$api_port,\"username\":null},\"debug\":4,\"protocols\":[{\"connector\":\"RTMP\",\"interface\":\"127.0.0.1\",\"port\":$rtmp_port}],\"sessionInputMode\":15,\"sessionOutputMode\":15,\"sessionStreamInfoMode\":1,\"sessionUnspecifiedMode\":0,\"sessionViewerMode\":14,\"tknMode\":15,\"triggers\":{\"PROCESS_EXIT\":[{\"handler\":\"$handler\",\"sync\":false,\"streams\":[\"$stream\"]}]},\"trustedproxy\":[]},\"push_settings\":{\"maxspeed\":0,\"wait\":3},\"streams\":{\"$stream\":{\"name\":\"$stream\",\"source\":\"push://\"$stream_settings,\"processes\":[{\"process\":\"AV\",\"codec\":\"opus\",\"track_inhibit\":\"audio=opus\",\"track_select\":\"audio=aac&video=none&subtitle=none&meta=none\"},{\"process\":\"Thumbs\",\"track_select\":\"video=maxbps&audio=none\"},{\"process\":\"Livepeer\",\"hardcoded_broadcasters\":\"http://127.0.0.1:$broadcaster_port\",\"target_profiles\":[{\"name\":\"audit\",\"bitrate\":400000,\"width\":320,\"height\":180,\"fps\":25,\"gop\":\"2.0\"}],\"target_mask\":3}]}}}" \
  >"$config"

TMP="$ipc_root" MIST_CONTROL=1 "$controller" -c "$config" -C r -L "$work/controller.log" &
controller_pid=$!
wait_for 30 "the test controller and its RTMP connector" logged "$work/controller.log" 'Started connector'

start_publisher() {
  "$ffmpeg" -hide_banner -loglevel error -re \
    -f lavfi -i "testsrc2=size=320x180:rate=25:duration=$1" \
    -f lavfi -i "sine=frequency=997:sample_rate=48000:duration=$1" \
    -c:v libx264 -pix_fmt yuv420p -preset ultrafast -g 50 -keyint_min 50 -bf 0 \
    -c:a aac -b:a 96k -f flv "rtmp://127.0.0.1:$rtmp_port/live/$stream" \
    >>"$work/publisher.log" 2>&1 &
  publisher_pid=$!
}
started() { grep -cE 'Started process [0-9]+: [^ ]*MistProc' "$work/controller.log" || true; }
started_at_least() { [ "$(started)" -ge "$1" ]; }
exits_with() { grep -c "|$1|" "$exits" || true; }
exit_status() { grep "|$1|{" "$exits" | head -n 1 | awk -F'|' '{print $7 "|" $8}'; }
three_exits() { [ "$(grep -c '^EXIT ' "$exits" || true)" -ge 3 ]; }
responses() { grep -c '^responded ' "$work/broadcaster.log" || true; }
responded_at_least() { [ "$(responses)" -ge "$1" ]; }
buffer_gone() { ! pgrep -f "MistInBuffer.*$stream" >/dev/null; }

# The id of the stream's session whose protocol matches the given pattern; the controller lists a
# session once its statistics came in.
listed_session() {
  deadline=$(($(date +%s) + 40))
  while [ "$(date +%s)" -le "$deadline" ]; do
    found=$("$curl" -s --max-time 5 "http://127.0.0.1:$api_port/api2" \
      --data-urlencode "command={\"clients\":{\"time\":-2,\"fields\":[\"stream\",\"protocol\",\"sessid\"]}}" |
      grep -oE "\"$stream\",\"$1\",\"[^\"]+\"" | head -n 1 | cut -d'"' -f6 || true)
    if [ -n "$found" ]; then
      printf '%s\n' "$found"
      return 0
    fi
    sleep 1
  done
}

start_publisher 300
wait_for 30 "the three processes to start" started_at_least 3
wait_for 60 "renditions to flow" responded_at_least 3
sleep 3

case "$mode" in
  stop | stop-return)
    "$curl" -s --max-time 5 "http://127.0.0.1:$api_port/api2" \
      --data-urlencode "command={\"stop_sessions\":\"$stream\"}" >/dev/null
    wait "$publisher_pid" >/dev/null 2>&1 || true
    publisher_pid=
    wait_for 20 "the three processes to report their exit" three_exits
    sleep 3
    for process in AV Thumbs Livepeer; do
      if [ "$(exit_status "$process")" != "stopped|CLEAN_CONTROLLER_REQ" ]; then
        echo "the $process process reported '$(exit_status "$process")' after its session was stopped" >&2
        exit 1
      fi
    done
    if [ "$(started)" -ne 3 ]; then
      echo "the buffer started $(($(started) - 3)) processes after the stream's sessions were stopped" >&2
      exit 1
    fi
    if [ "$mode" = stop ]; then
      wait_for 40 "the buffer to stop" buffer_gone
      if grep -q 'Sending SIGKILL' "$work/controller.log"; then
        echo "the buffer had to kill processes after the stream's sessions were stopped" >&2
        exit 1
      fi
      echo "stop: every process reported 'stopped' and none was started again"
    else
      count=$(responses)
      start_publisher 300
      wait_for 30 "the returning publisher's processes to start" started_at_least 6
      wait_for 60 "renditions for the returning publisher" responded_at_least $((count + 3))
      echo "stop-return: no process restarted after the stop; the returning publisher got its processes back"
    fi
    ;;
  expire)
    sessid=$(listed_session '(OUTPUT:)?AV')
    av_pid=$(pgrep -f "MistProcAV.*$stream" | head -n 1 || true)
    if [ -z "$sessid" ] || [ -z "$av_pid" ]; then
      echo "the AV process ($av_pid) or its reading session ($sessid) is not found" >&2
      exit 1
    fi
    page="/dev/shm/MstSession$sessid"
    # The session stops counting a connection 10 s after its last update and ends once it counted
    # none for 15 s: it flags its page closed, then asks what is left to disconnect and, after
    # about a second, terminates it.
    page_closed() {
      flags=$(od -An -tu1 -N1 "$page" 2>/dev/null | tr -d ' ')
      [ -n "$flags" ] && [ $((flags & 2)) -ne 0 ]
    }
    kill -STOP "$av_pid"
    wait_for 60 "the stalled AV process's session to end" page_closed
    kill -CONT "$av_pid"
    av_exited() { grep -q '|AV|{' "$exits"; }
    wait_for 20 "the AV process to report its exit" av_exited
    if [ "$(exit_status AV)" = "stopped|CLEAN_CONTROLLER_REQ" ]; then
      echo "the AV process reported a stop by the controller after its session ended for lack of updates" >&2
      exit 1
    fi
    wait_for 20 "the AV process to be started again" started_at_least 4
    echo "expire: the AV process whose session ended while it stalled reported '$(exit_status AV)' and was started again"
    ;;
  live-end)
    kill -TERM "$publisher_pid"
    wait "$publisher_pid" >/dev/null 2>&1 || true
    publisher_pid=
    wait_for 60 "the three processes to report their exit" three_exits
    wait_for 60 "the buffer to stop" buffer_gone
    for process in AV Thumbs Livepeer; do
      if [ "$(exit_status "$process")" != "clean|CLEAN_EOF" ]; then
        echo "the $process process reported '$(exit_status "$process")' at the end of its stream" >&2
        exit 1
      fi
    done
    if [ "$(started)" -ne 3 ]; then
      echo "the buffer started $(($(started) - 3)) processes after the publisher left" >&2
      exit 1
    fi
    if grep -q 'Sending SIGKILL' "$work/controller.log"; then
      echo "the buffer had to kill processes at the end of the stream" >&2
      exit 1
    fi
    echo "live-end: every process reported a clean end of stream and none was started again"
    ;;
esac
