#!/usr/bin/env bash
# End-to-end test of the core-linked engine over WebSocket, silent (audio outputs unconnected).
# A simulated e-drum ("e-drum-sim", auto-connected by --midi-match) hits pads while
# ws_smoke.mjs drives the engine. The pad map is a scratch copy (kits/pad-map.json is untouched).
# Usage: engine/tests/ws_test.sh [out-dir]
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD="$HERE/../build"
OUT="${1:-$(mktemp -d)}"
mkdir -p "$OUT"
cp "$HERE/../../kits/pad-map.json" "$OUT/pad-map.json"
PIDS=""
trap 'for p in $PIDS; do kill "$p" 2>/dev/null || true; done' EXIT

"$BUILD/drums-engine" --port 7979 --midi-match e-drum-sim --audio-match no-such-sink --pad-map "$OUT/pad-map.json" \
    --log-hits >"$OUT/engine.log" 2>&1 &
ENGINE=$!
PIDS="$ENGINE"
for _ in $(seq 50); do grep -q "running" "$OUT/engine.log" 2>/dev/null && break; sleep 0.1; done

"$BUILD/tests/midi-sender" --name e-drum-sim --delay 0.8 --count 60 --interval-ms 167 --notes 42,36,42,38 \
    >/dev/null 2>"$OUT/sender.log" &
PIDS="$PIDS $!"

node "$HERE/ws_smoke.mjs" 7979 && rc=0 || rc=$?
kill -INT "$ENGINE"
wait "$ENGINE" || true
PIDS=""
echo "---- engine log (excerpt)"
grep -vE 'hint_stealing|^\[drums-engine\] hit note' "$OUT/engine.log"
echo "  ... $(grep -c '^\[drums-engine\] hit note' "$OUT/engine.log") hit lines, e.g.:"
grep '^\[drums-engine\] hit note' "$OUT/engine.log" | sed -n '1,3p;$p'
exit $rc
