#!/bin/sh
# WHIP offers and their DTLS roles. ffmpeg's WHIP muxer offers a=setup:passive,
# so Mist must answer a=setup:active and start the DTLS handshake as client.
#  1. The captured ffmpeg offer gets a 201 SDP answer with a Content-Length
#     (ffmpeg rejects a body that only ends at connection close) that
#     announces a=setup:active.
#  2. Offers Mist cannot answer (a=setup:holdconn, an unparsable m= line) get
#     an HTTP error that names the reason, and a WARN log line with it.
#  3. ffmpeg publishes over WHIP end to end: the stream goes online with its
#     video and audio tracks, and ffmpeg's closing DELETE ends the session.
set -eu

if [ "$#" -ne 7 ]; then
  echo "usage: $0 ffmpeg curl MistController MistOutHTTP MistOutWebRTC MistUtilNuke timeout" >&2
  exit 2
fi
if [ "${MIST_RUN_MEDIA_TESTS:-}" != "1" ]; then
  echo "set MIST_RUN_MEDIA_TESTS=1 to run the WHIP ingest pipeline" >&2
  exit 77
fi

ffmpeg=$1
curl=$2
controller=$3
output_http=$4
output_webrtc=$5
util_nuke=$6
timeout_program=$7
for program in "$ffmpeg" "$curl" "$controller" "$output_http" "$output_webrtc" "$util_nuke" "$timeout_program"; do
  if [ ! -x "$program" ]; then
    echo "required executable is unavailable: $program" >&2
    exit 77
  fi
done
if ! "$ffmpeg" -hide_banner -muxers 2>/dev/null | grep -qw 'whip'; then
  echo "ffmpeg lacks the WHIP muxer required for the publisher fixture" >&2
  exit 77
fi
if ! "$ffmpeg" -hide_banner -encoders 2>/dev/null | grep -q 'libx264'; then
  echo "ffmpeg lacks the libx264 encoder required for the publisher fixture" >&2
  exit 77
fi

work=$(mktemp -d "${TMPDIR:-/tmp}/mist-whip-setup.XXXXXX")
ipc_root="$work/ipc"
mkdir -p "$ipc_root"
stream="whipsetup$$"
controller_pid=
publisher_pid=

cleanup() {
  status=$?
  trap - EXIT HUP INT TERM
  if [ "$status" -ne 0 ]; then
    echo "WHIP setup integration failed; logs follow:" >&2
    for log in "$work"/*.log "$work"/*.txt; do
      if [ -f "$log" ]; then
        echo "Log: $log" >&2
        grep -vE 'bootmsoffset' "$log" | tail -60 >&2
      fi
    done
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

api_port=$((24000 + ($$ % 12000)))
http_port=$((api_port + 1))
webrtc_port=$((api_port + 2))
config="$work/config.json"
printf '%s\n' \
  "{\"auto_push\":null,\"bandwidth\":{\"exceptions\":[\"::1\",\"127.0.0.0/8\"]},\"config\":{\"accesslog\":\"LOG\",\"controller\":{\"interface\":\"127.0.0.1\",\"port\":$api_port,\"username\":null},\"debug\":4,\"defaultStream\":null,\"prometheus\":\"\",\"protocols\":[{\"connector\":\"HTTP\",\"interface\":\"127.0.0.1\",\"port\":$http_port},{\"connector\":\"WebRTC\",\"bindhost\":\"127.0.0.1\",\"pubhost\":\"127.0.0.1\",\"port\":$webrtc_port}],\"serverid\":null,\"sessionInputMode\":15,\"sessionOutputMode\":15,\"sessionStreamInfoMode\":1,\"sessionUnspecifiedMode\":0,\"sessionViewerMode\":14,\"tknMode\":15,\"triggers\":{},\"trustedproxy\":[]},\"extwriters\":null,\"jwks\":null,\"push_settings\":{\"maxspeed\":0,\"wait\":3},\"streamkeys\":null,\"streams\":{\"$stream\":{\"name\":\"$stream\",\"source\":\"push://\"}},\"variables\":null}" \
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
logged() { grep -qE -- "$2" "$1" 2>/dev/null; }
connectors_started() {
  started=$(grep -c 'Started connector' "$work/controller.log" 2>/dev/null || true)
  [ "${started:-0}" -ge 2 ]
}
wait_for 30 "the test controller and its HTTP and WebRTC connectors" connectors_started
http_ready() { "$curl" -s -o /dev/null "http://127.0.0.1:$http_port/"; }
wait_for 10 "the HTTP connector to accept connections" http_ready

# The offer ffmpeg 9's WHIP muxer sends, as captured from `ffmpeg -f whip`.
ffmpeg_offer() {
  audio_setup=${1:-passive}
  video_line=${2:-'m=video 9 UDP/TLS/RTP/SAVPF 106 105'}
  printf '%s\r\n' \
    'v=0' \
    'o=FFmpeg 4489045141692799359 2 IN IP4 127.0.0.1' \
    's=FFmpegPublishSession' \
    't=0 0' \
    'a=group:BUNDLE 0 1' \
    'a=extmap-allow-mixed' \
    'a=msid-semantic: WMS' \
    'm=audio 9 UDP/TLS/RTP/SAVPF 111' \
    'c=IN IP4 0.0.0.0' \
    'a=ice-ufrag:5485e299' \
    'a=ice-pwd:a6eeb0b965dbbc0abccda0987882d428' \
    'a=fingerprint:sha-256 5C:25:B5:8A:EE:36:5E:29:40:98:3A:90:97:18:3A:61:40:17:2E:2C:2A:97:01:AB:D7:9B:90:F0:95:77:9C:ED' \
    "a=setup:$audio_setup" \
    'a=mid:0' \
    'a=sendonly' \
    'a=msid:FFmpeg audio' \
    'a=rtcp-mux' \
    'a=rtpmap:111 opus/48000/2' \
    'a=ssrc:3475922372 cname:FFmpeg' \
    'a=ssrc:3475922372 msid:FFmpeg audio' \
    "$video_line" \
    'c=IN IP4 0.0.0.0' \
    'a=ice-ufrag:5485e299' \
    'a=ice-pwd:a6eeb0b965dbbc0abccda0987882d428' \
    'a=fingerprint:sha-256 5C:25:B5:8A:EE:36:5E:29:40:98:3A:90:97:18:3A:61:40:17:2E:2C:2A:97:01:AB:D7:9B:90:F0:95:77:9C:ED' \
    "a=setup:$audio_setup" \
    'a=mid:1' \
    'a=sendonly' \
    'a=msid:FFmpeg video' \
    'a=rtcp-mux' \
    'a=rtcp-rsize' \
    'a=rtpmap:106 H264/90000' \
    'a=fmtp:106 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42001e' \
    'a=rtcp-fb:106 nack' \
    'a=rtpmap:105 rtx/90000' \
    'a=fmtp:105 apt=106' \
    'a=ssrc-group:FID 3475922373 3475922374' \
    'a=ssrc:3475922373 cname:FFmpeg' \
    'a=ssrc:3475922373 msid:FFmpeg video'
}

# post_offer <name> <offer file>: POSTs like ffmpeg does (Connection: close) and
# stores the status line + headers in <name>.headers.txt and the body in <name>.body.txt.
post_offer() {
  "$curl" -s --max-time 15 -D "$work/$1.headers.txt" -o "$work/$1.body.txt" \
    -H 'Connection: close' -H 'Content-Type: application/sdp' --data-binary "@$2" \
    "http://127.0.0.1:$http_port/webrtc/$stream" || true
}
status_of() { head -n 1 "$work/$1.headers.txt" 2>/dev/null | tr -d '\r' | cut -d' ' -f2; }
content_length_of() {
  grep -i '^content-length:' "$work/$1.headers.txt" 2>/dev/null | tail -n 1 | tr -d '\r' | cut -d' ' -f2
}
body_size_of() { wc -c <"$work/$1.body.txt" 2>/dev/null | tr -d ' '; }

# 1. The ffmpeg offer (a=setup:passive) is answered as the active side.
ffmpeg_offer passive >"$work/offer-passive.sdp"
post_offer passive "$work/offer-passive.sdp"
if [ "$(status_of passive)" != "201" ]; then
  echo "the ffmpeg offer got status '$(status_of passive)', not 201 Created" >&2
  exit 1
fi
length=$(content_length_of passive)
if [ -z "$length" ] || [ "$length" != "$(body_size_of passive)" ] || [ "$length" -eq 0 ]; then
  echo "the SDP answer has Content-Length '${length:-none}' for a $(body_size_of passive) byte body;" \
    "ffmpeg cannot read an answer without a matching length" >&2
  exit 1
fi
if ! grep -q '^a=setup:active' "$work/passive.body.txt"; then
  echo "the answer to an a=setup:passive offer does not take the active DTLS role:" \
    "$(grep '^a=setup' "$work/passive.body.txt" | head -n 1 | tr -d '\r')" >&2
  exit 1
fi

# 2. Offers Mist cannot answer are refused with a reason, in the response and the log.
ffmpeg_offer holdconn >"$work/offer-holdconn.sdp"
post_offer holdconn "$work/offer-holdconn.sdp"
if [ "$(status_of holdconn)" != "400" ] || ! grep -q 'holdconn' "$work/holdconn.body.txt"; then
  echo "an a=setup:holdconn offer got status '$(status_of holdconn)' and body" \
    "'$(cat "$work/holdconn.body.txt" 2>/dev/null)'; expected 400 naming holdconn" >&2
  exit 1
fi
wait_for 5 "the WARN line for the holdconn offer" \
  logged "$work/controller.log" 'WARN: Rejecting WebRTC offer .*unsupported a=setup:holdconn'

ffmpeg_offer passive 'm=video' >"$work/offer-broken.sdp"
post_offer broken "$work/offer-broken.sdp"
if [ "$(status_of broken)" != "400" ] || [ "$(body_size_of broken)" -eq 0 ]; then
  echo "an unparsable offer got status '$(status_of broken)' with a $(body_size_of broken) byte body; expected 400 with a reason" >&2
  exit 1
fi
wait_for 5 "the WARN line for the unparsable offer" \
  logged "$work/controller.log" 'WARN: Rejecting WebRTC offer .*could not be parsed'

# 3. ffmpeg publishes over WHIP; the stream goes online with video and audio,
# and ffmpeg's closing DELETE ends the Mist session.
"$timeout_program" 40 "$ffmpeg" -hide_banner -loglevel info -re \
  -f lavfi -i "testsrc2=size=320x180:rate=25" -f lavfi -i "sine=frequency=997:sample_rate=48000" -t 10 \
  -c:v libx264 -preset ultrafast -tune zerolatency -profile:v baseline -pix_fmt yuv420p -bf 0 -g 25 \
  -c:a libopus -ar 48000 -ac 2 -f whip "http://127.0.0.1:$http_port/webrtc/$stream" \
  >"$work/publisher.log" 2>&1 &
publisher_pid=$!

stream_state() {
  "$curl" -s --max-time 5 "http://127.0.0.1:$api_port/api2" \
    --data-urlencode 'command={"active_streams":["tracks","status"]}' 2>/dev/null |
    sed -n "s/.*\"$stream\":\[\([0-9]*\),\"\([^\"]*\)\"\].*/\1 \2/p"
}
live_with_tracks() {
  state=$(stream_state)
  tracks=${state%% *}
  [ -n "$tracks" ] && [ "$tracks" -ge 2 ] && [ "${state#* }" = "Online" ]
}
wait_for 25 "the WHIP publish to go online with video and audio tracks" live_with_tracks
live_state=$(stream_state)
if ! logged "$work/controller.log" 'Starting DTLS as client' || ! logged "$work/controller.log" 'dTLS handshake complete'; then
  echo "the stream went live without Mist completing DTLS as client" >&2
  exit 1
fi
publisher_status=0
wait "$publisher_pid" || publisher_status=$?
publisher_pid=
# ffmpeg 9 reports "Failed to dispose resource" for its closing DELETE even
# though Mist answers it with a complete 200 and ends the session; only the
# publish itself is checked here.
if [ "$publisher_status" -ne 0 ] || logged "$work/publisher.log" 'Failed to read response from url='; then
  echo "the WHIP publisher exited with status $publisher_status:" \
    "$(grep -E 'Failed|rror' "$work/publisher.log" | head -n 3)" >&2
  exit 1
fi
wait_for 10 "ffmpeg's DELETE to end the WebRTC session" \
  logged "$work/controller.log" 'exit reason: WebRTC session deleted by user'
echo "ffmpeg's a=setup:passive WHIP offer was answered active, refusals carry reasons," \
  "and the WHIP publish went live ($live_state tracks/status) and ended cleanly"
