#!/bin/sh
set -eu

if [ "$#" -ne 11 ]; then
  echo "usage: $0 ffmpeg ffprobe MistController MistInMP4 MistInEBML MistInBuffer MistProcThumbs MistOutThumbVTT MistUtilNuke timeout trigger-handler" >&2
  exit 2
fi
if [ "${MIST_RUN_MEDIA_TESTS:-}" != "1" ]; then
  echo "set MIST_RUN_MEDIA_TESTS=1 to run the isolated thumbnail pipeline" >&2
  exit 77
fi

ffmpeg=$1
ffprobe=$2
controller=$3
input_mp4=$4
input_ebml=$5
input_buffer=$6
process_thumbs=$7
output_thumbvtt=$8
util_nuke=$9
timeout_program=${10}
trigger_handler=${11}
for program in "$ffmpeg" "$ffprobe" "$controller" "$input_mp4" "$input_ebml" "$input_buffer" "$process_thumbs" \
  "$output_thumbvtt" "$util_nuke" "$timeout_program" "$trigger_handler"; do
  if [ ! -x "$program" ]; then
    echo "required executable is unavailable: $program" >&2
    exit 77
  fi
done

work=$(mktemp -d "${TMPDIR:-/tmp}/mist-thumbnail-vod.XXXXXX")
ipc_root="$work/ipc"
mkdir -p "$ipc_root"
stream="thumbvod$$"
artifact_dir="/tmp/mist_thumbs/$stream"
trigger_base="$work/trigger"
trigger_file="$trigger_base.THUMBNAIL_UPDATED"
export MIST_TEST_TRIGGER_OUTPUT="$trigger_base"
controller_pid=
input_pid=

cleanup() {
  status=$?
  trap - EXIT HUP INT TERM
  if [ "$status" -ne 0 ]; then
    echo "thumbnail VOD integration failed; logs follow:" >&2
    for log in "$work"/*.log; do
      if [ -f "$log" ]; then
        echo "Log: $log" >&2
        tail -150 "$log" >&2
      fi
    done
  fi
  if [ -n "$controller_pid" ]; then
    TMP="$ipc_root" MIST_CONTROL=1 "$util_nuke" "$stream" >/dev/null 2>&1 || true
  fi
  if [ -n "$input_pid" ]; then
    kill -TERM "$input_pid" >/dev/null 2>&1 || true
    wait "$input_pid" >/dev/null 2>&1 || true
  fi
  if [ -n "$controller_pid" ]; then
    kill -INT "$controller_pid" >/dev/null 2>&1 || true
    wait "$controller_pid" >/dev/null 2>&1 || true
  fi
  rm -rf -- "$artifact_dir"
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

source_codec=${MIST_THUMBNAIL_SOURCE_CODEC:-H264}
case "$source_codec" in
  H264)
    source_media="$work/source.mp4"
    source_input=$input_mp4
    expected_codec=h264
    "$ffmpeg" -hide_banner -loglevel error -y \
      -f lavfi -i testsrc2=size=320x180:rate=25:duration=8 \
      -c:v libx264 -pix_fmt yuv420p -preset veryfast -g 250 -keyint_min 250 -sc_threshold 0 \
      -force_key_frames 0,1,2,3,4,5 -an -movflags +faststart "$source_media"
    ;;
  AV1)
    source_media="$work/source.mp4"
    source_input=$input_mp4
    expected_codec=av1
    if "$ffmpeg" -hide_banner -encoders 2>/dev/null | grep -q 'libaom-av1'; then
      "$ffmpeg" -hide_banner -loglevel error -y \
        -f lavfi -i testsrc2=size=320x180:rate=25:duration=8 \
        -c:v libaom-av1 -cpu-used 8 -crf 45 -g 250 -force_key_frames 0,1,2,3,4,5 \
        -an -movflags +faststart "$source_media"
    elif "$ffmpeg" -hide_banner -encoders 2>/dev/null | grep -q 'libsvtav1'; then
      "$ffmpeg" -hide_banner -loglevel error -y \
        -f lavfi -i testsrc2=size=320x180:rate=25:duration=8 \
        -c:v libsvtav1 -preset 11 -crf 45 -g 250 -force_key_frames 0,1,2,3,4,5 \
        -an -movflags +faststart "$source_media"
    else
      echo "no supported AV1 fixture encoder is available" >&2
      exit 77
    fi
    ;;
  JPEG)
    source_media="$work/source.mkv"
    source_input=$input_ebml
    expected_codec=mjpeg
    "$ffmpeg" -hide_banner -loglevel error -y \
      -f lavfi -i testsrc2=size=320x180:rate=1:duration=6 \
      -c:v mjpeg -q:v 5 -pix_fmt yuvj420p -an "$source_media"
    ;;
  *)
    echo "unsupported MIST_THUMBNAIL_SOURCE_CODEC: $source_codec" >&2
    exit 2
    ;;
esac

fixture_bframes=$("$ffprobe" -v error -select_streams v:0 -show_entries stream=has_b_frames \
  -of default=noprint_wrappers=1:nokey=1 "$source_media")
fixture_codec=$("$ffprobe" -v error -select_streams v:0 -show_entries stream=codec_name \
  -of default=noprint_wrappers=1:nokey=1 "$source_media")
fixture_keyframes=$("$ffprobe" -v error -select_streams v:0 -skip_frame nokey -count_frames \
  -show_entries stream=nb_read_frames -of default=noprint_wrappers=1:nokey=1 "$source_media")
if [ "$fixture_codec" != "$expected_codec" ] || [ "$fixture_keyframes" -lt 6 ] || \
   { [ "$source_codec" = "H264" ] && [ "$fixture_bframes" -lt 1 ]; }; then
  echo "invalid thumbnail fixture: codec=$fixture_codec bframes=$fixture_bframes keyframes=$fixture_keyframes" >&2
  exit 1
fi

port=$((26000 + ($$ % 10000)))
config="$work/config.json"
printf '%s\n' \
  "{\"account\":{\"test\":{\"password\":\"098f6bcd4621d373cade4e832627b4f6\"}},\"auto_push\":null,\"bandwidth\":{\"exceptions\":[\"::1\",\"127.0.0.0/8\"]},\"config\":{\"accesslog\":\"LOG\",\"controller\":{\"interface\":\"127.0.0.1\",\"port\":$port,\"username\":null},\"debug\":4,\"defaultStream\":null,\"prometheus\":\"\",\"protocols\":[],\"serverid\":null,\"sessionInputMode\":15,\"sessionOutputMode\":15,\"sessionStreamInfoMode\":1,\"sessionUnspecifiedMode\":0,\"sessionViewerMode\":14,\"tknMode\":15,\"triggers\":{\"THUMBNAIL_UPDATED\":[{\"handler\":\"$trigger_handler\",\"sync\":false,\"streams\":[\"$stream\"]}]},\"trustedproxy\":[]},\"extwriters\":null,\"jwks\":null,\"push_settings\":{\"maxspeed\":0,\"wait\":3},\"streamkeys\":null,\"streams\":{\"$stream\":{\"name\":\"$stream\",\"source\":\"$source_media\",\"process_controlled_realtime\":true,\"realtime_speed\":1,\"processes\":[{\"process\":\"Thumbs\",\"track_select\":\"video=$source_codec\",\"thumb_width\":80,\"thumb_height\":80,\"grid_cols\":3,\"grid_rows\":2,\"jpeg_quality\":80,\"interval\":2000,\"source_mask\":4,\"target_mask\":3,\"restart_type\":\"fixed\"}]}},\"variables\":null}" \
  >"$config"

TMP="$ipc_root" MIST_CONTROL=1 "$controller" -c "$config" -C r -L "$work/controller.log" &
controller_pid=$!
ready=0
attempt=0
while [ "$attempt" -lt 100 ]; do
  if grep -q 'Controller started' "$work/controller.log" 2>/dev/null; then ready=1; break; fi
  if ! kill -0 "$controller_pid" 2>/dev/null; then break; fi
  attempt=$((attempt + 1))
  sleep 0.05
done
if [ "$ready" -ne 1 ]; then
  echo "test controller did not become ready" >&2
  exit 1
fi

TMP="$ipc_root" MIST_CONTROL=1 "$source_input" -r -s "$stream" "$source_media" >"$work/input.log" 2>&1 &
input_pid=$!
attempt=0
while [ "$attempt" -lt 300 ]; do
  if [ -s "$artifact_dir/poster.jpg" ] && [ -s "$artifact_dir/sprite.jpg" ] && \
     [ -s "$artifact_dir/sprite.vtt" ] && [ -s "$trigger_file" ] && \
     [ "$(grep -c -- '-->' "$artifact_dir/sprite.vtt" 2>/dev/null || true)" -ge 3 ]; then break; fi
  attempt=$((attempt + 1))
  sleep 0.1
done

for artifact in poster.jpg sprite.jpg sprite.vtt; do
  if [ ! -s "$artifact_dir/$artifact" ]; then
    echo "thumbnail processor did not publish $artifact" >&2
    exit 1
  fi
done
if find "$artifact_dir" -name '*.tmp.*' -print | grep -q .; then
  echo "thumbnail publisher left staged files visible" >&2
  exit 1
fi
if [ ! -s "$trigger_file" ]; then
  echo "THUMBNAIL_UPDATED trigger was not captured" >&2
  exit 1
fi

if [ "$source_codec" != "JPEG" ]; then
  request="GET /$stream.thumbvtt HTTP/1.0\r\nHost: localhost\r\n\r\n"
  printf '%b' "$request" | "$timeout_program" 10 env TMP="$ipc_root" MIST_CONTROL=1 \
    "$output_thumbvtt" --connection_handler 127.0.0.1 -s "$stream" >"$work/thumbvtt.http" 2>"$work/thumbvtt.log"
  if ! grep -q 'Content-Type: text/vtt' "$work/thumbvtt.http" || ! grep -q '^WEBVTT' "$work/thumbvtt.http"; then
    echo "ThumbVTT output did not return the live WebVTT track" >&2
    exit 1
  fi

  head_request="HEAD /$stream.thumbvtt HTTP/1.0\r\nHost: localhost\r\n\r\n"
  printf '%b' "$head_request" | "$timeout_program" 5 env TMP="$ipc_root" MIST_CONTROL=1 \
    "$output_thumbvtt" --connection_handler 127.0.0.1 -s "$stream" >"$work/thumbvtt.head" 2>"$work/thumbvtt-head.log"
  if ! grep -q 'HTTP/1.0 200 OK' "$work/thumbvtt.head" || \
     ! grep -q 'Content-Type: text/vtt' "$work/thumbvtt.head" || grep -q '^WEBVTT' "$work/thumbvtt.head"; then
    echo "ThumbVTT HEAD response contract is invalid" >&2
    exit 1
  fi

  # The H.264 case owns the realtime multipart transport contract. Keep the
  # AV1 variant focused on decoding and artifact generation so encoder startup
  # time cannot race a redundant live-pacing assertion.
  if [ "$source_codec" = "H264" ]; then
    push_request="GET /$stream.thumbvtt?mode=push HTTP/1.0\r\nHost: localhost\r\n\r\n"
    set +e
    printf '%b' "$push_request" | "$timeout_program" 10 env TMP="$ipc_root" MIST_CONTROL=1 \
      "$output_thumbvtt" --connection_handler 127.0.0.1 -s "$stream" >"$work/thumbvtt.push" 2>"$work/thumbvtt-push.log"
    push_status=$?
    set -e
    if { [ "$push_status" -ne 0 ] && [ "$push_status" -ne 124 ]; } || \
       ! grep -q 'Content-Type: multipart/mixed; boundary=' "$work/thumbvtt.push" || \
       ! grep -q 'Content-Type: text/vtt' "$work/thumbvtt.push" || \
       ! grep -q 'Content-Type: image/jpeg' "$work/thumbvtt.push"; then
      echo "ThumbVTT push response did not contain a VTT/JPEG pair" >&2
      exit 1
    fi
  fi
fi

attempt=0
while [ "$attempt" -lt 150 ]; do
  if [ "$(grep -c -- '-->' "$artifact_dir/sprite.vtt" 2>/dev/null || true)" -eq 6 ]; then break; fi
  attempt=$((attempt + 1))
  sleep 0.1
done

poster_dims=$("$ffprobe" -v error -select_streams v:0 -show_entries stream=width,height -of csv=p=0:s=x \
  "$artifact_dir/poster.jpg")
sprite_dims=$("$ffprobe" -v error -select_streams v:0 -show_entries stream=width,height -of csv=p=0:s=x \
  "$artifact_dir/sprite.jpg")
if [ "$poster_dims" != "80x44" ] || [ "$sprite_dims" != "240x88" ]; then
  echo "unexpected thumbnail geometry: poster=$poster_dims sprite=$sprite_dims" >&2
  exit 1
fi
if [ "$(sed -n '1p' "$artifact_dir/sprite.vtt")" != "WEBVTT" ]; then
  echo "thumbnail manifest lacks WEBVTT header" >&2
  exit 1
fi
cue_count=$(grep -c -- '-->' "$artifact_dir/sprite.vtt")
if [ "$cue_count" -ne 6 ]; then
  echo "thumbnail manifest has $cue_count cues; expected all six source keyframes" >&2
  exit 1
fi
if ! grep -q "/$stream.jpg?track=.*#xywh=160,44,80,44" "$artifact_dir/sprite.vtt"; then
  echo "thumbnail manifest does not cover the final 3x2 sprite cell" >&2
  exit 1
fi

if [ "$(sed -n '1p' "$trigger_file")" != "THUMBNAIL_UPDATED" ] || \
   [ "$(sed -n '2p' "$trigger_file")" != "$stream" ] || \
   [ "$(sed -n '3p' "$trigger_file")" != "$artifact_dir/poster.jpg" ] || \
   [ "$(sed -n '4p' "$trigger_file")" != "$artifact_dir/sprite.jpg" ] || \
   [ "$(sed -n '5p' "$trigger_file")" != "$artifact_dir/sprite.vtt" ]; then
  echo "THUMBNAIL_UPDATED payload does not match the published artifacts" >&2
  exit 1
fi

compose_count=$(grep -c 'Buffered sprite sheet:' "$work/input.log")
if [ "$compose_count" -lt 2 ] || [ "$compose_count" -gt 3 ]; then
  echo "thumbnail interval produced $compose_count sprite encodes; expected 2-3 coalesced generations" >&2
  exit 1
fi

set +e
printf '%s\n' '{}' | "$timeout_program" 5 env TMP="$ipc_root" MIST_CONTROL=1 \
  "$process_thumbs" - >"$work/invalid.log" 2>&1
invalid_status=$?
set -e
if [ "$invalid_status" -ne 2 ]; then
  echo "invalid thumbnail configuration exited $invalid_status; expected 2" >&2
  exit 1
fi
if ! grep -q 'Invalid source in config' "$work/invalid.log"; then
  echo "invalid thumbnail configuration omitted its reason" >&2
  exit 1
fi

set +e
printf '%s\n' '{"source":"oversized","thumb_width":4096,"grid_cols":64}' | \
  "$timeout_program" 5 env TMP="$ipc_root" MIST_CONTROL=1 "$process_thumbs" - \
  >"$work/oversized.log" 2>&1
oversized_status=$?
set -e
if [ "$oversized_status" -ne 2 ] || ! grep -q 'Thumbnail grid exceeds safe geometry limits' "$work/oversized.log"; then
  echo "oversized thumbnail configuration was not rejected safely" >&2
  exit 1
fi
