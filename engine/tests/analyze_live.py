#!/usr/bin/env python3
"""Checks a live_test.sh run: each MIDI note-on seen by jack-probe at frame F must start sounding in
the engine output captured by jack-probe at frame F (same JACK cycle, sample-accurate), the engine
must have logged exactly those hits (MIDI clock ignored), with the same frame spacing."""
import os
import re
import struct
import sys


def read_wav(path):
    data = open(path, "rb").read()
    pos, samples = 12, b""
    while pos < len(data):
        cid, size = data[pos:pos + 4], struct.unpack("<I", data[pos + 4:pos + 8])[0]
        if cid == b"data":
            samples = data[pos + 8:pos + 8 + size]
        pos += 8 + size + (size & 1)
    n = len(samples) // 8
    inter = struct.unpack("<%df" % (2 * n), samples[:8 * n])
    return inter[0::2], inter[1::2]


def main(out):
    left, right = read_wav(os.path.join(out, "probe.wav"))
    notes = []
    for line in open(os.path.join(out, "probe-midi.txt")):
        frame, status, d1, d2 = line.split()
        if int(status, 16) & 0xF0 == 0x90 and int(d2) > 0:
            notes.append((int(frame), int(d1), int(d2)))
    engine_hits = []
    for line in open(os.path.join(out, "engine.log")):
        m = re.search(r"hit #\d+ note (\d+) vel (\d+) -> (\w+)\s+frame (\d+)", line)
        if m:
            engine_hits.append((int(m.group(4)), int(m.group(1)), m.group(3)))
    print("probe saw %d note-ons; engine logged %d hits" % (len(notes), len(engine_hits)))
    ok = len(notes) > 0 and len(notes) == len(engine_hits)
    for i, (f, note, vel) in enumerate(notes):
        start = max(0, f - 4800)  # 100 ms before: the previous note's tail has ended by then
        silent_before = all(left[j] == 0.0 and right[j] == 0.0 for j in range(start, f))
        first = next((j for j in range(start, min(len(left), f + 48000))
                      if left[j] != 0.0 or right[j] != 0.0), None)
        peak = max(abs(v) for v in left[f:f + 4800]) if f < len(left) else 0
        delta = None if first is None else first - f
        good = silent_before and delta == 0 and peak > 0.01
        ok &= good
        print("  note %3d vel %3d  MIDI frame %7d  first sound %s  (%+s frames)  silent before: %s  peak %.3f  %s" % (
            note, vel, f, first, delta, silent_before, peak, "ok" if good else "FAIL"))
    if len(engine_hits) == len(notes) and notes:
        probe_gaps = [b[0] - a[0] for a, b in zip(notes, notes[1:])]
        engine_gaps = [b[0] - a[0] for a, b in zip(engine_hits, engine_hits[1:])]
        same = probe_gaps == engine_gaps and [n for _, n, _ in notes] == [n for _, n, _ in engine_hits]
        ok &= same
        print("  frame gaps probe %s / engine %s -> %s" % (probe_gaps, engine_gaps, "identical" if same else "DIFFER"))
        print("  engine instruments: %s" % ", ".join("%d=%s" % (n, i) for _, n, i in engine_hits))
    print("LIVE OK" if ok else "LIVE FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
