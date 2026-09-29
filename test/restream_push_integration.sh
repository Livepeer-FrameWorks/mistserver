#!/bin/sh
set -eu

if [ "$#" -ne 9 ]; then
  echo "usage: $0 ffmpeg ffprobe curl MistController MistInBuffer MistOutRTMP MistSession MistUtilNuke timeout" >&2
  exit 2
fi
if [ "${MIST_RUN_MEDIA_TESTS:-}" != "1" ]; then
  echo "set MIST_RUN_MEDIA_TESTS=1 to run the RTMP restream fixture" >&2
  exit 77
fi

ffmpeg=$1
ffprobe=$2
curl=$3
controller=$4
input_buffer=$5
output_rtmp=$6
session=$7
util_nuke=$8
timeout_program=$9
for program in "$ffmpeg" "$ffprobe" "$curl" "$controller" "$input_buffer" "$output_rtmp" "$session" "$util_nuke" "$timeout_program"; do
  if [ ! -x "$program" ]; then
    echo "required executable is unavailable: $program" >&2
    exit 77
  fi
done

work=$(mktemp -d "${TMPDIR:-/tmp}/mist-restream-push.XXXXXX")
ipc_root="$work/ipc"
mkdir -p "$ipc_root"
source_stream="restreamsource$$"
sink_stream="restreamsink$$"
failed_stream="restreamfailed$$"
controller_pid=
publisher_pid=
push_end_handler="$work/push_end.sh"
printf '%s\n' '#!/bin/sh' "cat >>'$work/push_end.log'" >"$push_end_handler"
chmod +x "$push_end_handler"
cleanup() {
  result=$?
  trap - EXIT HUP INT TERM
  if [ "$result" -ne 0 ]; then
    echo "RTMP restream integration failed; logs follow:" >&2
    for log in "$work"/*.log; do
      if [ -f "$log" ]; then
        echo "Log: $log" >&2
        tail -100 "$log" >&2
      fi
    done
  fi
  if [ -n "$publisher_pid" ]; then
    kill -TERM "$publisher_pid" >/dev/null 2>&1 || true
    wait "$publisher_pid" >/dev/null 2>&1 || true
  fi
  if [ -n "$controller_pid" ]; then
    TMP="$ipc_root" MIST_CONTROL=1 "$util_nuke" "$source_stream" >/dev/null 2>&1 || true
    TMP="$ipc_root" MIST_CONTROL=1 "$util_nuke" "$sink_stream" >/dev/null 2>&1 || true
    TMP="$ipc_root" MIST_CONTROL=1 "$util_nuke" "$failed_stream" >/dev/null 2>&1 || true
    kill -INT "$controller_pid" >/dev/null 2>&1 || true
    wait "$controller_pid" >/dev/null 2>&1 || true
  fi
  if [ "${MIST_KEEP_TEST_ARTIFACTS:-}" = "1" ]; then
    echo "preserved test artifacts in $work" >&2
  else
    rm -rf -- "$work"
  fi
  exit "$result"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

api_port=$((26000 + ($$ % 12000)))
rtmp_port=$((api_port + 1))
printf '%s\n' \
  "{\"account\":{\"test\":{\"password\":\"098f6bcd4621d373cade4e832627b4f6\"}},\"auto_push\":null,\"bandwidth\":{\"exceptions\":[\"::1\",\"127.0.0.0/8\"]},\"config\":{\"accesslog\":\"LOG\",\"controller\":{\"interface\":\"127.0.0.1\",\"port\":$api_port,\"username\":null},\"debug\":4,\"defaultStream\":null,\"prometheus\":\"\",\"protocols\":[{\"connector\":\"RTMP\",\"interface\":\"127.0.0.1\",\"port\":$rtmp_port}],\"serverid\":null,\"sessionInputMode\":15,\"sessionOutputMode\":15,\"sessionStreamInfoMode\":1,\"sessionUnspecifiedMode\":0,\"sessionViewerMode\":14,\"tknMode\":15,\"triggers\":{\"PUSH_END\":[{\"handler\":\"$push_end_handler\",\"sync\":false,\"streams\":[\"$source_stream\"]}]},\"trustedproxy\":[]},\"extwriters\":null,\"jwks\":null,\"push_settings\":{\"maxspeed\":0,\"wait\":0},\"streamkeys\":null,\"streams\":{\"$source_stream\":{\"name\":\"$source_stream\",\"source\":\"push://\",\"resume\":1,\"inputtimeout\":6},\"$sink_stream\":{\"name\":\"$sink_stream\",\"source\":\"push://\",\"resume\":1,\"inputtimeout\":6},\"$failed_stream\":{\"name\":\"$failed_stream\",\"source\":\"push://\",\"resume\":1,\"inputtimeout\":6}},\"variables\":null}" \
  >"$work/config.json"

TMP="$ipc_root" MIST_CONTROL=1 "$controller" -c "$work/config.json" -C r -L "$work/controller.log" &
controller_pid=$!
wait_for_log() {
  pattern=$1
  limit=$2
  attempt=0
  while [ "$attempt" -lt "$limit" ]; do
    if grep -q "$pattern" "$work/controller.log" 2>/dev/null; then return 0; fi
    if ! kill -0 "$controller_pid" 2>/dev/null; then return 1; fi
    attempt=$((attempt + 1))
    sleep 0.25
  done
  return 1
}
if ! wait_for_log 'Controller started' 120 || ! wait_for_log 'Started connector' 120; then
  echo "RTMP controller did not start" >&2
  exit 1
fi

"$timeout_program" 60 "$ffmpeg" -hide_banner -loglevel error -re \
  -f lavfi -i 'testsrc2=size=640x360:rate=25' \
  -f lavfi -i 'sine=frequency=997:sample_rate=48000' \
  -c:v libx264 -pix_fmt yuv420p -preset ultrafast -g 25 -keyint_min 25 -bf 0 \
  -c:a aac -b:a 96k -flvflags no_metadata -f flv "rtmp://127.0.0.1:$rtmp_port/live/$source_stream" \
  >"$work/publisher.log" 2>&1 &
publisher_pid=$!

stream_live() {
  stream=$1
  "$curl" -s --max-time 3 "http://127.0.0.1:$api_port/api2" \
    --data-urlencode 'command={"active_streams":["tracks","status"]}' 2>/dev/null |
    sed -n "s/.*\"$stream\":\[\([0-9]*\),\"\([^\"]*\)\"\].*/\1 \2/p" |
    awk '$1 >= 2 && $2 == "Online" { found = 1 } END { exit !found }'
}
wait_for_stream() {
  stream=$1
  limit=$2
  attempt=0
  while [ "$attempt" -lt "$limit" ]; do
    if stream_live "$stream"; then return 0; fi
    attempt=$((attempt + 1))
    sleep 0.25
  done
  return 1
}
if ! wait_for_stream "$source_stream" 60; then
  echo "source publisher did not become live" >&2
  exit 1
fi

api_command() {
  "$curl" -s --max-time 5 "http://127.0.0.1:$api_port/api2" --data-urlencode "command=$1"
}
start_seconds=$(date +%s)
api_command "{\"push_start\":{\"stream\":\"$source_stream\",\"target\":\"rtmp://127.0.0.1:$rtmp_port/live/$sink_stream\",\"params\":{\"video\":\"restream_source\",\"video_codecs\":[\"H264\"],\"audio_codecs\":[\"AAC\"]}}}" >"$work/push_start.log"
api_command '{"push_list":true}' >"$work/push_list.log"
if ! grep -q '"video":"restream_source"' "$work/push_list.log"; then
  echo "controller did not retain source-only push parameters" >&2
  exit 1
fi
if ! wait_for_stream "$sink_stream" 20; then
  echo "source-only RTMP push did not deliver video and audio within five seconds" >&2
  exit 1
fi
startup_seconds=$(( $(date +%s) - start_seconds ))
if [ "$startup_seconds" -gt 5 ]; then
  echo "source-only RTMP push took $startup_seconds seconds to deliver a track pair" >&2
  exit 1
fi
"$timeout_program" 8 "$ffprobe" -v error -show_entries stream=codec_name,width,height -of csv=p=0 \
  "rtmp://127.0.0.1:$rtmp_port/live/$sink_stream" >"$work/sink_tracks.log" 2>"$work/ffprobe.log"
if [ "$(wc -l <"$work/sink_tracks.log")" -ne 2 ] ||
   ! grep -q 'h264,640,360' "$work/sink_tracks.log" || ! grep -q 'aac' "$work/sink_tracks.log"; then
  echo "RTMP sink did not receive the expected one-video/one-audio source pair" >&2
  exit 1
fi

old_push_id=$(sed -n 's/.*"push_list":\[\[\([0-9]*\),.*/\1/p' "$work/push_list.log")
if [ -z "$old_push_id" ]; then
  echo "source-only push has no process ID" >&2
  exit 1
fi
api_command "{\"push_start\":{\"stream\":\"$source_stream\",\"target\":\"rtmp://127.0.0.1:$rtmp_port/live/$sink_stream\",\"params\":{\"video\":\"restream_auto\",\"video_codecs\":[\"H264\"],\"audio_codecs\":[\"AAC\"]}}}" >"$work/overlap_start.log"
api_command '{"push_list":true}' >"$work/overlap_list.log"
if ! grep -q "\[$old_push_id," "$work/overlap_list.log" ||
   grep -q '"video":"restream_auto"' "$work/overlap_list.log"; then
  echo "controller allowed overlapping same-target push processes" >&2
  exit 1
fi
api_command "{\"push_stop\":$old_push_id}" >"$work/replacement_stop.log"
attempt=0
while [ "$attempt" -lt 40 ]; do
  api_command '{"push_list":true}' >"$work/replacement_wait.log"
  if ! grep -q "\[$old_push_id," "$work/replacement_wait.log"; then break; fi
  attempt=$((attempt + 1))
  sleep 0.1
done
if [ "$attempt" -eq 40 ]; then
  echo "old RTMP push did not exit before replacement" >&2
  exit 1
fi
api_command "{\"push_start\":{\"stream\":\"$source_stream\",\"target\":\"rtmp://127.0.0.1:$rtmp_port/live/$sink_stream\",\"params\":{\"video\":\"restream_auto\",\"video_codecs\":[\"H264\"],\"audio_codecs\":[\"AAC\"]}}}" >"$work/replacement_start.log"
api_command '{"push_list":true}' >"$work/replacement_list.log"
new_push_id=$(sed -n 's/.*"push_list":\[\[\([0-9]*\),.*/\1/p' "$work/replacement_list.log")
if [ -z "$old_push_id" ] || [ -z "$new_push_id" ] || [ "$old_push_id" = "$new_push_id" ] ||
   ! grep -q '"video":"restream_auto"' "$work/replacement_list.log" ||
   grep -q '"video":"restream_source"' "$work/replacement_list.log"; then
  echo "same-target parameter update did not replace exactly one push process" >&2
  exit 1
fi
if ! wait_for_stream "$sink_stream" 20; then
  echo "replacement push did not resume the RTMP sink" >&2
  exit 1
fi

api_command "{\"push_stop\":$new_push_id}" >"$work/legacy_stop.log"
attempt=0
while [ "$attempt" -lt 40 ]; do
  api_command '{"push_list":true}' >"$work/legacy_wait.log"
  if ! grep -q "\[$new_push_id," "$work/legacy_wait.log"; then break; fi
  attempt=$((attempt + 1))
  sleep 0.1
done
if [ "$attempt" -eq 40 ]; then
  echo "replacement push did not exit before legacy push" >&2
  exit 1
fi
api_command "{\"push_start\":{\"stream\":\"$source_stream\",\"target\":\"rtmp://127.0.0.1:$rtmp_port/live/$sink_stream\"}}" >"$work/legacy_start.log"
api_command '{"push_list":true}' >"$work/legacy_list.log"
if grep -q 'Invalid RTMP push parameters' "$work/legacy_start.log" ||
   ! grep -q "rtmp://127.0.0.1:$rtmp_port/live/$sink_stream" "$work/legacy_list.log"; then
  echo "object-form push_start without params was rejected" >&2
  exit 1
fi
legacy_push_id=$(sed -n 's/.*"push_list":\[\[\([0-9]*\),.*/\1/p' "$work/legacy_list.log")
if [ -z "$legacy_push_id" ]; then
  echo "legacy push has no process ID" >&2
  exit 1
fi

api_command "{\"push_start\":{\"stream\":\"$source_stream\",\"target\":\"rtmp://127.0.0.1:$rtmp_port/live/$failed_stream\",\"params\":{\"video\":\"restream_source\",\"max_video_width\":100,\"audio_codecs\":[\"AAC\"]}}}" >"$work/failed_push_start.log"
api_command '{"push_list":true}' >"$work/failed_push_list.log"
if ! grep -q '"max_video_width":100' "$work/failed_push_list.log"; then
  echo "controller did not retain the strict video width cap" >&2
  exit 1
fi
if ! wait_for_log 'No compatible tracks for restream' 160; then
  echo "strict media selection did not fail within its bounded wait" >&2
  exit 1
fi
attempt=0
while [ "$attempt" -lt 160 ]; do
  if grep -q '"reason_code":"media_selection_failed"' "$work/push_end.log" 2>/dev/null; then break; fi
  attempt=$((attempt + 1))
  sleep 0.25
done
if [ "$attempt" -eq 160 ]; then
  echo "PUSH_END did not carry a structured media-selection failure reason" >&2
  exit 1
fi
api_command "{\"push_stop\":$legacy_push_id}" >"$work/late_status_stop.log"
attempt=0
while [ "$attempt" -lt 40 ]; do
  api_command '{"push_list":true}' >"$work/late_status_wait.log"
  if ! grep -q "\[$legacy_push_id," "$work/late_status_wait.log"; then break; fi
  attempt=$((attempt + 1))
  sleep 0.05
done
if [ "$attempt" -eq 40 ]; then
  echo "legacy push did not exit for late-status test" >&2
  exit 1
fi
api_command "{\"push_status_update\":{\"id\":$legacy_push_id,\"stream\":\"$source_stream\",\"status\":{\"reason_code\":\"late_status_test\"}}" >"$work/late_status_update.log"
attempt=0
while [ "$attempt" -lt 16 ]; do
  if grep -q 'late_status_test' "$work/push_end.log" 2>/dev/null; then break; fi
  attempt=$((attempt + 1))
  sleep 0.25
done
if [ "$attempt" -eq 16 ]; then
  echo "PUSH_END did not include a status update sent after process exit" >&2
  exit 1
fi
echo "RTMP source-only push delivered one A/V pair in ${startup_seconds}s; incompatible source failed within 40s"
