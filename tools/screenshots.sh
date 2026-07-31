#!/usr/bin/env bash
# Regenerates the images in docs/images.
#
# Two kinds of picture, and the difference matters:
#
#   * The control page is captured with headless Chrome, because the operator
#     window shows exactly this page and a browser capture is reproducible in a
#     way that driving the macOS window server is not.
#
#   * The output frames are captured by tools/ndi_probe — a separate receiver —
#     straight off the network and converted back to RGB. They are genuinely
#     what a receiver got, not a screenshot of a browser, which is the whole
#     point of showing them.
#
# macOS only as written: it uses sips to convert PPM to PNG.

set -euo pipefail

cd "$(dirname "$0")/.."
ROOT="$PWD"
OUT="$ROOT/docs/images"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

APP="$ROOT/build/Release/WebLinked.app/Contents/MacOS/WebLinked"
CHROME="/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"
NDI_INC="/Library/NDI SDK for Apple/include"
NDI_LIB="/Library/NDI SDK for Apple/lib/macOS/libndi.dylib"
PORT=7654
SOURCE_NAME="WebLinkedShots"

for required in "$APP" "$CHROME" "$NDI_LIB"; do
  [ -e "$required" ] || { echo "missing: $required" >&2; exit 1; }
done

mkdir -p "$OUT"

echo "==> building ndi_probe"
clang++ -std=c++20 -O1 -I"$NDI_INC" tools/ndi_probe.cpp "$NDI_LIB" \
  -Wl,-rpath,"$(dirname "$NDI_LIB")" -o "$WORK/ndi_probe"

# CEF refuses to start a second instance against the same profile, so make sure
# nothing else is holding it.
pkill -f "WebLinked.app/Contents/MacOS/WebLinked" 2>/dev/null || true
sleep 2

echo "==> starting WebLinked on https://github.com"
WEBLINKED_LOG_DIR="$WORK/logs" "$APP" \
  --url "https://github.com" --format 1080p50 --ndi="$SOURCE_NAME" \
  --port "$PORT" --headless >"$WORK/app.log" 2>&1 &
APP_PID=$!
trap 'kill -TERM $APP_PID 2>/dev/null || true; rm -rf "$WORK"' EXIT

# Give Chromium time to load the page and paint it.
sleep 16

capture_gui() {   # capture_gui <output.png>
  "$CHROME" --headless --disable-gpu --hide-scrollbars \
    --screenshot="$1" --window-size=1360,940 --virtual-time-budget=6000 \
    "http://127.0.0.1:$PORT/" >/dev/null 2>&1
}

capture_frame() { # capture_frame <output.png>
  # --save-after skips the first frames so the page has settled.
  "$WORK/ndi_probe" --source "$SOURCE_NAME" --frames 30 \
    --save "$WORK/frame.ppm" --save-after 20 --timeout 25 >/dev/null
  sips -s format png "$WORK/frame.ppm" --out "$1" >/dev/null
}

echo "==> capturing control page and github.com output"
capture_gui "$OUT/control-page.png"
capture_frame "$OUT/output-github.png"

echo "==> switching to the clock page"
curl -fsS -X POST -H 'Content-Type: application/json' \
  -d "{\"url\":\"file://$ROOT/tools/clock.html\"}" \
  "http://127.0.0.1:$PORT/api/url" >/dev/null
sleep 6

echo "==> capturing clock output"
capture_frame "$OUT/output-clock.png"

echo
echo "wrote:"
ls -la "$OUT"
