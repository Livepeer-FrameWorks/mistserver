#!/bin/sh
# An always-on pull input (an SRT listener, here a UDP MPEG-TS listener) that was killed leaves its
# pull lock taken: a POSIX semaphore keeps its count when its holder dies. The controller must not
# take that lock for a running input forever; once no running input shows up for the grace period
# it reclaims the lock and starts the input.
set -eu

if [ "$#" -ne 4 ]; then
  echo "usage: $0 MistController MistInTS MistUtilNuke stale_lock_fixture" >&2
  exit 2
fi
controller=$1
input_ts=$2
util_nuke=$3
fixture=$4
for program in "$controller" "$input_ts" "$util_nuke" "$fixture"; do
  if [ ! -x "$program" ]; then
    echo "required executable is unavailable: $program" >&2
    exit 77
  fi
done

work=$(mktemp -d "${TMPDIR:-/tmp}/mist-always-on-stale.XXXXXX")
ipc_root="$work/ipc"
mkdir -p "$ipc_root"
stream="aostale$$"
pull_lock="/MstSemPull_$stream"
controller_pid=

cleanup() {
  status=$?
  trap - EXIT HUP INT TERM
  if [ "$status" -ne 0 ]; then
    echo "always-on stale lock integration failed; controller log follows:" >&2
    grep -E 'always-on|Always-on|MistInTS|pull' "$work/controller.log" 2>/dev/null | tail -40 >&2 || true
  fi
  if [ -n "$controller_pid" ]; then
    TMP="$ipc_root" MIST_CONTROL=1 "$util_nuke" "$stream" >/dev/null 2>&1 || true
    kill -INT "$controller_pid" >/dev/null 2>&1 || true
    wait "$controller_pid" >/dev/null 2>&1 || true
  fi
  pkill -f "MistInTS.*$stream" >/dev/null 2>&1 || true
  TMP="$ipc_root" "$fixture" --release "$pull_lock" >/dev/null 2>&1 || true
  rm -rf -- "$work"
  exit "$status"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

TMP="$ipc_root" "$fixture" "$pull_lock"

api_port=$((24000 + ($$ % 9000)))
udp_port=$((api_port + 1))
printf '%s\n' \
  "{\"account\":{\"test\":{\"password\":\"098f6bcd4621d373cade4e832627b4f6\"}},\"config\":{\"accesslog\":\"LOG\",\"controller\":{\"interface\":\"127.0.0.1\",\"port\":$api_port,\"username\":null},\"debug\":4,\"protocols\":[],\"trustedproxy\":[]},\"streams\":{\"$stream\":{\"name\":\"$stream\",\"source\":\"tsudp://127.0.0.1:$udp_port\",\"always_on\":true}}}" \
  >"$work/config.json"

TMP="$ipc_root" MIST_CONTROL=1 "$controller" -c "$work/config.json" -C r -L "$work/controller.log" &
controller_pid=$!

input_running() { pgrep -f "MistInTS.*$stream" >/dev/null 2>&1; }
deadline=$(($(date +%s) + 45))
until input_running; do
  if [ "$(date +%s)" -gt "$deadline" ]; then
    echo "the always-on input was never started while a dead input's pull lock was left taken" >&2
    exit 1
  fi
  sleep 0.5
done
if ! grep -q "Always-on input for stream $stream looked served for" "$work/controller.log"; then
  echo "the always-on input started without the controller reclaiming the abandoned lock" >&2
  exit 1
fi
echo "the controller reclaimed the abandoned pull lock and started the always-on input"
