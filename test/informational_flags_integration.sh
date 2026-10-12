#!/bin/sh
# Every Mist binary answers --version (and MistController also --help) by
# printing and exiting 0 at once, as a single process, whatever name it was
# started under: its own path, a symlink with another name, or
# /proc/<pid>/exe of a running controller. A controller started under a name
# other than MistController used to take its own angel for a missing parent and
# start another angel, which did the same, without end.
set -eu

if [ "$#" -ne 1 ]; then
  echo "usage: $0 MistController" >&2
  exit 2
fi
binary_dir=$(CDPATH= cd -- "$(dirname -- "$1")" && pwd)
controller_binary=$binary_dir/$(basename -- "$1")

work=$(mktemp -d "${TMPDIR:-/tmp}/mist-informational.XXXXXX")
ipc_root="$work/ipc"
mkdir -p "$ipc_root" "$work/bin"
live_pid=

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
  kill_matching "$work/bin/"
  if [ -n "$live_pid" ]; then
    kill_matching "^/proc/$live_pid/exe"
    kill -INT "$live_pid" 2>/dev/null || true
    attempt=0
    while kill -0 "$live_pid" 2>/dev/null && [ "$attempt" -lt 200 ]; do
      sleep 0.05
      attempt=$((attempt + 1))
    done
    kill -KILL "$live_pid" 2>/dev/null || true
    wait "$live_pid" 2>/dev/null || true
  fi
  rm -rf "$work"
}
trap cleanup EXIT INT TERM

failures=0
fail() {
  echo "FAIL: $1" >&2
  failures=$((failures + 1))
}

# Runs "$@" with its output in $work/out, waits about 10 s for it to exit and
# records the exit code in $run_status (124 when it did not exit) and the most
# processes matching $1 seen at once in $run_peak. Stops early, killing every
# matching process, once more than 20 exist.
run_counted() {
  pattern=$1
  shift
  rm -f "$work/status"
  # The exit status goes through a file: a finished background job stays a
  # zombie that kill -0 still finds until the shell waits for it.
  (
    set +e
    "$@" >"$work/out" 2>&1
    echo $? >"$work/status"
  ) &
  run_pid=$!
  run_peak=0
  run_status=124
  attempt=0
  while [ "$attempt" -lt 1000 ]; do
    count=$(pgrep -f -- "$pattern" 2>/dev/null | wc -l | tr -d ' ')
    if [ "$count" -gt "$run_peak" ]; then run_peak=$count; fi
    if [ "$count" -gt 20 ]; then break; fi
    if [ -s "$work/status" ]; then
      run_status=$(cat "$work/status")
      break
    fi
    sleep 0.01
    attempt=$((attempt + 1))
  done
  if [ "$run_status" -eq 124 ]; then kill_matching "$pattern"; fi
  wait "$run_pid" 2>/dev/null || true
  leftover=$(pgrep -f -- "$pattern" 2>/dev/null | wc -l | tr -d ' ')
  if [ "$leftover" -gt 0 ]; then kill_matching "$pattern"; fi
}

expect_answer() {
  label=$1
  pattern=$2
  text=$3
  if [ "$run_status" -ne 0 ]; then
    fail "$label: exit status $run_status (124: did not exit), up to $run_peak processes at once"
    sed -n '1,20p' "$work/out" >&2
    return
  fi
  if ! grep -q -- "$text" "$work/out"; then
    fail "$label: did not print \"$text\""
    sed -n '1,20p' "$work/out" >&2
  fi
  if [ "$run_peak" -gt 1 ]; then fail "$label: ran as $run_peak processes at once instead of one"; fi
  if [ "$leftover" -gt 0 ]; then fail "$label: left $leftover processes running"; fi
}

# Every binary, started through a symlink so the name differs from the binary's.
# MIST_CONTROL keeps outputs and inputs from starting their log converter.
checked=0
for binary in "$binary_dir"/Mist*; do
  [ -f "$binary" ] && [ -x "$binary" ] || continue
  name=$(basename -- "$binary")
  link="$work/bin/linked-$name"
  ln -s "$binary" "$link"
  run_counted "$link" env TMP="$ipc_root" MIST_CONTROL=1 "$link" --version
  expect_answer "$name --version (as linked-$name)" "$link" "Version:"
  checked=$((checked + 1))
done
if [ "$checked" -eq 0 ]; then fail "no Mist binaries found next to $controller_binary"; fi

# The controller under its own path, for both flags and their short forms.
for flag in --version -v; do
  run_counted "^$controller_binary $flag" env TMP="$ipc_root" "$controller_binary" "$flag"
  expect_answer "MistController $flag" "^$controller_binary $flag" "Version:"
done
for flag in --help -h; do
  run_counted "^$controller_binary $flag" env TMP="$ipc_root" "$controller_binary" "$flag"
  expect_answer "MistController $flag" "^$controller_binary $flag" "Usage"
done

# MistInNDI loads the NDI runtime only when it has to: --help answers without it,
# and --json reports the input only where the runtime loads.
ndi_binary="$binary_dir/MistInNDI"
if [ -x "$ndi_binary" ]; then
  run_counted "^$ndi_binary --help" env TMP="$ipc_root" MIST_CONTROL=1 "$ndi_binary" --help
  expect_answer "MistInNDI --help" "^$ndi_binary --help" "Usage"
  run_counted "^$ndi_binary --json" env TMP="$ipc_root" MIST_CONTROL=1 "$ndi_binary" --json
  if grep -q "runtime library not found" "$work/out"; then
    if [ "$run_status" -eq 0 ] || grep -q '"name":"NDI"' "$work/out"; then
      fail "MistInNDI --json reported the input on a host without the NDI runtime"
    fi
  elif [ "$run_status" -ne 0 ] || ! grep -q '"name":"NDI"' "$work/out"; then
    fail "MistInNDI --json did not report the input (exit status $run_status)"
    sed -n '1,20p' "$work/out" >&2
  fi
fi

# The staging incident: --version through /proc/<pid>/exe of a running controller.
if [ -e /proc/self/exe ]; then
  port=$((21000 + ($$ % 20000)))
  printf '{"config":{"controller":{"interface":"127.0.0.1","port":%d},"debug":4,"prometheus":"","protocols":[]},"streams":{}}\n' \
    "$port" >"$work/config.json"
  TMP="$ipc_root" MIST_CONTROL=1 "$controller_binary" -c "$work/config.json" -C r -L "$work/controller.log" \
    >/dev/null 2>&1 &
  live_pid=$!
  attempt=0
  while ! grep -q "Controller started" "$work/controller.log" 2>/dev/null && [ "$attempt" -lt 400 ]; do
    if ! kill -0 "$live_pid" 2>/dev/null; then break; fi
    sleep 0.05
    attempt=$((attempt + 1))
  done
  if ! grep -q "Controller started" "$work/controller.log" 2>/dev/null; then
    fail "the running controller did not start"
    sed -n '1,40p' "$work/controller.log" >&2 || true
  else
    run_counted "^/proc/$live_pid/exe" env TMP="$ipc_root" "/proc/$live_pid/exe" --version
    expect_answer "/proc/<running controller>/exe --version" "^/proc/$live_pid/exe" "Version:"
    if ! kill -0 "$live_pid" 2>/dev/null; then fail "the running controller stopped"; fi
  fi
fi

if [ "$failures" -ne 0 ]; then
  echo "$failures informational-flag checks failed" >&2
  exit 1
fi
echo "$checked binaries and the controller answered --version/--help as one process and exited"
