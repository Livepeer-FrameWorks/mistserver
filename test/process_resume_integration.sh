#!/bin/sh
# Processes on a live RTMP stream are killed and restarted by the buffer. Each restarted process
# continues its own output tracks: the AV audio encoder (which registers before its encoder
# configuration is known) and the thumbnailer's sprite, VTT and preview tracks keep their track
# indexes, so the stream's track list does not change across the restarts.
set -eu

if [ "$#" -ne 8 ]; then
  echo "usage: $0 ffmpeg ffprobe MistController MistInBuffer MistOutRTMP MistProcAV MistProcThumbs MistUtilNuke" >&2
  exit 2
fi
if [ "${MIST_RUN_MEDIA_TESTS:-}" != "1" ]; then
  echo "set MIST_RUN_MEDIA_TESTS=1 to run the live process resumption pipeline" >&2
  exit 77
fi

ffmpeg=$1
ffprobe=$2
controller=$3
input_buffer=$4
output_rtmp=$5
process_av=$6
process_thumbs=$7
util_nuke=$8
for program in "$ffmpeg" "$ffprobe" "$controller" "$input_buffer" "$output_rtmp" "$process_av" "$process_thumbs" \
  "$util_nuke"; do
  if [ ! -x "$program" ]; then
    echo "required executable is unavailable: $program" >&2
    exit 77
  fi
done
if ! "$ffmpeg" -hide_banner -encoders 2>/dev/null | grep -q 'libx264'; then
  echo "ffmpeg lacks the libx264 encoder required for the publisher fixture" >&2
  exit 77
fi

work=$(mktemp -d "${TMPDIR:-/tmp}/mist-process-resume.XXXXXX")
ipc_root="$work/ipc"
mkdir -p "$ipc_root"
stream="procresume$$"
controller_pid=
publisher_pid=

cleanup() {
  status=$?
  trap - EXIT HUP INT TERM
  if [ "$status" -ne 0 ]; then
    echo "live process resumption integration failed; logs follow:" >&2
    grep -E 'track|Track|process|Process' "$work/controller.log" | grep -vE 'Type mismatch|Codec mismatch|bootmsoffset' | \
      tail -80 >&2 || true
    tail -n 3 "$work/track-lists" >&2 || true
  fi
  if [ -n "$publisher_pid" ]; then
    kill -TERM "$publisher_pid" >/dev/null 2>&1 || true
    wait "$publisher_pid" >/dev/null 2>&1 || true
  fi
  if [ -n "$controller_pid" ]; then
    TMP="$ipc_root" MIST_CONTROL=1 "$util_nuke" "$stream" >/dev/null 2>&1 || true
    kill -INT "$controller_pid" >/dev/null 2>&1 || true
    wait "$controller_pid" >/dev/null 2>&1 || true
  fi
  rm -rf -- "/tmp/mist_thumbs/$stream"
  if [ "${MIST_KEEP_TEST_ARTIFACTS:-}" = "1" ]; then
    echo "preserved test artifacts in $work" >&2
  else
    rm -rf -- "$work"
  fi
  exit "$status"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

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
    sleep 0.1
  done
}
logged() { grep -qE -- "$2" "$1" 2>/dev/null; }

track_lists="$work/track-lists"
handler="$work/track-list-handler.sh"
cat >"$handler" <<EOF
#!/bin/sh
printf 'LIST %s\n' "\$(tr '\n' ' ')" >>"$track_lists"
EOF
chmod +x "$handler"
: >"$track_lists"

api_port=$((23000 + ($$ % 9000)))
rtmp_port=$((api_port + 1))
config="$work/config.json"
printf '%s\n' \
  "{\"account\":{\"test\":{\"password\":\"098f6bcd4621d373cade4e832627b4f6\"}},\"auto_push\":null,\"bandwidth\":{\"exceptions\":[\"::1\",\"127.0.0.0/8\"]},\"config\":{\"accesslog\":\"LOG\",\"controller\":{\"interface\":\"127.0.0.1\",\"port\":$api_port,\"username\":null},\"debug\":4,\"defaultStream\":null,\"prometheus\":\"\",\"protocols\":[{\"connector\":\"RTMP\",\"interface\":\"127.0.0.1\",\"port\":$rtmp_port}],\"serverid\":null,\"sessionInputMode\":15,\"sessionOutputMode\":15,\"sessionStreamInfoMode\":1,\"sessionUnspecifiedMode\":0,\"sessionViewerMode\":14,\"tknMode\":15,\"triggers\":{\"LIVE_TRACK_LIST\":[{\"handler\":\"$handler\",\"sync\":false,\"streams\":[\"$stream\"]}]},\"trustedproxy\":[]},\"extwriters\":null,\"jwks\":null,\"push_settings\":{\"maxspeed\":0,\"wait\":3},\"streamkeys\":null,\"streams\":{\"$stream\":{\"name\":\"$stream\",\"source\":\"push://\",\"processes\":[{\"process\":\"AV\",\"x-LSP-kind\":\"audio\",\"codec\":\"opus\",\"track_select\":\"audio=AAC&video=none\",\"restart_type\":\"fixed\"},{\"process\":\"Thumbs\",\"track_select\":\"video=H264&audio=none\",\"thumb_width\":80,\"thumb_height\":80,\"grid_cols\":3,\"grid_rows\":2,\"jpeg_quality\":80,\"interval\":2000,\"restart_type\":\"fixed\"}]}},\"variables\":null}" \
  >"$config"

TMP="$ipc_root" MIST_CONTROL=1 "$controller" -c "$config" -C r -L "$work/controller.log" &
controller_pid=$!
wait_for 30 "the test controller and its RTMP connector" logged "$work/controller.log" 'Started connector'

"$ffmpeg" -hide_banner -loglevel error -re \
  -f lavfi -i "testsrc2=size=320x180:rate=25:duration=300" \
  -f lavfi -i "sine=frequency=997:sample_rate=48000:duration=300" \
  -c:v libx264 -pix_fmt yuv420p -preset ultrafast -g 50 -keyint_min 50 -bf 0 \
  -c:a aac -b:a 96k -f flv "rtmp://127.0.0.1:$rtmp_port/live/$stream" \
  >"$work/publisher.log" 2>&1 &
publisher_pid=$!
wait_for 20 "the stream to become active" logged "$work/controller.log" "Stream $stream became active"

pid_of() { pgrep -f "MistProc$1.*$stream" | head -n 1; }
listed() { tail -n 1 "$track_lists" | grep -q "$1"; }
restarted() { [ -n "$(pid_of "$1")" ] && [ "$(pid_of "$1")" != "$2" ]; }
wait_for 40 "the opus output" listed '"codec":"opus"'
wait_for 40 "the thumbnail outputs" listed '"codec":"thumbvtt"'
sleep 4
tracks_before=$(tail -n 1 "$track_lists" | grep -oE '"idx":[0-9]+' | sort | tr '\n' ' ')

for kill_round in 1 2; do
  for proc in AV Thumbs; do
    pid=$(pid_of "$proc")
    kill -KILL "$pid"
    wait_for 20 "MistProc$proc to be restarted" restarted "$proc" "$pid"
  done
  sleep 8
done

tracks_after=$(tail -n 1 "$track_lists" | grep -oE '"idx":[0-9]+' | sort | tr '\n' ' ')
if [ "$tracks_after" != "$tracks_before" ]; then
  echo "the track list changed across process restarts: [$tracks_before] -> [$tracks_after]" >&2
  exit 1
fi
resumed=$(grep -c 'Resuming track [0-9]* (output ' "$work/controller.log" || true)
if [ "$resumed" -lt 8 ]; then
  echo "restarted processes resumed $resumed outputs by key; expected the opus track and three thumbnail tracks twice" >&2
  exit 1
fi
echo "two restarts of AV and Thumbs kept the track list [$tracks_after]"
