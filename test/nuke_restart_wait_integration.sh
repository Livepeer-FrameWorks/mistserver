#!/bin/sh
# A stream requested while MistUtilNuke is still cleaning up its previous
# generation must wait for the nuke instead of racing it. Util::startInput
# waits while the stream's status reads anything but off, offline or ready (a
# push provider also goes ahead on "waiting for data"), so the nuke must hold
# the status at "shutting down" until it has released the stream's locks, and
# then remove it so the waiting request boots.
#
# The fixture freezes the old buffer's child, so the nuke waits on the old
# generation before wiping the stream's pages, and reads the status in that
# window.
set -eu

if [ "$#" -ne 4 ]; then
  echo "usage: $0 MistController MistInBuffer MistUtilNuke timeout" >&2
  exit 2
fi

controller=$1
input_buffer=$2
util_nuke=$3
timeout_program=$4
for program in "$controller" "$input_buffer" "$util_nuke" "$timeout_program"; do
  if [ ! -x "$program" ]; then
    echo "required executable is unavailable: $program" >&2
    exit 77
  fi
done
if [ ! -d /dev/shm ]; then
  echo "shared memory pages are not files under /dev/shm on this platform" >&2
  exit 77
fi

work=$(mktemp -d "${TMPDIR:-/tmp}/mist-nuke-wait.XXXXXX")
ipc_root="$work/ipc"
mkdir -p "$ipc_root"
stream="nukewait$$"
controller_pid=
old_pid=
old_child=
nuke_pid=

cleanup() {
  status=$?
  trap - EXIT HUP INT TERM
  if [ "$status" -ne 0 ]; then
    echo "nuke restart wait integration failed; logs follow:" >&2
    for log in "$work"/*.log; do
      if [ -f "$log" ]; then
        echo "Log: $log" >&2
        tail -40 "$log" >&2
      fi
    done
  fi
  if [ -n "$old_child" ]; then kill -KILL "$old_child" >/dev/null 2>&1 || true; fi
  for pid in $old_pid $nuke_pid; do
    kill -TERM "$pid" >/dev/null 2>&1 || true
    wait "$pid" >/dev/null 2>&1 || true
  done
  if [ -n "$controller_pid" ]; then
    TMP="$ipc_root" MIST_CONTROL=1 "$util_nuke" "$stream" >/dev/null 2>&1 || true
    kill -INT "$controller_pid" >/dev/null 2>&1 || true
    wait "$controller_pid" >/dev/null 2>&1 || true
  fi
  rm -rf -- "$work"
  exit "$status"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

port=$((21000 + ($$ % 15000)))
config="$work/config.json"
printf '%s\n' \
  "{\"account\":{\"test\":{\"password\":\"098f6bcd4621d373cade4e832627b4f6\"}},\"auto_push\":null,\"bandwidth\":{\"exceptions\":[\"::1\",\"127.0.0.0/8\"]},\"config\":{\"accesslog\":\"LOG\",\"controller\":{\"interface\":\"127.0.0.1\",\"port\":$port,\"username\":null},\"debug\":4,\"defaultStream\":null,\"prometheus\":\"\",\"protocols\":[],\"serverid\":null,\"sessionInputMode\":15,\"sessionOutputMode\":15,\"sessionStreamInfoMode\":1,\"sessionUnspecifiedMode\":0,\"sessionViewerMode\":14,\"tknMode\":15,\"triggers\":{},\"trustedproxy\":[]},\"extwriters\":null,\"jwks\":null,\"push_settings\":{\"maxspeed\":0,\"wait\":3},\"streamkeys\":null,\"streams\":{},\"variables\":null}" \
  >"$config"

TMP="$ipc_root" MIST_CONTROL=1 "$controller" -c "$config" -C r -L "$work/controller.log" &
controller_pid=$!

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
    sleep 0.02
  done
}
logged() { grep -q "$2" "$1" 2>/dev/null; }
wait_for 5 "the test controller" logged "$work/controller.log" 'Controller started'

TMP="$ipc_root" MIST_CONTROL=1 "$input_buffer" -s "$stream" "push://INTERNAL_ONLY:test" >"$work/old.log" 2>&1 &
old_pid=$!
wait_for 10 "the first generation" logged "$work/old.log" 'Input started'
old_child=$(grep 'Input started' "$work/old.log" | head -n 1 | cut -d'|' -f3)
kill -STOP "$old_child"

TMP="$ipc_root" MIST_CONTROL=1 "$timeout_program" 30 "$util_nuke" "$stream" >"$work/nuke.log" 2>&1 &
nuke_pid=$!
wait_for 10 "the nuke to hold the input lock and stop the inputs" logged "$work/nuke.log" 'Detecting running inputs'

state=$(od -An -t u1 -N 1 "/dev/shm/MstSTATE$stream" 2>/dev/null | tr -d ' ')
if [ "$state" != "5" ]; then
  echo "mid-nuke stream status is '${state:-no page}', not shutting down (5): a restart would not wait for the nuke" >&2
  exit 1
fi

wait "$nuke_pid" >/dev/null 2>&1 || true
nuke_pid=
if [ -e "/dev/shm/MstSTATE$stream" ]; then
  echo "the nuke finished but left the stream's status page, so a waiting restart never proceeds" >&2
  exit 1
fi
echo "the nuke held the stream at shutting down until it was done"
