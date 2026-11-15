#!/usr/bin/env bash
# Live JACK test, silent: the engine runs with --no-autoconnect (nothing reaches the speakers).
#   1. starts drums-engine (transport stopped, so only pad hits make sound)
#   2. starts midi-sender: a virtual ALSA MIDI port "e-drum-sim" sending MIDI clock + drum notes
#   3. connects e-drum-sim -> drums-engine:midi_in with pw-link
#   4. jack-probe records the engine's output and the same MIDI on the same JACK frame clock
#   5. analyze_live.py: every note must start sounding exactly at its MIDI frame
# Usage: engine/tests/live_test.sh [out-dir] [extra drums-engine args...]
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD="$HERE/../build"
OUT="${1:-$(mktemp -d)}"
shift || true
mkdir -p "$OUT"

cleanup() {
    for p in ${PIDS:-}; do kill "$p" 2>/dev/null || true; done
}
trap cleanup EXIT
PIDS=""

"$BUILD/drums-engine" --no-autoconnect --log-hits --buffer 128 --port 7979 "$@" >"$OUT/engine.log" 2>&1 &
ENGINE=$!
PIDS="$ENGINE"
for _ in $(seq 50); do grep -q "running" "$OUT/engine.log" 2>/dev/null && break; sleep 0.1; done

"$BUILD/tests/midi-sender" --name e-drum-sim --delay 2.5 --count 5 --interval-ms 3000 --notes 36,38,42,45,38 \
    >"$OUT/sender.txt" 2>"$OUT/sender.log" &
SENDER=$!
PIDS="$PIDS $SENDER"

SRC=""
for _ in $(seq 40); do
    SRC="$(pw-link -o 2>/dev/null | grep -i 'e-drum-sim' | head -1 || true)"
    [[ -n "$SRC" ]] && break
    sleep 0.05
done
[[ -n "$SRC" ]] || { echo "e-drum-sim port did not appear" >&2; exit 1; }
echo "MIDI source port: $SRC"
pw-link "$SRC" "drums-engine:midi_in"

# CLOCK_FROM: a device port (default: the analog sink's monitor) so a real device clock drives the
# graph, like when the engine plays to headphones. Set CLOCK_FROM= to use PipeWire's dummy driver.
CLOCK_FROM="${CLOCK_FROM-Built-in Audio Analog Stereo:monitor_FL}"
"$BUILD/tests/jack-probe" --seconds 18 --wav "$OUT/probe.wav" --midi-log "$OUT/probe-midi.txt" \
    --midi-match e-drum-sim ${CLOCK_FROM:+--clock-from "$CLOCK_FROM"} 2>"$OUT/probe.log" &
PROBE=$!
PIDS="$PIDS $PROBE"

wait "$SENDER"
wait "$PROBE"
kill -INT "$ENGINE"
wait "$ENGINE" || true
PIDS=""

echo "---- engine log"
cat "$OUT/engine.log"
echo "---- probe"
cat "$OUT/probe.log"
python3 -I "$HERE/analyze_live.py" "$OUT"
