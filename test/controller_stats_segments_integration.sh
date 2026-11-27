#!/bin/sh
# A controller started next to the statistics and state pages a previous release left behind, as
# after an in-place Mist update, publishes pages with its own layout, keeps counting the sessions
# registered on the previous release's page, finds them again after a rolling restart, and
# unlinks its pages on a full shutdown.
set -eu

if [ "$#" -ne 2 ]; then
  echo "usage: $0 MistController commsstatssegmentstest" >&2
  exit 2
fi
controller_binary=$1
fixture=$2

work=$(mktemp -d "${TMPDIR:-/tmp}/mist-controller-stats-segments.XXXXXX")
ipc_root="$work/ipc"
mkdir -p "$ipc_root"
angel_pid=
legacy_pid=
fresh_pid=

stop_pid() {
  if [ -n "$1" ] && kill -0 "$1" 2>/dev/null; then
    kill -TERM "$1" 2>/dev/null || true
    wait "$1" 2>/dev/null || true
  fi
}

cleanup() {
  stop_pid "$legacy_pid"
  stop_pid "$fresh_pid"
  if [ -n "$angel_pid" ] && kill -0 "$angel_pid" 2>/dev/null; then
    kill -INT "$angel_pid" 2>/dev/null || true
    attempt=0
    while kill -0 "$angel_pid" 2>/dev/null && [ "$attempt" -lt 300 ]; do
      sleep 0.05
      attempt=$((attempt + 1))
    done
    kill -KILL "$angel_pid" 2>/dev/null || true
    wait "$angel_pid" 2>/dev/null || true
  fi
  "$fixture" clean >/dev/null 2>&1 || true
  rm -rf "$work"
}
trap cleanup EXIT INT TERM

fail() {
  echo "FAIL: $1" >&2
  if [ -f "$work/controller.log" ]; then
    echo "--- controller.log" >&2
    grep -E 'statistics|state page|Controller started|USR1' "$work/controller.log" | tail -40 >&2 || true
  fi
  exit 1
}

# Waits up to $2 tenths of a second for the output of command $1 to contain $3.
wait_for() {
  wf_attempt=0
  while [ "$wf_attempt" -lt "$2" ]; do
    if sh -c "$1" 2>/dev/null | grep -qF -- "$3"; then return 0; fi
    sleep 0.1
    wf_attempt=$((wf_attempt + 1))
  done
  return 1
}

started_count() { grep -c "Controller started" "$work/controller.log" 2>/dev/null || true; }

"$fixture" clean
"$fixture" seed-previous-release || fail "could not create the previous release's pages"

# A session of the previous release, registered before the new controller starts.
"$fixture" hold Ilegacysession "" >"$work/legacy.out" 2>"$work/legacy.err" &
legacy_pid=$!
wait_for "cat '$work/legacy.out'" 50 "/" || fail "the previous release's session did not register"
[ "$(head -n 1 "$work/legacy.out")" = "/MstStat" ] || fail "the previous release's session registered on $(cat "$work/legacy.out")"

port=$(python3 "$(dirname "$0")/test_ports.py" 1 $$)
printf '{"config":{"controller":{"interface":"127.0.0.1","port":%d},"debug":4,"prometheus":"metrics","protocols":[]},"streams":{}}\n' \
  "$port" >"$work/config.json"
TMP="$ipc_root" MIST_CONTROL=1 "$controller_binary" -c "$work/config.json" -C r -L "$work/controller.log" \
  >"$work/angel.err" 2>&1 &
angel_pid=$!
wait_for "grep -c 'Controller started' '$work/controller.log'" 200 "1" || fail "the controller did not start"

"$fixture" check-started >"$work/started.out" || { cat "$work/started.out"; fail "the controller start left the previous release's layout in place"; }
current=$(sed -n 's/^current //p' "$work/started.out")

"$fixture" hold Vfreshsession alias-stream >"$work/fresh.out" 2>"$work/fresh.err" &
fresh_pid=$!
wait_for "cat '$work/fresh.out'" 50 "/" || fail "a new session did not register"
[ "$(head -n 1 "$work/fresh.out")" = "$current" ] || fail "a new session registered on $(cat "$work/fresh.out") instead of $current"
"$fixture" check-session "$current" Vfreshsession alias-stream || fail "the new session's requested stream was not stored"

metrics="curl -s http://127.0.0.1:$port/metrics"
wait_for "$metrics" 50 'mist_sessions_total{sessType="incoming"}1' || fail "the controller does not count the previous release's session"
wait_for "$metrics" 50 'mist_sessions_total{sessType="viewers"}1' || fail "the controller does not count the new session"

# A rolling restart: the next controller finds both segments through the index.
kill -USR1 "$angel_pid"
wait_for "grep -c 'Controller started' '$work/controller.log'" 300 "2" || fail "the controller did not restart"
"$fixture" check-started >"$work/restarted.out" || { cat "$work/restarted.out"; fail "the restarted controller lost a statistics page"; }
[ "$(sed -n 's/^current //p' "$work/restarted.out")" = "$current" ] || fail "the restarted controller replaced the current segment"
wait_for "$metrics" 50 'mist_sessions_total{sessType="incoming"}1' || fail "the restarted controller lost the previous release's session"
wait_for "$metrics" 50 'mist_sessions_total{sessType="viewers"}1' || fail "the restarted controller lost the new session"

# Once its last session ends the legacy page closes registration.
stop_pid "$legacy_pid"
legacy_pid=
wait_for "'$fixture' check-legacy-closed" 100 "ok" || fail "the drained legacy page kept registration open"

stop_pid "$fresh_pid"
fresh_pid=
kill -INT "$angel_pid"
attempt=0
while kill -0 "$angel_pid" 2>/dev/null && [ "$attempt" -lt 300 ]; do
  sleep 0.05
  attempt=$((attempt + 1))
done
kill -0 "$angel_pid" 2>/dev/null && fail "the controller did not stop"
wait "$angel_pid" 2>/dev/null || true
angel_pid=
"$fixture" check-shutdown "$current" || fail "a full shutdown left statistics pages behind"
echo "a controller beside the previous release's pages publishes, keeps and unlinks its statistics segments"
