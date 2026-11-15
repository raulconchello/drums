// Exercise file parser (docs/EXERCISE_FORMAT.md): grid strings and/or explicit notes -> canonical form.
import { instrumentIndex, isInstrument, type Instrument } from './instruments';
import type { Exercise, ExerciseNote } from './protocol';

export class ExerciseError extends Error {
  constructor(message: string) {
    super(message);
    this.name = 'ExerciseError';
  }
}

const GRID_VELOCITY: Record<string, number> = { x: 100, X: 124, g: 40 };
const GRID_REST = new Set(['.', '-']);
const GRID_IGNORED = new Set(['|', ' ']);

export function barTicks(ex: Pick<Exercise, 'timeSignature' | 'ppq'>): number {
  const [num, den] = ex.timeSignature;
  return (num * ex.ppq * 4) / den;
}

export function lengthTicks(ex: Pick<Exercise, 'timeSignature' | 'ppq' | 'bars'>): number {
  return barTicks(ex) * ex.bars;
}

export function msPerTick(ex: Pick<Exercise, 'beatTicks'>, bpm: number): number {
  return 60000 / (bpm * ex.beatTicks);
}

/** Default beat: one denominator note, or a dotted one for compound meters (6/8, 9/8, 12/8...). */
export function defaultBeatTicks(timeSignature: [number, number], ppq = 480): number {
  const [num, den] = timeSignature;
  const denTicks = (ppq * 4) / den;
  const compound = den >= 8 && num >= 6 && num % 3 === 0;
  return compound ? denTicks * 3 : denTicks;
}

/** Removes // line comments outside strings, so the commented examples in the docs parse too. */
export function stripJsonComments(text: string): string {
  let out = '';
  let inString = false;
  for (let i = 0; i < text.length; i++) {
    const c = text[i];
    if (inString) {
      out += c;
      if (c === '\\') {
        out += text[++i] ?? '';
      } else if (c === '"') {
        inString = false;
      }
    } else if (c === '"') {
      inString = true;
      out += c;
    } else if (c === '/' && text[i + 1] === '/') {
      while (i < text.length && text[i] !== '\n') i++;
      out += '\n';
    } else {
      out += c;
    }
  }
  return out;
}

function isInt(v: unknown): v is number {
  return typeof v === 'number' && Number.isInteger(v);
}

function positiveInt(obj: Record<string, unknown>, field: string, fallback: number): number {
  const v = obj[field];
  if (v === undefined) return fallback;
  if (!isInt(v) || v <= 0) throw new ExerciseError(`"${field}" must be a positive integer`);
  return v;
}

/** Parses exercise JSON text or an already-parsed object into the canonical form. Throws ExerciseError. */
export function parseExercise(input: string | unknown): Exercise {
  let raw: unknown = input;
  if (typeof input === 'string') {
    try {
      raw = JSON.parse(stripJsonComments(input));
    } catch (e) {
      throw new ExerciseError(`invalid JSON: ${(e as Error).message}`);
    }
  }
  if (typeof raw !== 'object' || raw === null || Array.isArray(raw)) {
    throw new ExerciseError('exercise must be a JSON object');
  }
  const obj = raw as Record<string, unknown>;

  const id = obj.id === undefined ? 'exercise' : obj.id;
  if (typeof id !== 'string' || id === '') throw new ExerciseError('"id" must be a non-empty string');
  const title = obj.title === undefined ? id : obj.title;
  if (typeof title !== 'string') throw new ExerciseError('"title" must be a string');

  let timeSignature: [number, number] = [4, 4];
  if (obj.timeSignature !== undefined) {
    const ts = obj.timeSignature;
    if (!Array.isArray(ts) || ts.length !== 2 || !isInt(ts[0]) || !isInt(ts[1]) || ts[0] < 1 || ts[0] > 32) {
      throw new ExerciseError('"timeSignature" must be [numerator, denominator]');
    }
    if (![1, 2, 4, 8, 16, 32].includes(ts[1])) {
      throw new ExerciseError('time signature denominator must be 1, 2, 4, 8, 16 or 32');
    }
    timeSignature = [ts[0], ts[1]];
  }

  const ppq = positiveInt(obj, 'ppq', 480);
  if (ppq !== 480) throw new ExerciseError('only "ppq": 480 is supported');
  const denTicks = (ppq * 4) / timeSignature[1];
  const beatTicks = positiveInt(obj, 'beatTicks', defaultBeatTicks(timeSignature, ppq));
  const subdivisionTicks = positiveInt(obj, 'subdivisionTicks', denTicks);
  const bars = positiveInt(obj, 'bars', 1);

  let defaultBpm = 80;
  if (obj.defaultBpm !== undefined) {
    if (typeof obj.defaultBpm !== 'number' || !(obj.defaultBpm > 0)) {
      throw new ExerciseError('"defaultBpm" must be a positive number');
    }
    defaultBpm = obj.defaultBpm;
  }

  const total = barTicks({ timeSignature, ppq }) * bars;
  const notes: ExerciseNote[] = [];

  if (obj.grid !== undefined) {
    if (typeof obj.grid !== 'object' || obj.grid === null || Array.isArray(obj.grid)) {
      throw new ExerciseError('"grid" must be an object of instrument -> string');
    }
    const gridTicks = positiveInt(obj, 'gridTicks', subdivisionTicks);
    const maxSteps = Math.floor(total / gridTicks);
    for (const [name, row] of Object.entries(obj.grid as Record<string, unknown>)) {
      if (!isInstrument(name)) throw new ExerciseError(`unknown instrument "${name}" in grid`);
      if (typeof row !== 'string') throw new ExerciseError(`grid row "${name}" must be a string`);
      let step = 0;
      for (const c of row) {
        if (GRID_IGNORED.has(c)) continue;
        if (step >= maxSteps) {
          throw new ExerciseError(`grid row "${name}" is longer than ${maxSteps} steps (bars * barTicks / gridTicks)`);
        }
        if (c in GRID_VELOCITY) {
          notes.push({ instrument: name, tick: step * gridTicks, velocity: GRID_VELOCITY[c] });
        } else if (!GRID_REST.has(c)) {
          throw new ExerciseError(`bad character "${c}" in grid row "${name}"`);
        }
        step++;
      }
    }
  }

  if (obj.notes !== undefined) {
    if (!Array.isArray(obj.notes)) throw new ExerciseError('"notes" must be an array');
    obj.notes.forEach((n: unknown, i: number) => {
      if (typeof n !== 'object' || n === null) throw new ExerciseError(`notes[${i}] must be an object`);
      const note = n as Record<string, unknown>;
      if (!isInstrument(note.instrument)) {
        throw new ExerciseError(`notes[${i}]: unknown instrument "${String(note.instrument)}"`);
      }
      if (!isInt(note.tick) || note.tick < 0) throw new ExerciseError(`notes[${i}]: "tick" must be an integer >= 0`);
      const velocity = note.velocity === undefined ? 100 : note.velocity;
      if (!isInt(velocity) || velocity < 1 || velocity > 127) {
        throw new ExerciseError(`notes[${i}]: "velocity" must be 1..127`);
      }
      notes.push({ instrument: note.instrument as Instrument, tick: note.tick, velocity });
    });
  }

  for (const n of notes) {
    if (n.tick >= total) {
      throw new ExerciseError(`${n.instrument} note at tick ${n.tick} is outside the exercise (${bars} bar(s) = ${total} ticks)`);
    }
  }

  // Merge (an explicit note replaces a grid note on the same instrument and tick) and sort.
  const merged = new Map<string, ExerciseNote>();
  for (const n of notes) merged.set(`${n.instrument}@${n.tick}`, n);
  const sorted = [...merged.values()].sort(
    (a, b) => a.tick - b.tick || instrumentIndex(a.instrument) - instrumentIndex(b.instrument),
  );

  return { id, title, timeSignature, ppq, beatTicks, subdivisionTicks, bars, defaultBpm, notes: sorted };
}

/** Instruments used by the exercise, in staff order is left to the caller. */
export function exerciseInstruments(ex: Exercise): Instrument[] {
  return [...new Set(ex.notes.map((n) => n.instrument))];
}
