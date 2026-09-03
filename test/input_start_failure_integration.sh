#!/bin/sh
set -eu

if [ "$#" -ne 4 ]; then
  echo "usage: $0 MistController MistInBuffer MistInRawGenerator timeout" >&2
  exit 2
fi

controller_binary=$1
buffer_binary=$2
raw_binary=$3
timeout_binary=$4
fixture_dir=$(mktemp -d "${TMPDIR:-/tmp}/mist-input-start-failure.XXXXXX")
fixture_id=$(basename -- "$fixture_dir")
fixture_id=${fixture_id##*.}
ipc_root="$fixture_dir/ipc"
mkdir -p "$ipc_root"
controller_pid=
buffer_pid=

cleanup() {
  if [ -n "$buffer_pid" ]; then
    kill -TERM "$buffer_pid" 2>/dev/null || true
    wait "$buffer_pid" 2>/dev/null || true
  fi
  if [ -n "$controller_pid" ]; then
    kill -INT "$controller_pid" 2>/dev/null || true
    wait "$controller_pid" 2>/dev/null || true
  fi
  rm -rf "$fixture_dir"
}
trap cleanup EXIT INT TERM

run_and_capture() {
  output_file=$1
  shift
  set +e
  env TMP="$ipc_root" MIST_CONTROL=1 ATHEIST=1 "$timeout_binary" 10 "$@" >"$output_file" 2>&1
  command_status=$?
  set -e
  if [ "$command_status" -ne 1 ]; then
    echo "expected input startup to exit 1, got $command_status" >&2
    sed -n '1,160p' "$output_file" >&2
    exit 1
  fi
}

port=$((21000 + ($$ % 20000)))
config="$fixture_dir/config.json"
printf '%s\n' \
  "{\"account\":{\"test\":{\"password\":\"098f6bcd4621d373cade4e832627b4f6\"}},\"auto_push\":null,\"bandwidth\":{\"exceptions\":[\"::1\",\"127.0.0.0/8\"]},\"config\":{\"accesslog\":\"LOG\",\"controller\":{\"interface\":\"127.0.0.1\",\"port\":$port,\"username\":null},\"debug\":4,\"defaultStream\":null,\"prometheus\":\"\",\"protocols\":[],\"serverid\":null,\"sessionInputMode\":15,\"sessionOutputMode\":15,\"sessionStreamInfoMode\":1,\"sessionUnspecifiedMode\":0,\"sessionViewerMode\":14,\"tknMode\":15,\"triggers\":{},\"trustedproxy\":[]},\"extwriters\":null,\"jwks\":null,\"push_settings\":{\"maxspeed\":0,\"wait\":3},\"streamkeys\":null,\"streams\":{},\"variables\":null}" \
  >"$config"

TMP="$ipc_root" MIST_CONTROL=1 "$controller_binary" -c "$config" -C r -L "$fixture_dir/controller.log" &
controller_pid=$!
ready=0
attempt=0
while [ "$attempt" -lt 100 ]; do
  if grep -q "Controller started" "$fixture_dir/controller.log" 2>/dev/null; then
    ready=1
    break
  fi
  if ! kill -0 "$controller_pid" 2>/dev/null; then
    break
  fi
  sleep 0.05
  attempt=$((attempt + 1))
done
if [ "$ready" -ne 1 ]; then
  echo "test controller did not become ready" >&2
  sed -n '1,160p' "$fixture_dir/controller.log" >&2
  exit 1
fi

online_stream="fa${fixture_id}online"
env TMP="$ipc_root" MIST_CONTROL=1 ATHEIST=1 "$buffer_binary" -s "$online_stream" \
  "push://INTERNAL_ONLY:test" >"$fixture_dir/buffer.log" 2>&1 &
buffer_pid=$!

ready=0
attempt=0
while [ "$attempt" -lt 100 ]; do
  if grep -q "Input started" "$fixture_dir/buffer.log"; then
    ready=1
    break
  fi
  if ! kill -0 "$buffer_pid" 2>/dev/null; then
    break
  fi
  sleep 0.05
  attempt=$((attempt + 1))
done
if [ "$ready" -ne 1 ]; then
  echo "buffer fixture did not become ready" >&2
  sed -n '1,160p' "$fixture_dir/buffer.log" >&2
  exit 1
fi

run_and_capture "$fixture_dir/already-online.log" "$raw_binary" -s "$online_stream" rawgenerator:
if ! grep -q "Logging unclean exit reason: Stream already online, cancelling" "$fixture_dir/already-online.log" ||
   ! grep -q "Input closing unclean, reason: Stream already online, cancelling" "$fixture_dir/already-online.log"; then
  echo "online-stream refusal did not retain its machine-readable startup reason" >&2
  sed -n '1,160p' "$fixture_dir/already-online.log" >&2
  exit 1
fi

kill -TERM "$buffer_pid" 2>/dev/null || true
wait "$buffer_pid" 2>/dev/null || true
buffer_pid=

isolated_dir="$fixture_dir/isolated"
mkdir -p "$isolated_dir/lib"
cp "$raw_binary" "$isolated_dir/MistInRawGenerator"
binary_dir=$(CDPATH= cd -- "$(dirname -- "$raw_binary")" && pwd)
for library in "$binary_dir"/lib/libmist.*; do
  if [ -e "$library" ]; then
    ln -s "$library" "$isolated_dir/lib/$(basename -- "$library")"
  fi
done

singular_stream="fa${fixture_id}singular"
run_and_capture "$fixture_dir/no-singular-buffer.log" "$isolated_dir/MistInRawGenerator" -s "$singular_stream" rawgenerator:
if ! grep -q "Logging unclean exit reason: Could not start buffer for '$singular_stream', cancelling" "$fixture_dir/no-singular-buffer.log" ||
   ! grep -q "Input closing unclean, reason: Could not start buffer for '$singular_stream', cancelling" "$fixture_dir/no-singular-buffer.log"; then
  echo "singular buffer startup failure did not retain its machine-readable reason" >&2
  sed -n '1,160p' "$fixture_dir/no-singular-buffer.log" >&2
  exit 1
fi

push_stream="fa${fixture_id}push"
run_and_capture "$fixture_dir/no-push-buffer.log" "$isolated_dir/MistInRawGenerator" --realtime 1 -s "$push_stream" rawgenerator:
if ! grep -q "Logging unclean exit reason: Could not start buffer for '$push_stream', cancelling" "$fixture_dir/no-push-buffer.log" ||
   ! grep -q "Input closing unclean, reason: Could not start buffer for '$push_stream', cancelling" "$fixture_dir/no-push-buffer.log"; then
  echo "non-singular push buffer startup failure did not retain its machine-readable reason" >&2
  sed -n '1,160p' "$fixture_dir/no-push-buffer.log" >&2
  exit 1
fi
