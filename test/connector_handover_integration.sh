#!/usr/bin/env bash
# Changing a running connector's settings replaces its process. A stop request
# that reaches the old listener while it waits for connections must still end
# it and release its port, and the replacement must bind that port.
set -eu

if [ "$#" -ne 3 ]; then
  echo "usage: $0 MistController MistOutHTTP MistUtilNuke" >&2
  exit 2
fi
controller=$1
output_http=$2
util_nuke=$3
for program in "$controller" "$output_http" "$util_nuke"; do
  if [ ! -x "$program" ]; then
    echo "required executable is unavailable: $program" >&2
    exit 77
  fi
done

work=$(mktemp -d "${TMPDIR:-/tmp}/mist-connector-handover.XXXXXX")
ipc_root="$work/ipc"
mkdir -p "$ipc_root"
api_port=$((21000 + ($$ % 15000)))
udp_port=$((api_port + 1))
http_port=$((api_port + 2))
controller_pid=

cleanup() {
  if [ -n "$controller_pid" ]; then kill "$controller_pid" 2>/dev/null || true; fi
  sleep 0.5
  pkill -f "$output_http --port $http_port" 2>/dev/null || true
  TMP="$ipc_root" "$util_nuke" >/dev/null 2>&1 || true
  rm -rf "$work"
}
trap cleanup EXIT

old_http="{\"connector\":\"HTTP\",\"port\":$http_port,\"pubaddr\":\"http://localhost:18090/view/\"}"
new_http="{\"connector\":\"HTTP\",\"port\":$http_port,\"pubaddr\":\"http://edge:8082/\"}"
printf '%s\n' \
  "{\"account\":{\"test\":{\"password\":\"098f6bcd4621d373cade4e832627b4f6\"}},\"bandwidth\":{\"exceptions\":[\"::1\",\"127.0.0.0/8\"]},\"config\":{\"controller\":{\"interface\":\"127.0.0.1\",\"port\":$api_port},\"debug\":4,\"protocols\":[$old_http],\"triggers\":{},\"trustedproxy\":[]},\"streams\":{}}" \
  >"$work/config.json"

UDP_API="udp://127.0.0.1:$udp_port" TMP="$ipc_root" MIST_CONTROL=1 \
  "$controller" -c "$work/config.json" -C r -L "$work/controller.log" &
controller_pid=$!

# A TCP connect succeeds only while some listener holds the port.
accepts() { (exec 3<>"/dev/tcp/127.0.0.1/$http_port") 2>/dev/null; }

waited=0
until accepts; do
  waited=$((waited + 1))
  if [ "$waited" -gt 100 ]; then
    echo "the initial HTTP connector never listened" >&2
    exit 1
  fi
  sleep 0.1
done

# The stop arrives while the idle listener is inside its connection wait.
sleep 3

printf '%s' "{\"updateprotocol\":[$old_http,$new_http]}" >"/dev/udp/127.0.0.1/$udp_port"

waited=0
new_bound() {
  awk -v port="$http_port" '
    /Started connector: \{"connector":"HTTP"/ && /edge:8082/ { started = 1 }
    started && /MistOutHTTP/ && /Socket bound to/ && index($0, ":" port) { found = 1 }
    END { exit !found }' "$work/controller.log"
}
until new_bound && accepts; do
  waited=$((waited + 1))
  # The controller kills a listener that outlives its 5 s grace and retries on
  # its 3 s check cycle, so the port must be rebound within 10 s.
  if [ "$waited" -gt 100 ]; then
    echo "the replaced HTTP connector did not bind within 10 s" >&2
    grep -E 'connector|Binding|did not stop' "$work/controller.log" >&2 || true
    exit 1
  fi
  sleep 0.1
done

if grep -q 'Binding .*:'"$http_port"' failed' "$work/controller.log"; then
  echo "the replacement tried to bind while the old listener still held the port" >&2
  grep -E 'connector|Binding' "$work/controller.log" >&2
  exit 1
fi
if pgrep -f "$output_http --port $http_port --public-address http://localhost:18090/view/" >/dev/null 2>&1; then
  echo "the old HTTP listener is still running" >&2
  exit 1
fi
echo "connector replaced and listening after $((waited / 10)) s"
