#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "Usage: $0 IMAGE" >&2
  exit 2
fi

image=$1
fixture_image=${FFMPEG_IMAGE:-$image}
test_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
work_dir=$(mktemp -d)
container="mist-webrtc-smoke-${RANDOM}-${RANDOM}"
server_pid=
chrome_pid=
debug_port=19222

# shellcheck disable=SC2329
cleanup() {
  if [[ -n "$chrome_pid" ]]; then
    kill "$chrome_pid" >/dev/null 2>&1 || true
    wait "$chrome_pid" >/dev/null 2>&1 || true
  fi
  if [[ -n "$server_pid" ]]; then
    kill "$server_pid" >/dev/null 2>&1 || true
    wait "$server_pid" >/dev/null 2>&1 || true
  fi
  docker stop "$container" >/dev/null 2>&1 || true
  rm -rf "$work_dir"
}
trap cleanup EXIT

docker run --rm --entrypoint ffmpeg -v "$work_dir:/work" "$fixture_image" \
  -hide_banner -loglevel error -y \
  -f lavfi -i testsrc2=size=640x360:rate=24 \
  -f lavfi -i sine=frequency=1000:sample_rate=48000 \
  -t 8 -c:v libvpx -deadline realtime -cpu-used 8 -pix_fmt yuv420p \
  -c:a libopus /work/test.webm

fixture_streams=$(docker run --rm --entrypoint ffprobe -v "$work_dir:/work:ro" "$fixture_image" \
  -v error -show_entries stream=codec_name,codec_type -of compact /work/test.webm)
grep -Fq 'codec_name=vp8|codec_type=video' <<<"$fixture_streams"
grep -Fq 'codec_name=opus|codec_type=audio' <<<"$fixture_streams"

cat >"$work_dir/config.json" <<'JSON'
{"config":{"debug":3,"protocols":[{"connector":"HTTP","port":18080},{"bindhost":"0.0.0.0","connector":"WebRTC","port":18203,"pubhost":"127.0.0.1"}]},"streams":{"demo":{"name":"demo","source":"/work/test.webm"}}}
JSON

docker run --rm -d --name "$container" --shm-size=256m \
  -p 18080:18080 -p 18203:18203/udp \
  -v "$work_dir:/work" "$image" -c /work/config.json >/dev/null

mist_ready=false
for _attempt in $(seq 1 30); do
  if curl --silent --output /dev/null http://127.0.0.1:18080/webrtc/demo; then
    mist_ready=true
    break
  fi
  sleep 1
done
if [[ "$mist_ready" != true ]]; then
  echo "MistServer HTTP connector did not become ready" >&2
  docker logs "$container" >&2
  exit 1
fi

python3 -m http.server 18081 --bind 127.0.0.1 --directory "$test_dir" >"$work_dir/http.log" 2>&1 &
server_pid=$!
page_ready=false
for _attempt in $(seq 1 10); do
  if curl --silent --fail --output /dev/null http://127.0.0.1:18081/webrtc_browser_smoke.html; then
    page_ready=true
    break
  fi
  sleep 1
done
if [[ "$page_ready" != true ]]; then
  echo "WebRTC smoke-test page did not become ready" >&2
  cat "$work_dir/http.log" >&2
  exit 1
fi

chrome=${CHROME:-}
if [[ -z "$chrome" ]]; then
  for candidate in google-chrome google-chrome-stable chromium chromium-browser; do
    if command -v "$candidate" >/dev/null 2>&1; then chrome=$candidate; break; fi
  done
fi
if [[ -z "$chrome" ]]; then
  echo "Chrome or Chromium is required for the WebRTC browser smoke test" >&2
  exit 1
fi
"$chrome" --version

"$chrome" --headless=new --no-sandbox --disable-gpu \
  --autoplay-policy=no-user-gesture-required \
  --remote-debugging-address=127.0.0.1 --remote-debugging-port="$debug_port" \
  --user-data-dir="$work_dir/chrome-profile" \
  http://127.0.0.1:18081/webrtc_browser_smoke.html \
  >"$work_dir/chrome.log" 2>&1 &
chrome_pid=$!

for _attempt in $(seq 1 30); do
  tabs=$(curl --silent --fail "http://127.0.0.1:${debug_port}/json/list" || true)
  if grep -Fq '"title": "PASS frames=' <<<"$tabs"; then
    grep -F '"title": "PASS frames=' <<<"$tabs"
    exit 0
  fi
  if grep -Fq '"title": "FAIL frames=' <<<"$tabs"; then break; fi
  if ! kill -0 "$chrome_pid" 2>/dev/null; then break; fi
  sleep 1
done

curl --silent "http://127.0.0.1:${debug_port}/json/list" >&2 || true
cat "$work_dir/chrome.log" >&2
docker logs "$container" >&2
exit 1
