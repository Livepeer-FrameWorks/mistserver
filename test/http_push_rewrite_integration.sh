#!/bin/sh
# A Matroska HTTP PUT whose stream key PUSH_REWRITE maps to another stream name
# is admitted once and answered 200 when its body ends. The PUSH_REWRITE handler
# here admits a publisher only once, as a platform that tracks ingest sessions
# refuses a second admission of the same publisher.
set -eu

if [ "$#" -ne 6 ]; then
  echo "usage: $0 ffmpeg curl MistController MistOutHTTP MistOutEBML MistUtilNuke" >&2
  exit 2
fi
if [ "${MIST_RUN_MEDIA_TESTS:-}" != "1" ]; then
  echo "set MIST_RUN_MEDIA_TESTS=1 to run the Matroska push pipeline" >&2
  exit 77
fi

ffmpeg=$1
curl=$2
controller=$3
output_http=$4
output_ebml=$5
util_nuke=$6
for program in "$ffmpeg" "$curl" "$controller" "$output_http" "$output_ebml" "$util_nuke"; do
  if [ ! -x "$program" ]; then
    echo "required executable is unavailable: $program" >&2
    exit 77
  fi
done
if ! "$ffmpeg" -hide_banner -encoders 2>/dev/null | grep -q 'libx264'; then
  echo "ffmpeg lacks the libx264 encoder required for the publisher fixture" >&2
  exit 77
fi

work=$(mktemp -d "${TMPDIR:-/tmp}/mist-push-rewrite.XXXXXX")
ipc_root="$work/ipc"
mkdir -p "$ipc_root"
stream="pushrewrite$$"
key="streamkey$$"
controller_pid=

cleanup() {
  status=$?
  trap - EXIT HUP INT TERM
  if [ "$status" -ne 0 ]; then
    echo "Matroska push rewrite integration failed; logs follow:" >&2
    for log in "$work"/*.log "$work"/*.txt; do
      if [ -f "$log" ]; then
        echo "Log: $log" >&2
        tail -60 "$log" >&2
      fi
    done
  fi
  if [ -n "$controller_pid" ]; then
    TMP="$ipc_root" MIST_CONTROL=1 "$util_nuke" "$stream" >/dev/null 2>&1 || true
    kill -INT "$controller_pid" >/dev/null 2>&1 || true
    wait "$controller_pid" >/dev/null 2>&1 || true
  fi
  rm -rf -- "$work"
  exit "$status"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

# Each call appends its payload to calls.txt; only the first one is admitted.
rewrite="$work/push_rewrite.sh"
cat >"$rewrite" <<EOF
#!/bin/sh
calls="$work/calls.txt"
seen=0
[ -f "\$calls" ] && seen=1
cat >>"\$calls"
echo >>"\$calls"
echo "--" >>"\$calls"
[ "\$seen" = 1 ] && exit 0
printf '%s' "$stream"
EOF
chmod +x "$rewrite"

source_mkv="$work/source.mkv"
"$ffmpeg" -hide_banner -loglevel error -y \
  -f lavfi -i testsrc2=size=320x180:rate=15:duration=4 \
  -f lavfi -i sine=frequency=440:sample_rate=48000:duration=4 \
  -c:v libx264 -pix_fmt yuv420p -preset veryfast -g 15 -c:a aac -b:a 64k -f matroska "$source_mkv"

api_port=$(python3 "$(dirname "$0")/test_ports.py" 2 $$)
http_port=$((api_port + 1))
config="$work/config.json"
printf '%s\n' \
  "{\"auto_push\":null,\"bandwidth\":{\"exceptions\":[\"::1\",\"127.0.0.0/8\"]},\"config\":{\"accesslog\":\"LOG\",\"controller\":{\"interface\":\"127.0.0.1\",\"port\":$api_port,\"username\":null},\"debug\":4,\"defaultStream\":null,\"prometheus\":\"\",\"protocols\":[{\"connector\":\"HTTP\",\"interface\":\"127.0.0.1\",\"port\":$http_port},{\"connector\":\"EBML\"}],\"serverid\":null,\"sessionInputMode\":15,\"sessionOutputMode\":15,\"sessionStreamInfoMode\":1,\"sessionUnspecifiedMode\":0,\"sessionViewerMode\":14,\"tknMode\":15,\"triggers\":{\"PUSH_REWRITE\":[{\"handler\":\"$rewrite\",\"sync\":true,\"default\":\"\"}]},\"trustedproxy\":[]},\"extwriters\":null,\"jwks\":null,\"push_settings\":{\"maxspeed\":0,\"wait\":3},\"streamkeys\":null,\"streams\":{\"$stream\":{\"name\":\"$stream\",\"source\":\"push://\"}},\"variables\":null}" \
  >"$config"

TMP="$ipc_root" MIST_CONTROL=1 "$controller" -c "$config" -C r -L "$work/controller.log" &
controller_pid=$!

# wait_for <seconds> <description> <command...>: polls until the command succeeds.
wait_for() {
  seconds=$1
  description=$2
  shift 2
  deadline=$(($(date +%s) + seconds))
  until "$@"; do
    if [ "$(date +%s)" -gt "$deadline" ]; then
      echo "timed out waiting for $description" >&2
      exit 1
    fi
    sleep 0.2
  done
}
connectors_started() {
  started=$(grep -c 'Started connector' "$work/controller.log" 2>/dev/null || true)
  [ "${started:-0}" -ge 1 ]
}
wait_for 30 "the test controller and its HTTP connector" connectors_started
http_ready() { "$curl" -s -o /dev/null "http://127.0.0.1:$http_port/"; }
wait_for 10 "the HTTP connector to accept connections" http_ready

# A chunked upload paced like a live publisher, so the body arrives over many reads.
"$ffmpeg" -hide_banner -loglevel error -re -i "$source_mkv" -c copy -f matroska - |
  "$curl" -s --max-time 60 -T - -o "$work/response-body.txt" -w '%{http_code}' \
    "http://127.0.0.1:$http_port/$key.mkv" >"$work/status.txt" || true

calls=$(grep -c -- '^--$' "$work/calls.txt" 2>/dev/null || true)
status=$(cat "$work/status.txt")
if [ "${calls:-0}" -ne 1 ]; then
  echo "the push ran PUSH_REWRITE ${calls:-0} times, not once; Mist admitted the same request again when its body ended" >&2
  exit 1
fi
if [ "$status" != "200" ]; then
  echo "the finished push got HTTP $status ($(cat "$work/response-body.txt" 2>/dev/null)), not 200" >&2
  exit 1
fi
if ! grep -q "$key" "$work/calls.txt"; then
  echo "PUSH_REWRITE did not receive the stream key from the URL" >&2
  exit 1
fi
echo "PASS: the rewritten Matroska push was admitted once and answered 200"
