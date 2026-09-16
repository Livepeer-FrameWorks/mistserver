#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "Usage: $0 IMAGE" >&2
  exit 2
fi

image=$1
test_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
work_dir=$(mktemp -d)
container="mist-webrtc-smoke-${RANDOM}-${RANDOM}"
server_pid=

cleanup() {
  docker stop "$container" >/dev/null 2>&1 || true
  if [[ -n "$server_pid" ]]; then kill "$server_pid" >/dev/null 2>&1 || true; fi
  rm -rf "$work_dir"
}
trap cleanup EXIT

docker run --rm --entrypoint ffmpeg -v "$work_dir:/work" "$image" \
  -hide_banner -loglevel error -y \
  -f lavfi -i testsrc2=size=640x360:rate=24 \
  -f lavfi -i sine=frequency=1000:sample_rate=48000 \
  -t 8 -c:v libx264 -pix_fmt yuv420p -c:a aac /work/test.mp4

cat >"$work_dir/config.json" <<'JSON'
{"config":{"debug":3,"protocols":[{"connector":"HTTP","port":18080},{"connector":"HLS"},{"bindhost":"0.0.0.0","connector":"WebRTC","port":18203,"pubhost":"127.0.0.1"}]},"streams":{"demo":{"name":"demo","source":"/work/test.mp4"}}}
JSON

docker run --rm -d --name "$container" --network host --shm-size=256m \
  -v "$work_dir:/work:ro" "$image" -c /work/config.json >/dev/null

for _attempt in $(seq 1 30); do
  if curl --silent --output /dev/null http://127.0.0.1:18080/webrtc/demo; then break; fi
  sleep 1
done

python3 -m http.server 18081 --bind 127.0.0.1 --directory "$test_dir" >"$work_dir/http.log" 2>&1 &
server_pid=$!

chrome=
for candidate in google-chrome google-chrome-stable chromium chromium-browser; do
  if command -v "$candidate" >/dev/null 2>&1; then chrome=$candidate; break; fi
done
if [[ -z "$chrome" ]]; then
  echo "Chrome or Chromium is required for the WebRTC browser smoke test" >&2
  exit 1
fi

set +e
dom=$(timeout 35 "$chrome" --headless=new --no-sandbox --disable-gpu \
  --autoplay-policy=no-user-gesture-required --virtual-time-budget=25000 --dump-dom \
  http://127.0.0.1:18081/webrtc_browser_smoke.html 2>"$work_dir/chrome.log")
chrome_status=$?
set -e

if [[ $chrome_status -ne 0 ]] || ! grep -Fq '&quot;result&quot;:&quot;PASS&quot;' <<<"$dom"; then
  echo "$dom" >&2
  cat "$work_dir/chrome.log" >&2
  docker logs "$container" >&2
  exit 1
fi

grep -F '&quot;result&quot;:&quot;PASS&quot;' <<<"$dom"
