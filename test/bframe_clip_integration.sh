#!/bin/sh
set -eu

if [ "$#" -ne 8 ]; then
  echo "usage: $0 ffmpeg ffprobe MistController MistInMP4 MistOutMP4 MistSession MistUtilLog MistUtilNuke" >&2
  exit 2
fi

# MistController uses process-global shared-memory pages and a fixed UDP API port. Do not start an
# integration-test controller implicitly on a developer machine that may already run MistServer.
# CI and explicit local media validation opt in to owning those resources for this test.
if [ "${MIST_RUN_MEDIA_TESTS:-}" != "1" ]; then
  echo "set MIST_RUN_MEDIA_TESTS=1 to run the isolated B-frame Controller pipeline" >&2
  exit 77
fi

ffmpeg=$1
ffprobe=$2
controller=$3
input_mp4=$4
output_mp4=$5
session=$6
util_log=$7
util_nuke=$8

for program in "$ffmpeg" "$ffprobe" "$controller" "$input_mp4" "$output_mp4" "$session" "$util_log" "$util_nuke"; do
  if [ ! -x "$program" ]; then
    echo "required executable is unavailable: $program" >&2
    exit 77
  fi
done

if ! "$ffmpeg" -hide_banner -encoders 2>/dev/null | grep -q 'libx264'; then
  echo "ffmpeg lacks the libx264 encoder required for the B-frame fixture" >&2
  exit 77
fi

work=$(mktemp -d "${TMPDIR:-/tmp}/mist-bframe-clip.XXXXXX")
ipc_root="$work/ipc"
mkdir -p "$ipc_root"
stream="bframeclip$$"
reject_stream="bframereject$$"
controller_pid=

show_logs() {
  status=$?
  if [ "$status" -ne 0 ]; then
    echo "B-frame clip integration failed; controller log follows:" >&2
    if [ -f "$work/controller.log" ]; then
      tail -100 "$work/controller.log" >&2
    fi
    for log in "$work"/output-*.log; do
      if [ -f "$log" ]; then
        echo "Output log: $log" >&2
        cat "$log" >&2
      fi
    done
  fi
  if [ -n "$controller_pid" ]; then
    TMP="$ipc_root" MIST_CONTROL=1 "$util_nuke" "$stream" >/dev/null 2>&1 || true
    TMP="$ipc_root" MIST_CONTROL=1 "$util_nuke" "$reject_stream" >/dev/null 2>&1 || true
    kill -INT "$controller_pid" >/dev/null 2>&1 || true
    wait "$controller_pid" >/dev/null 2>&1 || true
  fi
  rm -rf -- "$work"
  exit "$status"
}
trap show_logs EXIT HUP INT TERM

source_mp4="$work/source.mp4"
"$ffmpeg" -hide_banner -loglevel error -y \
  -f lavfi -i testsrc2=size=320x180:rate=25:duration=8 \
  -f lavfi -i sine=frequency=997:sample_rate=48000:duration=8 \
  -c:v libx264 -pix_fmt yuv420p -preset veryfast -g 50 -keyint_min 50 -bf 2 -sc_threshold 0 \
  -c:a aac -b:a 96k -movflags +faststart "$source_mp4"

# A shorter audio track forces the generic reader and MP4 sample table to disagree at the audio
# tail. The output must diagnose and stop that case before audio bytes enter video sample slots.
reject_source_mp4="$work/reject-source.mp4"
"$ffmpeg" -hide_banner -loglevel error -y -i "$source_mp4" -map 0:v:0 -map 0:a:0 \
  -c:v copy -c:a aac -af atrim=duration=4.5 -movflags +faststart "$reject_source_mp4"

if [ "$("$ffprobe" -v error -select_streams v:0 -show_entries stream=has_b_frames -of csv=p=0 "$source_mp4")" -lt 1 ]; then
  echo "generated fixture unexpectedly has no B-frames" >&2
  exit 1
fi

port=$((20000 + ($$ % 20000)))
config="$work/config.json"
printf '%s\n' \
  "{\"account\":{\"test\":{\"password\":\"098f6bcd4621d373cade4e832627b4f6\"}},\"auto_push\":null,\"bandwidth\":{\"exceptions\":[\"::1\",\"127.0.0.0/8\"]},\"config\":{\"accesslog\":\"LOG\",\"controller\":{\"interface\":\"127.0.0.1\",\"port\":$port,\"username\":null},\"debug\":4,\"defaultStream\":null,\"prometheus\":\"\",\"protocols\":[],\"serverid\":null,\"sessionInputMode\":15,\"sessionOutputMode\":15,\"sessionStreamInfoMode\":1,\"sessionUnspecifiedMode\":0,\"sessionViewerMode\":14,\"tknMode\":15,\"triggers\":{},\"trustedproxy\":[]},\"extwriters\":null,\"jwks\":null,\"push_settings\":{\"maxspeed\":0,\"wait\":3},\"streamkeys\":null,\"streams\":{\"$stream\":{\"name\":\"$stream\",\"source\":\"$source_mp4\"},\"$reject_stream\":{\"name\":\"$reject_stream\",\"source\":\"$reject_source_mp4\"}},\"variables\":null}" \
  >"$config"

TMP="$ipc_root" MIST_CONTROL=1 "$controller" -c "$config" -C r -L "$work/controller.log" &
controller_pid=$!

ready=0
attempt=0
while [ "$attempt" -lt 30 ]; do
  if grep -q 'Controller started' "$work/controller.log" 2>/dev/null; then
    ready=1
    break
  fi
  if ! kill -0 "$controller_pid" 2>/dev/null; then
    break
  fi
  attempt=$((attempt + 1))
  sleep 1
done
if [ "$ready" -ne 1 ]; then
  echo "test controller did not become ready" >&2
  exit 1
fi

av_clip="$work/av.mp4"
video_clip="$work/video.mp4"
TMP="$ipc_root" MIST_CONTROL=1 "$output_mp4" -s "$stream" "$av_clip?start=3337&duration=3" \
  >"$work/output-av.log" 2>&1
TMP="$ipc_root" MIST_CONTROL=1 "$output_mp4" -s "$stream" "$video_clip?start=3337&duration=3&audio=none" \
  >"$work/output-video.log" 2>&1

for clip in "$av_clip" "$video_clip"; do
  "$ffmpeg" -hide_banner -loglevel error -i "$clip" -f null -
  duration=$("$ffprobe" -v error -show_entries format=duration -of csv=p=0 "$clip")
  awk -v duration="$duration" 'BEGIN { if (duration < 2.999 || duration > 3.001) exit 1 }' || {
    echo "$clip has duration $duration instead of 3 seconds" >&2
    exit 1
  }
  sample_count=$("$ffprobe" -v error -select_streams v:0 -count_packets \
    -show_entries stream=nb_read_packets -of csv=p=0 "$clip")
  if [ "$sample_count" -ne 76 ]; then
    echo "$clip has $sample_count encoded video samples instead of the expected 75 visible frames plus one decode-tail sample" >&2
    exit 1
  fi
done

if [ "$("$ffprobe" -v error -select_streams a -show_entries stream=index -of csv=p=0 "$av_clip" | wc -l | tr -d ' ')" -ne 1 ]; then
  echo "audio/video clip lost its audio track" >&2
  exit 1
fi
if [ -n "$("$ffprobe" -v error -select_streams a -show_entries stream=index -of csv=p=0 "$video_clip")" ]; then
  echo "video-only clip unexpectedly contains audio" >&2
  exit 1
fi

"$ffmpeg" -hide_banner -loglevel error -ss 2 -t 3 -i "$source_mp4" -map 0:v:0 -f framemd5 "$work/expected.md5"
"$ffmpeg" -hide_banner -loglevel error -i "$av_clip" -map 0:v:0 -f framemd5 "$work/av.md5"
"$ffmpeg" -hide_banner -loglevel error -i "$video_clip" -map 0:v:0 -f framemd5 "$work/video.md5"
awk '!/^#/ { print $NF }' "$work/expected.md5" >"$work/expected.hashes"
awk '!/^#/ { print $NF }' "$work/av.md5" >"$work/av.hashes"
awk '!/^#/ { print $NF }' "$work/video.md5" >"$work/video.hashes"

if [ "$(wc -l <"$work/expected.hashes" | tr -d ' ')" -ne 75 ]; then
  echo "reference cut did not contain exactly 75 displayed frames" >&2
  exit 1
fi
diff -u "$work/expected.hashes" "$work/av.hashes"
diff -u "$work/expected.hashes" "$work/video.hashes"

reject_clip="$work/reject.mp4"
TMP="$ipc_root" MIST_CONTROL=1 "$output_mp4" -s "$reject_stream" \
  "$reject_clip?start=3337&duration=3" >"$work/output-reject.log" 2>&1 || true
if ! grep -q 'Inconsistent MP4 input' "$work/output-reject.log"; then
  echo "mismatched track tails were not rejected before MP4 sample corruption" >&2
  exit 1
fi
"$ffmpeg" -hide_banner -loglevel error -i "$reject_clip" -f null - \
  2>"$work/reject-decode.log" || true
if grep -Eq 'Invalid NAL unit|Error splitting the input' "$work/reject-decode.log"; then
  echo "rejected MP4 contains cross-track H.264 sample corruption" >&2
  exit 1
fi

echo "B-frame MP4 clips preserved 75 visible frames and rejected mismatched track tails"
