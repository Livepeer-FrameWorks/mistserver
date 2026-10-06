#!/bin/sh
# A controller told to stop while it is still starting up (here: while it
# checks the available protocols) stops once startup is done, instead of
# running on as if the signal never arrived.
set -eu

if [ "$#" -ne 1 ]; then
  echo "usage: $0 MistController" >&2
  exit 2
fi
controller_binary=$1

work=$(mktemp -d "${TMPDIR:-/tmp}/mist-controller-startup-stop.XXXXXX")
ipc_root="$work/ipc"
mkdir -p "$ipc_root"
controller_pid=

cleanup() {
  if [ -n "$controller_pid" ] && kill -0 "$controller_pid" 2>/dev/null; then
    kill -KILL "$controller_pid" 2>/dev/null || true
    wait "$controller_pid" 2>/dev/null || true
  fi
  rm -rf "$work"
}
trap cleanup EXIT INT TERM

port=$((21000 + ($$ % 20000)))
printf '{"config":{"controller":{"interface":"127.0.0.1","port":%d},"debug":4,"protocols":[]},"streams":{}}\n' "$port" \
  >"$work/config.json"

ATHEIST=1 TMP="$ipc_root" MIST_CONTROL=1 "$controller_binary" -c "$work/config.json" -C r -L "$work/controller.log" \
  >/dev/null 2>&1 &
controller_pid=$!

# The log file exists once the controller set up its logging, which it does
# after installing its signal handlers and before it checks the protocols.
attempt=0
while [ ! -s "$work/controller.log" ] && [ "$attempt" -lt 500 ]; do
  if ! kill -0 "$controller_pid" 2>/dev/null; then
    echo "controller exited before it started logging" >&2
    exit 1
  fi
  sleep 0.01
  attempt=$((attempt + 1))
done
if grep -q "Controller started" "$work/controller.log"; then
  echo "controller finished starting before the stop request; nothing was tested" >&2
  exit 77
fi
kill -INT "$controller_pid"

attempt=0
while kill -0 "$controller_pid" 2>/dev/null && [ "$attempt" -lt 300 ]; do
  sleep 0.05
  attempt=$((attempt + 1))
done
if kill -0 "$controller_pid" 2>/dev/null; then
  echo "controller kept running 15 s after a stop request during startup" >&2
  exit 1
fi
wait "$controller_pid" 2>/dev/null || true
controller_pid=
echo "controller stopped after a stop request during startup"
