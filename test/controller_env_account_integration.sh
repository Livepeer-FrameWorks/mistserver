#!/bin/sh
# MistController takes its API account from MIST_API_USERNAME/MIST_API_PASSWORD:
# the account is stored like `-a user:password` (MD5 digest, replacing a stale
# one), the variables are gone from the environment of every process the
# controller starts, the password never reaches the log, and -a wins over the
# environment when both are given.
set -eu

if [ "$#" -ne 2 ]; then
  echo "usage: $0 MistController MistOutHTTP" >&2
  exit 2
fi

# The controller starts the HTTP connector from its own directory.
controller_binary=$1
binary_dir=$(CDPATH= cd -- "$(dirname -- "$controller_binary")" && pwd)
if [ ! -x "$binary_dir/$(basename -- "$2")" ]; then
  echo "$2 is not next to $controller_binary" >&2
  exit 2
fi

fixture_dir=$(mktemp -d "${TMPDIR:-/tmp}/mist-env-account.XXXXXX")
fixture_id=$(basename -- "$fixture_dir")
fixture_id=${fixture_id##*.}
ipc_root="$fixture_dir/ipc"
mkdir -p "$ipc_root"
controller_pid=

env_password=env-rotated-pw-51c7
env_password_md5=1af83e3ce92d633b5135e6d907034fcc
ignored_password=env-ignored-pw-9d2e
old_password_md5=1f839e097163424c1965065b3f6adf6b
cli_password=ops-cli-pw
cli_password_md5=1246dd9136d02b8d976ff05afee2152b
marker="MIST_ENV_ACCOUNT_TEST=$fixture_id"

cleanup() {
  if [ -n "$controller_pid" ]; then
    kill -INT "$controller_pid" 2>/dev/null || true
    wait "$controller_pid" 2>/dev/null || true
  fi
  rm -rf "$fixture_dir"
}
trap cleanup EXIT INT TERM

fail() {
  echo "$1" >&2
  if [ -f "$fixture_dir/controller.log" ]; then sed -n '1,200p' "$fixture_dir/controller.log" >&2; fi
  exit 1
}

port=$((21000 + ($$ % 20000)))
http_port=$((port + 1))
config="$fixture_dir/config.json"
printf '%s\n' \
  "{\"account\":{\"frameworks\":{\"password\":\"$old_password_md5\"}},\"config\":{\"controller\":{\"interface\":\"127.0.0.1\",\"port\":$port},\"debug\":4,\"prometheus\":\"\",\"protocols\":[{\"connector\":\"HTTP\",\"interface\":\"127.0.0.1\",\"port\":$http_port}],\"triggers\":{}},\"streams\":{}}" \
  >"$config"

wait_for_log() {
  attempt=0
  while [ "$attempt" -lt 200 ]; do
    if grep -q "$1" "$fixture_dir/controller.log" 2>/dev/null; then return 0; fi
    if ! kill -0 "$controller_pid" 2>/dev/null; then return 1; fi
    sleep 0.05
    attempt=$((attempt + 1))
  done
  return 1
}

stop_controller() {
  kill -INT "$controller_pid" 2>/dev/null || true
  attempt=0
  while kill -0 "$controller_pid" 2>/dev/null && [ "$attempt" -lt 200 ]; do
    sleep 0.05
    attempt=$((attempt + 1))
  done
  if kill -0 "$controller_pid" 2>/dev/null; then fail "controller did not shut down"; fi
  wait "$controller_pid" 2>/dev/null || true
  controller_pid=
}

# Run 1: environment account only, started through the angel process as in production.
env TMP="$ipc_root" "$marker" MIST_API_USERNAME=" frameworks " MIST_API_PASSWORD="$env_password" \
  "$controller_binary" -c "$config" -L "$fixture_dir/controller.log" &
controller_pid=$!
wait_for_log "Controller started" || fail "controller did not become ready"
grep -q "API account 'frameworks' set from MIST_API_USERNAME/MIST_API_PASSWORD" "$fixture_dir/controller.log" ||
  fail "controller did not report applying the environment account"

if [ -r /proc/self/environ ]; then
  # Every process started from the controller carries the marker; wait for the
  # HTTP connector the config asks for, then inspect them all.
  connector_seen=0
  attempt=0
  while [ "$attempt" -lt 200 ] && [ "$connector_seen" -eq 0 ]; do
    for environ in /proc/[0-9]*/environ; do
      pid_dir=${environ%/environ}
      if ! tr '\0' '\n' <"$environ" 2>/dev/null | grep -qx "$marker"; then continue; fi
      if [ "$(cat "$pid_dir/comm" 2>/dev/null)" = "MistOutHTTP" ]; then connector_seen=1; fi
    done
    [ "$connector_seen" -eq 1 ] || sleep 0.05
    attempt=$((attempt + 1))
  done
  [ "$connector_seen" -eq 1 ] || fail "controller did not start its HTTP connector"

  checked=0
  for environ in /proc/[0-9]*/environ; do
    pid_dir=${environ%/environ}
    vars=$(tr '\0' '\n' <"$environ" 2>/dev/null) || continue
    if ! printf '%s\n' "$vars" | grep -qx "$marker"; then continue; fi
    name=$(cat "$pid_dir/comm" 2>/dev/null || echo "?")
    if printf '%s\n' "$vars" | grep -q "$env_password"; then
      fail "${pid_dir#/proc/} ($name) still holds the API password in its environment"
    fi
    if [ "$name" != "MistController" ] && printf '%s\n' "$vars" | grep -q '^MIST_API_'; then
      fail "${pid_dir#/proc/} ($name) inherited a MIST_API_ variable"
    fi
    checked=$((checked + 1))
  done
  # Angel, controller and connector at least.
  [ "$checked" -ge 3 ] || fail "expected at least 3 controller processes to inspect, found $checked"
else
  echo "no /proc environ support; skipping child environment checks"
fi

stop_controller
grep -q "$env_password_md5" "$config" || fail "config does not hold the environment account digest"
if grep -q "$old_password_md5" "$config"; then fail "stale account digest was not replaced"; fi
if grep -q "$env_password" "$config"; then fail "config holds the plaintext password"; fi
if grep -q "$env_password" "$fixture_dir/controller.log"; then fail "log contains the API password"; fi

# Run 2: -a wins over the environment.
rm -f "$fixture_dir/controller.log"
env TMP="$ipc_root" "$marker" ATHEIST=1 MIST_API_PASSWORD="$ignored_password" \
  "$controller_binary" -c "$config" -L "$fixture_dir/controller.log" -a "ops:$cli_password" &
controller_pid=$!
wait_for_log "Controller started" || fail "controller did not become ready with -a"
grep -q "Ignoring MIST_API_USERNAME/MIST_API_PASSWORD: an account was given with -a" "$fixture_dir/controller.log" ||
  fail "controller did not report ignoring the environment account"
stop_controller
grep -q "$cli_password_md5" "$config" || fail "config does not hold the -a account digest"
grep -q "$env_password_md5" "$config" || fail "environment account digest was changed although -a was given"
if grep -q "f5a2f650bc42429fbb41f9824f275035" "$config"; then fail "environment account applied although -a was given"; fi
if grep -q "$ignored_password" "$fixture_dir/controller.log"; then fail "log contains the API password"; fi

echo "environment API account applied, scrubbed from children, and overridden by -a"
