#!/usr/bin/env python3
"""Offline-render test: runs `drums-engine --offline-render` and checks the WAV sample by sample.

1. Click only, 60 bpm, 12/8 (beat = dotted quarter = 720 ticks), 1 count-in bar: every click onset
   must be at an exact multiple of 16000 frames (1/3 s); accents every 192000 (a 4-beat bar = 4 s),
   beats every 48000 (1 s), subdivisions in between. The click kind is identified from the onset
   sample's level (accent 0.5, beat 0.36, subdivision 0.2, times click volume 0.8 and master 0.8).
2. Same at 97 bpm, where frame positions are fractional: onsets must equal ceil(tick * frames/tick).
3. Groove only (click off) with --trace: the groove notes must be scheduled at 192000 + tick*200/3
   (+ 192000 per loop), the first non-zero sample must be frame 192000 and every step must be audible.
Exit code 0 = all good. Uses only the standard library.
"""
import argparse
import math
import os
import struct
import subprocess
import sys

RATE = 48000
LEVELS = {"accent": 0.5 * 0.64, "beat": 0.36 * 0.64, "sub": 0.2 * 0.64}


def read_wav(path):
    with open(path, "rb") as f:
        data = f.read()
    assert data[:4] == b"RIFF" and data[8:12] == b"WAVE", "not a WAV file"
    pos, fmt, samples = 12, None, None
    while pos < len(data):
        cid, size = data[pos:pos + 4], struct.unpack("<I", data[pos + 4:pos + 8])[0]
        body = data[pos + 8:pos + 8 + size]
        if cid == b"fmt ":
            fmt = struct.unpack("<HHIIHH", body[:16])
        elif cid == b"data":
            samples = body
        pos += 8 + size + (size & 1)
    tag, channels, rate, _, _, bits = fmt
    assert tag == 3 and bits == 32 and channels == 2 and rate == RATE, fmt
    n = len(samples) // 8
    inter = struct.unpack("<%df" % (2 * n), samples[:8 * n])
    return list(inter[0::2]), list(inter[1::2])


def onsets(x, threshold=1e-4, quiet=1e-6, gap=200):
    """Frames where |x| exceeds threshold after at least `gap` frames of near-silence."""
    out, silent = [], gap
    for i, v in enumerate(x):
        a = abs(v)
        if a > threshold and silent >= gap:
            out.append(i)
        silent = silent + 1 if a <= quiet else 0
    return out


def classify(v):
    return min(LEVELS, key=lambda k: abs(LEVELS[k] - abs(v)))


def render(engine, out, extra):
    cmd = [engine, "--offline-render", out] + extra
    print("$", " ".join(cmd))
    r = subprocess.run(cmd, capture_output=True, text=True)
    sys.stdout.write("".join("  " + l + "\n" for l in r.stderr.splitlines() if "offline render" in l))
    if r.returncode != 0:
        print(r.stderr)
        raise SystemExit("render failed")
    return r.stdout


def expected_clicks(bpm, seconds, count_in=1, bar=2880, beat=720, sub=240):
    frames_per_tick = 60.0 * RATE / (bpm * beat)
    start = -count_in * bar
    out, t = [], start
    while True:
        f = math.ceil((t - start) * frames_per_tick - 1e-6)
        if f >= seconds * RATE:
            return out
        in_bar = t % bar
        kind = "accent" if in_bar == 0 else ("beat" if in_bar % beat == 0 else "sub")
        out.append((f, kind, t))
        t += sub


def check_clicks(left, right, bpm, seconds):
    exp = expected_clicks(bpm, seconds)
    got = onsets(left)
    ok = len(got) == len(exp)
    bad = 0
    for (f, kind, _), g in zip(exp, got):
        k = classify(left[g])
        if g != f or k != kind or left[g] != right[g]:
            bad += 1
            if bad <= 5:
                print("  MISMATCH expected %d %s, got %d %s (%.4f)" % (f, kind, g, k, left[g]))
    ok = ok and bad == 0
    by_kind = {}
    for f, kind, _ in exp:
        by_kind.setdefault(kind, []).append(f)
    acc = by_kind["accent"]
    beats = sorted(by_kind["accent"] + by_kind["beat"])
    print("  %d onsets found / %d expected, %d mismatches" % (len(got), len(exp), bad))
    print("  accent frames %s -> spacing %s frames = %.4f s" % (
        acc[:4], sorted(set(b - a for a, b in zip(acc, acc[1:]))), (acc[1] - acc[0]) / RATE))
    print("  beat-or-accent spacing %s frames = %s s" % (
        sorted(set(b - a for a, b in zip(beats, beats[1:]))),
        sorted(set(round((b - a) / RATE, 4) for a, b in zip(beats, beats[1:])))))
    allf = [f for f, _, _ in exp]
    print("  any-click spacing %s frames = %s s" % (
        sorted(set(b - a for a, b in zip(allf, allf[1:]))),
        sorted(set(round((b - a) / RATE, 5) for a, b in zip(allf, allf[1:])))))
    print("  onset levels: accent %.4f, beat %.4f, sub %.4f" % tuple(
        left[got[i]] for i in (0, 3, 1)))
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--engine", required=True)
    ap.add_argument("--out", required=True, help="output directory")
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    ok = True

    print("[1] click only, 60 bpm 12/8, 13 s")
    wav = os.path.join(args.out, "click-60.wav")
    render(args.engine, wav, ["--seconds", "13", "--bpm", "60", "--no-groove", "--buffer", "128"])
    left, right = read_wav(wav)
    r = check_clicks(left, right, 60, 13)
    print("  ->", "OK" if r else "FAIL")
    ok &= r

    print("[2] click only, 97 bpm (fractional frame positions), block 100, 9 s")
    wav = os.path.join(args.out, "click-97.wav")
    render(args.engine, wav, ["--seconds", "9", "--bpm", "97", "--no-groove", "--buffer", "100"])
    left, right = read_wav(wav)
    r = check_clicks(left, right, 97, 9)
    print("  ->", "OK" if r else "FAIL")
    ok &= r

    print("[3] groove only (kit, click off), 60 bpm, 13 s, --trace")
    wav = os.path.join(args.out, "groove-60.wav")
    trace = render(args.engine, wav, ["--seconds", "13", "--bpm", "60", "--groove", "--no-click", "--trace",
                                      "--buffer", "128"])
    left, _ = read_wav(wav)
    groove = []
    for line in trace.splitlines():
        p = line.split()
        if len(p) >= 3 and p[2] == "groove":
            groove.append((int(p[0]), float(p[1]), p[3]))
    exercise = [(0, "kick"), (0, "hihatClosed"), (240, "hihatClosed"), (480, "hihatClosed"),
                (720, "hihatClosed"), (960, "snare"), (960, "hihatClosed"), (1200, "kick"),
                (1200, "hihatClosed"), (1440, "snare"), (1440, "hihatClosed"), (1680, "hihatClosed"),
                (1920, "hihatClosed"), (2160, "hihatClosed"), (2400, "hihatClosed"), (2640, "kick"),
                (2640, "hihatClosed")]
    exp = [(192000 + loop * 192000 + tick * 200 // 3, name)
           for loop in range(3) for tick, name in exercise if 192000 + loop * 192000 + tick * 200 // 3 < 13 * RATE]
    got = [(f, name) for f, _, name in groove]
    r = got == exp
    first = next(i for i, v in enumerate(left) if v != 0.0)
    # Each groove step (distinct frame) must raise the level: peak in the 20 ms after > peak in the 5 ms before.
    steps = sorted(set(f for f, _ in got))
    rising = sum(1 for f in steps if max(abs(v) for v in left[f:f + 960]) > max(abs(v) for v in left[f - 240:f]))
    print("  %d groove notes scheduled, %d expected, frames %s" % (len(got), len(exp), "match" if r else "MISMATCH"))
    print("  first loop: %s" % ", ".join("%d:%s" % g for g in got[:6]))
    print("  first non-zero sample at frame %d (exercise start = 192000 after the 4 s count-in bar)" % first)
    print("  level rises at %d/%d groove steps" % (rising, len(steps)))
    r = r and first == 192000 and rising == len(steps)
    print("  ->", "OK" if r else "FAIL")
    ok &= r

    print("ALL OK" if ok else "FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
