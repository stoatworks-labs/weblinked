#!/usr/bin/env bash
# Render the launcher panel (the exact HTML/CSS the app's webview shows) to a
# PNG via headless Chrome. Used to produce README screenshots without having to
# capture the native window.
#
# Usage:
#   scripts/screenshot.sh "flock" 8080 running 10.147.17.93 out.png
#   scripts/screenshot.sh <app-name> <port> <stopped|running> <host> <out.png>
set -euo pipefail

APP="${1:-SRT Router}"
PORT="${2:-8080}"
STATE="${3:-running}"
HOST="${4:-10.147.17.93}"
OUT="${5:-panel.png}"

DIR="$(cd "$(dirname "$0")/.." && pwd)"
CHROME="/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"
SRV_PORT=5601

# Serve src/ and make sure the server is torn down on exit.
( cd "$DIR/src" && exec python3 -m http.server "$SRV_PORT" ) >/dev/null 2>&1 &
SRV_PID=$!
trap 'kill "$SRV_PID" 2>/dev/null || true' EXIT
sleep 1

QUERY="app=$(python3 -c "import urllib.parse,sys;print(urllib.parse.quote(sys.argv[1]))" "$APP")&port=${PORT}&state=${STATE}&host=${HOST}"
URL="http://localhost:${SRV_PORT}/index.html?${QUERY}"

OUT_ABS="$(cd "$(dirname "$OUT")" && pwd)/$(basename "$OUT")"
# Note: headless Chrome enforces a ~500px minimum window width, so we render at
# 500 (the panel is happy at any width >= its 460 design width) to avoid the
# right edge being clipped.
"$CHROME" --headless=new --disable-gpu --hide-scrollbars \
  --force-device-scale-factor=2 --window-size=500,470 \
  --virtual-time-budget=1500 \
  --screenshot="$OUT_ABS" "$URL" >/dev/null 2>&1

echo "wrote $OUT_ABS"
