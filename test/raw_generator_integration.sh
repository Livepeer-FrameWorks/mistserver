#!/bin/sh
set -eu

if [ "$#" -ne 7 ]; then
  echo "usage: $0 ffmpeg ffprobe MistController MistInRawGenerator MistOutEBML MistSession MistUtilLog" >&2
  exit 2
fi
if [ "${MIST_RUN_MEDIA_TESTS:-}" != "1" ]; then
  echo "set MIST_RUN_MEDIA_TESTS=1 to run the raw-generator pipeline" >&2
  exit 77
fi

ffmpeg=$1
ffprobe=$2
controller=$3
input_raw=$4
output_ebml=$5
session=$6
util_log=$7
for program in "$ffmpeg" "$ffprobe" "$controller" "$input_raw" "$output_ebml" "$session" "$util_log"; do
  if [ ! -x "$program" ]; then
    echo "required executable is unavailable: $program" >&2
    exit 77
  fi
done

capabilities=$($input_raw -j)
if ! printf '%s' "$capabilities" | grep -q '"desc":"Generates raw video and timed subtitles"' ||
   ! printf '%s' "$capabilities" | grep -q '"video":\["UYVY"\]' ||
   ! printf '%s' "$capabilities" | grep -q '"subtitle":\["subtitle"\]' ||
   printf '%s' "$capabilities" | grep -q '"audio"' ||
   printf '%s' "$capabilities" | grep -q '"meta":' ||
   printf '%s' "$capabilities" | grep -q '"metadata"'; then
  echo "RawGenerator advertised capabilities do not match its produced tracks" >&2
  printf '%s\n' "$capabilities" >&2
  exit 1
fi

work=$(mktemp -d "${TMPDIR:-/tmp}/mist-raw-generator.XXXXXX")
ipc_root="$work/ipc"
bin_dir="$work/bin"
mkdir -p "$ipc_root" "$bin_dir"
controller_pid=
stream="rawgenerator$$"

cleanup() {
  status=$?
  trap - EXIT HUP INT TERM
  if [ "$status" -ne 0 ]; then
    echo "RawGenerator integration failed; logs follow:" >&2
    for log in "$work"/*.log; do
      if [ -f "$log" ]; then
        echo "Log: $log" >&2
        tail -100 "$log" >&2
      fi
    done
  fi
  if [ -n "$controller_pid" ]; then
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

stage_executable() {
  source_path=$1
  target_name=$2
  ln "$source_path" "$bin_dir/$target_name" 2>/dev/null || cp "$source_path" "$bin_dir/$target_name"
}
stage_executable "$controller" MistController
stage_executable "$input_raw" MistInRawGenerator
stage_executable "$output_ebml" MistOutEBML
stage_executable "$session" MistSession
stage_executable "$util_log" MistUtilLog

controller_dir=$(CDPATH='' cd -- "$(dirname -- "$controller")" && pwd)
if [ ! -x "$controller_dir/MistInBuffer" ]; then
  echo "required executable is unavailable: $controller_dir/MistInBuffer" >&2
  exit 77
fi
stage_executable "$controller_dir/MistInBuffer" MistInBuffer
for runtime_dir in lib subprojects; do
  if [ -d "$controller_dir/$runtime_dir" ]; then
    ln -s "$controller_dir/$runtime_dir" "$bin_dir/$runtime_dir"
  fi
done
controller="$bin_dir/MistController"
output_ebml="$bin_dir/MistOutEBML"

port=$((24000 + ($$ % 12000)))
config="$work/config.json"
printf '%s\n' \
  "{\"account\":{\"test\":{\"password\":\"098f6bcd4621d373cade4e832627b4f6\"}},\"auto_push\":null,\"bandwidth\":{\"exceptions\":[\"::1\",\"127.0.0.0/8\"]},\"config\":{\"accesslog\":\"LOG\",\"controller\":{\"interface\":\"127.0.0.1\",\"port\":$port,\"username\":null},\"debug\":4,\"defaultStream\":null,\"prometheus\":\"\",\"protocols\":[],\"serverid\":null,\"sessionInputMode\":15,\"sessionOutputMode\":15,\"sessionStreamInfoMode\":1,\"sessionUnspecifiedMode\":0,\"sessionViewerMode\":14,\"tknMode\":15,\"triggers\":{},\"trustedproxy\":[]},\"extwriters\":null,\"jwks\":null,\"push_settings\":{\"maxspeed\":0,\"wait\":3},\"streamkeys\":null,\"streams\":{\"$stream\":{\"name\":\"$stream\",\"source\":\"rawgenerator:\"}},\"variables\":null}" \
  >"$config"

TMP="$ipc_root" MIST_CONTROL=1 "$controller" -c "$config" -C r -L "$work/controller.log" &
controller_pid=$!

ready=0
attempt=0
while [ "$attempt" -lt 100 ]; do
  if grep -q 'Controller started' "$work/controller.log" 2>/dev/null; then
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
  echo "RawGenerator test controller did not become ready" >&2
  exit 1
fi

recording="$work/recording.mkv"
TMP="$ipc_root" MIST_CONTROL=1 "$output_ebml" -s "$stream" \
  "$recording?duration=3" >"$work/output.log" 2>&1

"$ffmpeg" -hide_banner -loglevel error -i "$recording" -f null - 2>"$work/decode.log"
if [ -s "$work/decode.log" ]; then
  echo "RawGenerator recording emitted decoder/demuxer diagnostics" >&2
  exit 1
fi

codec=$($ffprobe -v error -select_streams v:0 -show_entries stream=codec_name \
  -of default=noprint_wrappers=1:nokey=1 "$recording")
pixel_format=$($ffprobe -v error -select_streams v:0 -show_entries stream=pix_fmt \
  -of default=noprint_wrappers=1:nokey=1 "$recording")
dimensions=$($ffprobe -v error -select_streams v:0 -show_entries stream=width,height \
  -of csv=p=0:s=x "$recording")
frame_rate=$($ffprobe -v error -select_streams v:0 -show_entries stream=r_frame_rate \
  -of default=noprint_wrappers=1:nokey=1 "$recording")
packets=$($ffprobe -v error -select_streams v:0 -count_packets -show_entries stream=nb_read_packets \
  -of default=noprint_wrappers=1:nokey=1 "$recording")
packet_sizes=$($ffprobe -v error -select_streams v:0 -show_entries packet=size \
  -of csv=p=0 "$recording" | sort -u)
first_pts=$($ffprobe -v error -select_streams v:0 -read_intervals '%+#1' \
  -show_entries packet=pts_time -of csv=p=0 "$recording")
first_frame="$work/first-frame.uyvy"
"$ffmpeg" -hide_banner -loglevel error -y -i "$recording" -map 0:v:0 -frames:v 1 \
  -f rawvideo "$first_frame"
first_frame_size=$(wc -c <"$first_frame" | tr -d ' ')
nonzero_bytes=$(LC_ALL=C tr -d '\000' <"$first_frame" | wc -c | tr -d ' ')
first_pts_valid=$(awk -v pts="$first_pts" 'BEGIN { print (pts >= 0) ? 1 : 0 }')

if [ "$codec" != "rawvideo" ] || [ "$pixel_format" != "uyvy422" ] ||
   [ "$dimensions" != "800x600" ] || [ "$frame_rate" != "50/1" ] ||
   [ "$packets" -lt 45 ] || [ "$packets" -gt 155 ] ||
   [ "$packet_sizes" != "960000" ] || [ "$first_pts_valid" -ne 1 ] ||
   [ "$first_frame_size" -ne 960000 ] ||
   [ "$nonzero_bytes" -ne 0 ]; then
  echo "invalid RawGenerator output: codec=$codec format=$pixel_format dimensions=$dimensions rate=$frame_rate packets=$packets sizes=$packet_sizes first_pts=$first_pts first_frame=$first_frame_size nonzero=$nonzero_bytes" >&2
  exit 1
fi

echo "RawGenerator produced $packets decoder-clean 800x600 UYVY frames"
