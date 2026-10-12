#!/bin/sh
# The controller's angel process starts exactly one controller, also when the
# binary was started under another name (a symlink). A controller that judged
# its angel by the executable's name took the angel for a missing parent,
# became an angel itself and started another controller, without end.
set -eu

if [ "$#" -ne 1 ]; then
  echo "usage: $0 MistController" >&2
  exit 2
fi
# Absolute, so the symlink below resolves from anywhere.
controller_binary=$(CDPATH= cd -- "$(dirname -- "$1")" && pwd)/$(basename -- "$1")

work=$(mktemp -d "${TMPDIR:-/tmp}/mist-controller-angel.XXXXXX")
ipc_root="$work/ipc"
mkdir -p "$ipc_root"
angel_pid=
link="$work/mist-controller-link"
ln -s "$controller_binary" "$link"

# Kills every process matching $1. A runaway chain keeps starting processes, so
# all of them are stopped first (a stopped process starts nothing) and killed
# once no new one appears.
kill_matching() {
  km_previous=-1
  km_attempt=0
  while [ "$km_attempt" -lt 200 ]; do
    pkill -STOP -f -- "$1" 2>/dev/null || true
    km_count=$(pgrep -f -- "$1" 2>/dev/null | wc -l | tr -d ' ')
    if [ "$km_count" -eq "$km_previous" ]; then break; fi
    km_previous=$km_count
    sleep 0.02
    km_attempt=$((km_attempt + 1))
  done
  km_attempt=0
  while pgrep -f -- "$1" >/dev/null 2>&1 && [ "$km_attempt" -lt 100 ]; do
    pkill -KILL -f -- "$1" 2>/dev/null || true
    sleep 0.05
    km_attempt=$((km_attempt + 1))
  done
}

cleanup() {
  if [ -n "$angel_pid" ]; then kill -INT "$angel_pid" 2>/dev/null || true; fi
  attempt=0
  while [ -n "$angel_pid" ] && kill -0 "$angel_pid" 2>/dev/null && [ "$attempt" -lt 200 ]; do
    sleep 0.05
    attempt=$((attempt + 1))
  done
  kill_matching "$work/"
  if [ -n "$angel_pid" ]; then wait "$angel_pid" 2>/dev/null || true; fi
  rm -rf "$work"
}
trap cleanup EXIT INT TERM

fail() {
  echo "FAIL: $1" >&2
  for f in "$work/angel.err" "$work/controller.log"; do
    if [ -f "$f" ]; then
      echo "--- $f" >&2
      sed -n '1,60p' "$f" >&2
    fi
  done
  exit 1
}

count_link() { pgrep -f -- "^$link " 2>/dev/null | wc -l | tr -d ' '; }

port=$((21000 + ($$ % 20000)))
printf '{"config":{"controller":{"interface":"127.0.0.1","port":%d},"debug":4,"prometheus":"","protocols":[]},"streams":{}}\n' \
  "$port" >"$work/config.json"

TMP="$ipc_root" MIST_CONTROL=1 "$link" -c "$work/config.json" -C r -L "$work/controller.log" \
  >"$work/angel.err" 2>&1 &
angel_pid=$!
attempt=0
while ! grep -q "Controller started" "$work/controller.log" 2>/dev/null && [ "$attempt" -lt 400 ]; do
  running=$(count_link)
  if [ "$running" -gt 20 ]; then
    kill_matching "^$link "
    fail "the symlinked controller started $running processes instead of an angel and one controller"
  fi
  if ! kill -0 "$angel_pid" 2>/dev/null; then fail "the symlinked controller exited during startup"; fi
  sleep 0.05
  attempt=$((attempt + 1))
done
grep -q "Controller started" "$work/controller.log" 2>/dev/null || fail "the symlinked controller did not start"
peak=0
attempt=0
while [ "$attempt" -lt 20 ]; do
  count=$(count_link)
  if [ "$count" -gt "$peak" ]; then peak=$count; fi
  sleep 0.05
  attempt=$((attempt + 1))
done
if [ "$peak" -ne 2 ]; then
  kill_matching "^$link "
  fail "the symlinked controller runs as $peak processes instead of an angel and one controller"
fi
kill -INT "$angel_pid"
attempt=0
while kill -0 "$angel_pid" 2>/dev/null && [ "$attempt" -lt 300 ]; do
  sleep 0.05
  attempt=$((attempt + 1))
done
if kill -0 "$angel_pid" 2>/dev/null; then fail "the symlinked controller did not stop"; fi
wait "$angel_pid" 2>/dev/null || true
angel_pid=

echo "the angel runs one controller when started under another name"
