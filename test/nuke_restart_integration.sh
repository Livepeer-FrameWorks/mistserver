#!/bin/sh
# A stream restarted while MistUtilNuke is still tearing down its previous
# generation must survive the nuke. The nuke holds the stream's input lock, but
# an exiting input unlinks that lock's name, after which the next input creates
# a fresh lock under the same name and boots while the nuke keeps working on
# every page named after the stream.
#
# The fixture makes that window deterministic: the old buffer's child is
# frozen, so the nuke waits on the old generation before wiping the stream's
# pages; meanwhile the lock name is unlinked the way an exiting input does and
# the next generation boots. The nuke must then leave the new generation
# running.
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
  echo "named semaphores are not files under /dev/shm on this platform" >&2
  exit 77
fi

work=$(mktemp -d "${TMPDIR:-/tmp}/mist-nuke-restart.XXXXXX")
ipc_root="$work/ipc"
mkdir -p "$ipc_root"
stream="nukerestart$$"
controller_pid=
old_pid=
old_child=
new_pid=
nuke_pid=

cleanup() {
  status=$?
  trap - EXIT HUP INT TERM
  if [ "$status" -ne 0 ]; then
    echo "nuke restart integration failed; logs follow:" >&2
    for log in "$work"/*.log; do
      if [ -f "$log" ]; then
        echo "Log: $log" >&2
        tail -40 "$log" >&2
      fi
    done
  fi
  if [ -n "$old_child" ]; then kill -KILL "$old_child" >/dev/null 2>&1 || true; fi
  for pid in $old_pid $new_pid $nuke_pid; do
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

# The buffer runs with its angel, as the controller starts it: the angel holds
# the input lock and its child serves the stream.
TMP="$ipc_root" MIST_CONTROL=1 "$input_buffer" -s "$stream" "push://INTERNAL_ONLY:test" >"$work/old.log" 2>&1 &
old_pid=$!
wait_for 10 "the first generation" logged "$work/old.log" 'Input started'
old_child=$(grep 'Input started' "$work/old.log" | head -n 1 | cut -d'|' -f3)
lock="/dev/shm/sem.MstSemInpt$stream"
if [ ! -e "$lock" ]; then
  echo "input lock $lock is not a file under /dev/shm" >&2
  exit 77
fi
kill -STOP "$old_child"

TMP="$ipc_root" MIST_CONTROL=1 "$timeout_program" 30 "$util_nuke" "$stream" >"$work/nuke.log" 2>&1 &
nuke_pid=$!
# The nuke now holds the input lock and waits for the frozen generation's
# processes to exit before it wipes the stream's pages.
wait_for 10 "the nuke to hold the input lock and stop the inputs" logged "$work/nuke.log" 'Detecting running inputs'

# What an exiting input does with its lock: the name goes, and the lock the
# nuke holds stays behind under no name.
rm -f -- "$lock"
TMP="$ipc_root" MIST_CONTROL=1 "$input_buffer" -s "$stream" "push://INTERNAL_ONLY:test" >"$work/new.log" 2>&1 &
new_pid=$!
wait_for 10 "the next generation" logged "$work/new.log" 'Input started'
if ! kill -0 "$nuke_pid" 2>/dev/null; then
  echo "the nuke finished before the next generation started; the fixture did not hold it in its shutdown loop" >&2
  exit 1
fi

wait "$nuke_pid" >/dev/null 2>&1 || true
nuke_pid=
sleep 1
if ! kill -0 "$new_pid" 2>/dev/null || grep -q 'signal Terminated' "$work/new.log"; then
  echo "the nuke of the previous generation stopped the stream's new generation" >&2
  exit 1
fi
# The new generation's own shared state must still be there and still name it.
if [ ! -e "/dev/shm/MstSTATE$stream" ]; then
  echo "the nuke of the previous generation wiped the new generation's stream state page" >&2
  exit 1
fi
input_pid=$(od -An -t u8 -N 8 "/dev/shm/MstIPID$stream" 2>/dev/null | tr -d ' ')
if [ "$input_pid" != "$new_pid" ]; then
  echo "the stream's input PID page names '${input_pid:-nothing}', not the new generation's input $new_pid" >&2
  exit 1
fi
if ! grep -q 'restarted during the nuke' "$work/nuke.log"; then
  echo "the nuke did not report leaving the new generation alone" >&2
  exit 1
fi
echo "the stream's new generation survived the nuke of its previous one"
