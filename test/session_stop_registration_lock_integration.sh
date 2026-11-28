#!/bin/sh
# A session waiting for the statistics registration lock, which a session that died while
# registering left taken, still stops when signalled. At a service stop every process of the unit
# gets SIGTERM; a session that kept waiting outlived its controller until the stop timed out and
# SIGKILLed it.
set -eu

if [ "$#" -ne 2 ]; then
  echo "usage: $0 MistSession commsstatssegmentstest" >&2
  exit 2
fi
session_binary=$1
fixture=$2

work=$(mktemp -d "${TMPDIR:-/tmp}/mist-session-stop-lock.XXXXXX")
session_id="Iwedgedstop$$"
session_pid=

cleanup() {
  status=$?
  trap - EXIT INT TERM
  if [ -n "$session_pid" ] && kill -0 "$session_pid" 2>/dev/null; then
    kill -KILL "$session_pid" 2>/dev/null || true
    wait "$session_pid" 2>/dev/null || true
  fi
  if [ "$status" -ne 0 ] && [ -f "$work/session.log" ]; then
    echo "--- session.log" >&2
    tail -40 "$work/session.log" >&2
  fi
  "$fixture" clean-session "$session_id" >/dev/null 2>&1 || true
  rm -rf "$work"
  exit "$status"
}
trap cleanup EXIT
trap 'exit 130' INT TERM

fail() {
  echo "FAIL: $1" >&2
  exit 1
}

"$fixture" wedge-registration >"$work/fixture.log" 2>&1 || {
  cat "$work/fixture.log" >&2
  fail "the fixture leaves a statistics segment with its registration lock taken"
}

# MIST_CONTROL keeps the session's logs raw, as under a controller, instead of starting a log child.
MIST_CONTROL=1 "$session_binary" "$session_id" --streamname wedged --protocol INPUT:Buffer >"$work/session.log" 2>&1 &
session_pid=$!

# The session cannot register while the lock is taken, so it is still waiting a second later.
sleep 1
kill -0 "$session_pid" 2>/dev/null || fail "the session waits for the statistics registration lock"

kill -TERM "$session_pid"
attempt=0
while kill -0 "$session_pid" 2>/dev/null && [ "$attempt" -lt 30 ]; do
  sleep 0.1
  attempt=$((attempt + 1))
done
if kill -0 "$session_pid" 2>/dev/null; then
  fail "a session waiting for a registration lock left taken is still running 3 s after SIGTERM"
fi
wait "$session_pid" 2>/dev/null || true
session_pid=
echo "ok   a session waiting for a registration lock left taken stops on SIGTERM"
