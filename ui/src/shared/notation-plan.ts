// Turns an exercise into a drum-notation plan: per bar, two voices (hands: stems up, feet: stems down)
// made of notes, rests and invisible spaces with VexFlow durations, beam groups per beat and tuplets.
// Pure logic (no DOM, no VexFlow) so it is unit-tested; notation.ts turns it into VexFlow objects.
import { barTicks } from './exercise';
import { INSTRUMENT_INFO, byStaffOrder, type Instrument, type NoteHead, type VoiceName } from './instruments';
import type { Exercise, ExerciseNote } from './protocol';

export interface PlannedKey {
  instrument: Instrument;
  key: string;
  head: NoteHead;
  open: boolean;
  accent: boolean;
  ghost: boolean;
  line: number;
}

export type EventKind = 'note' | 'rest' | 'space';

export interface PlannedEvent {
  kind: EventKind;
  /** Absolute tick (from the start of the exercise) where the event is drawn. */
  tick: number;
  /** Real length in ticks. */
  ticks: number;
  /** VexFlow base duration: w h q 8 16 32 64 (nominal value inside a tuplet). */
  duration: string;
  dots: number;
  keys: PlannedKey[];
  /** Beat index inside the bar. */
  beat: number;
  /** A whole-bar rest (drawn as a centred whole rest whatever the meter). */
  wholeBar?: boolean;
}

export interface PlannedVoice {
  name: VoiceName;
  events: PlannedEvent[];
  /** Groups of event indices to beam together. */
  beams: number[][];
  /** Groups of event indices forming a 3:2 tuplet. */
  tuplets: number[][];
}

export interface PlannedBar {
  index: number;
  startTick: number;
  endTick: number;
  hands: PlannedVoice;
  feet: PlannedVoice;
}

export interface PlanOptions {
  /**
   * Hide rests of whole empty beats in the feet voice (as in the reference engraving, where the kick
   * voice shows only the rests needed to place its notes). Default true.
   */
  hideFeetBeatRests?: boolean;
}

const BEAMABLE = new Set(['8', '16', '32', '64']);

// [ticks, base duration, dots] for ppq 480, longest first.
const DURATIONS: Array<[number, string, number]> = [
  [2880, 'w', 1],
  [1920, 'w', 0],
  [1440, 'h', 1],
  [960, 'h', 0],
  [720, 'q', 1],
  [480, 'q', 0],
  [360, '8', 1],
  [240, '8', 0],
  [180, '16', 1],
  [120, '16', 0],
  [90, '32', 1],
  [60, '32', 0],
  [30, '64', 0],
];

const GRID = 30; // smallest representable value (64th)

export function durationFor(ticks: number): { duration: string; dots: number } | null {
  const d = DURATIONS.find(([t]) => t === ticks);
  return d ? { duration: d[1], dots: d[2] } : null;
}

/** Splits a length into representable values, longest first. Remainders below a 64th are dropped. */
export function splitDuration(ticks: number): Array<{ ticks: number; duration: string; dots: number }> {
  const out: Array<{ ticks: number; duration: string; dots: number }> = [];
  let left = ticks;
  while (left >= GRID) {
    const d = DURATIONS.find(([t]) => t <= left + 1e-6);
    if (!d) break;
    out.push({ ticks: d[0], duration: d[1], dots: d[2] });
    left -= d[0];
  }
  return out;
}

function keyFor(note: ExerciseNote): PlannedKey {
  const info = INSTRUMENT_INFO[note.instrument];
  return {
    instrument: note.instrument,
    key: info.key,
    head: info.head,
    open: info.open,
    accent: note.velocity >= 120,
    ghost: note.velocity <= 50,
    line: info.line,
  };
}

function chordKeys(notes: ExerciseNote[]): PlannedKey[] {
  const sorted = [...notes].sort((a, b) => byStaffOrder(a.instrument, b.instrument));
  const byKey = new Map<string, PlannedKey>();
  for (const n of sorted) {
    const k = keyFor(n);
    const existing = byKey.get(k.key);
    if (existing) {
      // Two instruments on one staff position (e.g. open + closed hi-hat): keep one head.
      existing.open ||= k.open;
      existing.accent ||= k.accent;
      continue;
    }
    byKey.set(k.key, k);
  }
  return [...byKey.values()].sort((a, b) => a.line - b.line);
}

interface BeatPlan {
  beat: number;
  start: number; // relative to bar
  end: number;
  empty: boolean;
  triplet: boolean;
  events: PlannedEvent[];
}

function planBeat(
  beat: number,
  start: number,
  end: number,
  barStart: number,
  onsets: Map<number, ExerciseNote[]>,
): BeatPlan {
  const len = end - start;
  let ticks = [...onsets.keys()].filter((t) => t >= start && t < end).sort((a, b) => a - b);
  const plan: BeatPlan = { beat, start, end, empty: ticks.length === 0, triplet: false, events: [] };
  if (plan.empty) return plan;

  // Which subdivision grid does this beat use?
  let scale = 1;
  let groups = new Map<number, ExerciseNote[]>();
  const binary = ticks.every((t) => (t - start) % GRID === 0) && len % GRID === 0;
  const simpleBeat = [240, 480, 960].includes(len);
  const triplet = !binary && simpleBeat && ticks.every((t) => (t - start) % (len / 6) === 0);
  if (binary || triplet) {
    for (const t of ticks) groups.set(t, onsets.get(t)!);
    if (triplet) {
      scale = 1.5;
      plan.triplet = true;
    }
  } else {
    // Unusual timing: quantize to the 64th grid for display.
    groups = new Map();
    for (const t of ticks) {
      const q = Math.min(end - GRID, start + Math.round((t - start) / GRID) * GRID);
      groups.set(q, [...(groups.get(q) ?? []), ...onsets.get(t)!]);
    }
  }
  ticks = [...groups.keys()].sort((a, b) => a - b);

  const push = (kind: EventKind, at: number, nominal: number, keys: PlannedKey[]): number => {
    let cursor = at;
    splitDuration(nominal).forEach((piece, i) => {
      const real = piece.ticks / scale;
      plan.events.push({
        kind: i === 0 ? kind : 'rest',
        tick: barStart + cursor,
        ticks: real,
        duration: piece.duration,
        dots: piece.dots,
        keys: i === 0 ? keys : [],
        beat,
      });
      cursor += real;
    });
    return cursor;
  };

  if (ticks[0] > start) push('rest', start, (ticks[0] - start) * scale, []);
  ticks.forEach((t, i) => {
    const next = i + 1 < ticks.length ? ticks[i + 1] : end;
    push('note', t, (next - t) * scale, chordKeys(groups.get(t)!));
  });
  return plan;
}

function planVoice(
  name: VoiceName,
  notes: ExerciseNote[],
  barStart: number,
  barLen: number,
  beatTicks: number,
  opts: Required<PlanOptions>,
): PlannedVoice {
  const onsets = new Map<number, ExerciseNote[]>();
  for (const n of notes) {
    const rel = n.tick - barStart;
    onsets.set(rel, [...(onsets.get(rel) ?? []), n]);
  }

  const beats: BeatPlan[] = [];
  for (let b = 0, s = 0; s < barLen; b++, s += beatTicks) {
    beats.push(planBeat(b, s, Math.min(s + beatTicks, barLen), barStart, onsets));
  }

  const voice: PlannedVoice = { name, events: [], beams: [], tuplets: [] };
  const hideEmpty = name === 'feet' && opts.hideFeetBeatRests;

  if (beats.every((b) => b.empty)) {
    if (hideEmpty) {
      for (const b of beats) {
        for (const p of splitDuration(b.end - b.start)) {
          voice.events.push({ kind: 'space', tick: barStart + b.start, ticks: p.ticks, duration: p.duration, dots: p.dots, keys: [], beat: b.beat });
        }
      }
    } else {
      voice.events.push({ kind: 'rest', tick: barStart, ticks: barLen, duration: 'w', dots: 0, keys: [], beat: 0, wholeBar: true });
    }
    return voice;
  }

  for (let i = 0; i < beats.length; ) {
    const b = beats[i];
    if (!b.empty) {
      const first = voice.events.length;
      voice.events.push(...b.events);
      const idx = b.events.map((_, k) => first + k);
      if (b.triplet) voice.tuplets.push(idx);
      // Beam runs of consecutive flagged notes inside the beat.
      let run: number[] = [];
      const flush = () => {
        if (run.length >= 2) voice.beams.push(run);
        run = [];
      };
      for (const k of idx) {
        const e = voice.events[k];
        if (e.kind === 'note' && BEAMABLE.has(e.duration)) run.push(k);
        else flush();
      }
      flush();
      i++;
      continue;
    }
    // Run of empty beats: merge aligned pairs/fours into one longer rest where a single value exists.
    let runEnd = i;
    while (runEnd < beats.length && beats[runEnd].empty) runEnd++;
    let k = i;
    while (k < runEnd) {
      let span = 1;
      for (const cand of [4, 2]) {
        if (k % cand === 0 && k + cand <= runEnd && beats[k + cand - 1].end - beats[k].start === cand * beatTicks && durationFor(cand * beatTicks)) {
          span = cand;
          break;
        }
      }
      const start = beats[k].start;
      const len = beats[k + span - 1].end - start;
      let cursor = start;
      for (const p of splitDuration(len)) {
        voice.events.push({ kind: hideEmpty ? 'space' : 'rest', tick: barStart + cursor, ticks: p.ticks, duration: p.duration, dots: p.dots, keys: [], beat: beats[k].beat });
        cursor += p.ticks;
      }
      k += span;
    }
    i = runEnd;
  }
  return voice;
}

export function planNotation(ex: Exercise, options: PlanOptions = {}): PlannedBar[] {
  const opts: Required<PlanOptions> = { hideFeetBeatRests: options.hideFeetBeatRests ?? true };
  const bt = barTicks(ex);
  const bars: PlannedBar[] = [];
  for (let i = 0; i < ex.bars; i++) {
    const startTick = i * bt;
    const endTick = startTick + bt;
    const inBar = ex.notes.filter((n) => n.tick >= startTick && n.tick < endTick);
    const hands = inBar.filter((n) => INSTRUMENT_INFO[n.instrument].voice === 'hands');
    const feet = inBar.filter((n) => INSTRUMENT_INFO[n.instrument].voice === 'feet');
    bars.push({
      index: i,
      startTick,
      endTick,
      hands: planVoice('hands', hands, startTick, bt, ex.beatTicks, opts),
      feet: planVoice('feet', feet, startTick, bt, ex.beatTicks, opts),
    });
  }
  return bars;
}

/** Compact text form for tests and debugging: "8d:kick", "q:rest", "qd:space", "8:hihatClosed+snare". */
export function describeVoice(v: PlannedVoice): string[] {
  return v.events.map((e) => {
    const d = e.duration + (e.dots ? 'd'.repeat(e.dots) : '');
    if (e.kind === 'note') return `${d}:${e.keys.map((k) => k.instrument).join('+')}`;
    return `${d}:${e.kind}`;
  });
}
