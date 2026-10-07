#!/bin/sh
# A session whose boot crosses a second boundary, between the static initialisation of its clock
# and main(), stays up for its connections. The preloaded helper steps CLOCK_MONOTONIC by one
# second at main(); a session without connections must then stay up for STATS_DELAY seconds
# instead of shutting down at once with no exit reason.
set -eu

if [ "$#" -ne 3 ]; then
  echo "usage: $0 MistController MistSession clock-step-preload" >&2
  exit 2
fi
controller_binary=$1
session_binary=$2
preload=$3
if [ ! -f "$preload" ]; then
  echo "clock step helper is missing: $preload" >&2
  exit 2
fi

work=$(mktemp -d "${TMPDIR:-/tmp}/mist-session-second-boundary.XXXXXX")
ipc_root="$work/ipc"
mkdir -p "$ipc_root"
controller_pid=
session_pid=

cleanup() {
  status=$?
  trap - EXIT INT TERM
  if [ -n "$session_pid" ] && kill -0 "$session_pid" 2>/dev/null; then
    kill -KILL "$session_pid" 2>/dev/null || true
    wait "$session_pid" 2>/dev/null || true
  fi
  if [ -n "$controller_pid" ] && kill -0 "$controller_pid" 2>/dev/null; then
    kill -INT "$controller_pid" 2>/dev/null || true
    wait "$controller_pid" 2>/dev/null || true
  fi
  if [ "$status" -ne 0 ]; then
    for log in "$work"/*.log; do
      if [ -f "$log" ]; then
        echo "Log: $log" >&2
        tail -50 "$log" >&2
      fi
    done
  fi
  rm -rf "$work"
  exit "$status"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

port=$((21000 + ($$ % 20000)))
printf '{"config":{"controller":{"interface":"127.0.0.1","port":%d},"debug":4,"protocols":[]},"streams":{}}\n' "$port" \
  >"$work/config.json"
ATHEIST=1 TMP="$ipc_root" MIST_CONTROL=1 "$controller_binary" -c "$work/config.json" -C r -L "$work/controller.log" \
  >/dev/null 2>&1 &
controller_pid=$!

attempt=0
until grep -q 'Controller started' "$work/controller.log" 2>/dev/null; do
  if ! kill -0 "$controller_pid" 2>/dev/null || [ "$attempt" -ge 300 ]; then
    echo "test controller did not become ready" >&2
    exit 1
  fi
  attempt=$((attempt + 1))
  sleep 0.1
done

session_id="O$(printf '%016x' "$$")"
TMP="$ipc_root" LD_PRELOAD="$preload" "$session_binary" "$session_id" -s secondboundary >"$work/session.log" 2>&1 &
session_pid=$!

attempt=0
until grep -q "Started new session $session_id" "$work/session.log" 2>/dev/null; do
  if ! kill -0 "$session_pid" 2>/dev/null || [ "$attempt" -ge 300 ]; then
    echo "session $session_id did not start" >&2
    exit 1
  fi
  attempt=$((attempt + 1))
  sleep 0.1
done

# A session without connections lives STATS_DELAY (15) seconds; the defect ends it within
# milliseconds of its start, so three seconds of observation separate the two.
attempt=0
while [ "$attempt" -lt 30 ]; do
  if ! kill -0 "$session_pid" 2>/dev/null; then
    echo "session $session_id shut down right after it started:" >&2
    grep 'Shutting down session' "$work/session.log" >&2 || true
    exit 1
  fi
  attempt=$((attempt + 1))
  sleep 0.1
done
echo "session $session_id stayed up across the second boundary of its boot"
