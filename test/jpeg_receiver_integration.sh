#!/bin/sh
set -eu

if [ "$#" -ne 7 ]; then
  echo "usage: $0 ffmpeg ffprobe MistController MistOutJPEGReceiver MistOutEBML MistUtilNuke timeout" >&2
  exit 2
fi
if [ "${MIST_RUN_MEDIA_TESTS:-}" != "1" ]; then
  echo "set MIST_RUN_MEDIA_TESTS=1 to run the JPEGReceiver pipeline" >&2
  exit 77
fi

ffmpeg=$1
ffprobe=$2
controller=$3
output_jpeg_receiver=$4
output_ebml=$5
util_nuke=$6
timeout_program=$7
for program in "$ffmpeg" "$ffprobe" "$controller" "$output_jpeg_receiver" "$output_ebml" \
  "$util_nuke" "$timeout_program"; do
  if [ ! -x "$program" ]; then
    echo "required executable is unavailable: $program" >&2
    exit 77
  fi
done
if ! "$ffmpeg" -hide_banner -encoders 2>/dev/null | grep -q 'mjpeg'; then
  echo "ffmpeg lacks the MJPEG encoder required for the JPEGReceiver fixture" >&2
  exit 77
fi

work=$(mktemp -d "${TMPDIR:-/tmp}/mist-jpeg-receiver.XXXXXX")
ipc_root="$work/ipc"
mkdir -p "$ipc_root"

# Controller discovers every Mist* executable next to itself by invoking each
# with -j. A full developer build can contain hundreds of binaries, making that
# unrelated discovery take longer than this focused integration test. Stage the
# controller with only the connector and support executables it needs.
controller_dir=$(CDPATH= cd -- "$(dirname -- "$controller")" && pwd)
bin_dir="$work/bin"
mkdir -p "$bin_dir"
for executable in MistController MistInBuffer MistOutJPEGReceiver MistSession MistUtilLog; do
  source_path="$controller_dir/$executable"
  if [ -x "$source_path" ]; then
    ln "$source_path" "$bin_dir/$executable" 2>/dev/null || cp "$source_path" "$bin_dir/$executable"
  fi
done
for runtime_dir in lib subprojects; do
  if [ -d "$controller_dir/$runtime_dir" ]; then
    ln -s "$controller_dir/$runtime_dir" "$bin_dir/$runtime_dir"
  fi
done
controller="$bin_dir/MistController"

stream="jpegreceiver$$"
controller_pid=
feeder_pid=

cleanup() {
  status=$?
  trap - EXIT HUP INT TERM
  if [ "$status" -ne 0 ]; then
    echo "JPEGReceiver integration failed; logs follow:" >&2
    for log in "$work"/*.log; do
      if [ -f "$log" ]; then
        echo "Log: $log" >&2
        tail -120 "$log" >&2
      fi
    done
  fi
  if [ -n "$feeder_pid" ]; then
    kill "$feeder_pid" >/dev/null 2>&1 || true
    wait "$feeder_pid" >/dev/null 2>&1 || true
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

controller_port=$((28000 + ($$ % 4000)))
receiver_port=$((32000 + ($$ % 4000)))
jpeg_pixel_format=${MIST_JPEG_PIXEL_FORMAT:-yuvj420p}
config="$work/config.json"
printf '%s\n' \
  "{\"account\":{\"test\":{\"password\":\"098f6bcd4621d373cade4e832627b4f6\"}},\"auto_push\":null,\"bandwidth\":{\"exceptions\":[\"::1\",\"127.0.0.0/8\"]},\"config\":{\"accesslog\":\"LOG\",\"controller\":{\"interface\":\"127.0.0.1\",\"port\":$controller_port,\"username\":null},\"debug\":4,\"defaultStream\":null,\"prometheus\":\"\",\"protocols\":[{\"connector\":\"JPEGReceiver\",\"interface\":\"127.0.0.1\",\"port\":$receiver_port,\"streamname\":\"$stream\",\"gopsize\":5,\"bitrate\":500000}],\"serverid\":null,\"sessionInputMode\":15,\"sessionOutputMode\":15,\"sessionStreamInfoMode\":1,\"sessionUnspecifiedMode\":0,\"sessionViewerMode\":14,\"tknMode\":15,\"triggers\":{},\"trustedproxy\":[]},\"extwriters\":null,\"jwks\":null,\"push_settings\":{\"maxspeed\":0,\"wait\":3},\"streamkeys\":null,\"streams\":{\"$stream\":{\"name\":\"$stream\",\"source\":\"push://\"}},\"variables\":null}" \
  >"$config"

TMP="$ipc_root" MIST_CONTROL=1 "$controller" -c "$config" -C r -L "$work/controller.log" &
controller_pid=$!

ready=0
attempt=0
while [ "$attempt" -lt 100 ]; do
  if grep -q 'Started connector:.*JPEGReceiver' "$work/controller.log" 2>/dev/null; then
    ready=1
    break
  fi
  if ! kill -0 "$controller_pid" 2>/dev/null; then
    break
  fi
  attempt=$((attempt + 1))
  sleep 0.1
done
if [ "$ready" -ne 1 ]; then
  echo "JPEGReceiver listener did not become ready" >&2
  exit 1
fi
sleep 0.5

"$ffmpeg" -hide_banner -loglevel error -re \
  -f lavfi -i testsrc2=size=320x180:rate=10:duration=15 \
  -c:v mjpeg -q:v 5 -pix_fmt "$jpeg_pixel_format" -f image2pipe \
  "tcp://127.0.0.1:$receiver_port" >"$work/feeder.log" 2>&1 &
feeder_pid=$!

active=0
attempt=0
while [ "$attempt" -lt 100 ]; do
  if grep -q "Stream $stream became active" "$work/controller.log" 2>/dev/null; then
    active=1
    break
  fi
  if ! kill -0 "$feeder_pid" 2>/dev/null; then
    break
  fi
  attempt=$((attempt + 1))
  sleep 0.1
done
if [ "$active" -ne 1 ]; then
  echo "JPEGReceiver stream did not publish usable metadata" >&2
  exit 1
fi

recording="$work/recording.mkv"
TMP="$ipc_root" MIST_CONTROL=1 "$timeout_program" 15 "$output_ebml" -s "$stream" \
  "$recording?duration=3" >"$work/output.log" 2>&1

"$ffmpeg" -hide_banner -loglevel error -i "$recording" -f null - 2>"$work/decode.log"
if [ -s "$work/decode.log" ]; then
  echo "JPEGReceiver recording emitted decoder/demuxer diagnostics" >&2
  exit 1
fi
if grep -q 'Invalid H264 init data received' "$work/controller.log"; then
  echo "JPEGReceiver published invalid H.264 initialization metadata" >&2
  exit 1
fi
"$ffmpeg" -hide_banner -loglevel error -i "$recording" -map 0:v:0 -f framemd5 "$work/frames.md5"
unique_frames=$(awk -F, '!/^#/ {gsub(/[[:space:]]/, "", $6); print $6}' "$work/frames.md5" | \
  sort -u | wc -l | tr -d ' ')

codec=$("$ffprobe" -v error -select_streams v:0 -show_entries stream=codec_name \
  -of default=noprint_wrappers=1:nokey=1 "$recording")
dimensions=$("$ffprobe" -v error -select_streams v:0 -show_entries stream=width,height \
  -of csv=p=0:s=x "$recording")
packets=$("$ffprobe" -v error -select_streams v:0 -count_packets -show_entries stream=nb_read_packets \
  -of default=noprint_wrappers=1:nokey=1 "$recording")
keyframes=$("$ffprobe" -v error -select_streams v:0 -skip_frame nokey -count_frames \
  -show_entries stream=nb_read_frames -of default=noprint_wrappers=1:nokey=1 "$recording")
first_key=$("$ffprobe" -v error -select_streams v:0 -show_packets \
  -show_entries packet=flags -of csv=p=0 "$recording" | sed -n '1p')
if [ "$codec" != "h264" ] || [ "$dimensions" != "320x180" ] || \
   [ "$packets" -lt 25 ] || [ "$unique_frames" -lt 25 ] || \
   [ "$keyframes" -lt 4 ] || [ "$keyframes" -gt 8 ] || \
   [ "${first_key#K}" = "$first_key" ]; then
  echo "invalid JPEGReceiver output: codec=$codec dimensions=$dimensions packets=$packets unique=$unique_frames keyframes=$keyframes first_flags=$first_key" >&2
  exit 1
fi

echo "JPEGReceiver produced $packets clean, unique H.264 frames with $keyframes keyframes"
