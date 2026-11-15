// Mock drum engine: implements docs/PROTOCOL.md on ws://127.0.0.1:7878 so the UI can be developed
// without the C++ engine. Simulates transport, count-in, loop, a drummer hitting the exercise's notes
// with random timing (plus misses and extras), loop summaries, free hits while stopped, recording.
//
//   npm run mock -- [--port 7878] [--root <repo>] [--exercise exercises/groove-12-8.json]
//                   [--autoplay (play when the first UI talks to it)] [--bpm 60] [--sd 15] [--miss 0.05] [--extra 0.5] [--seed 1] [--no-idle-hits]
import { existsSync, readdirSync, readFileSync } from 'node:fs';
import { isAbsolute, join, relative, resolve } from 'node:path';
import { performance } from 'node:perf_hooks';
import { WebSocket, WebSocketServer } from 'ws';
import { barTicks, lengthTicks, msPerTick, parseExercise } from '../src/shared/exercise';
import { INSTRUMENTS, isInstrument, type Instrument } from '../src/shared/instruments';
import {
  LIMITS,
  ON_TIME_MS,
  PROTOCOL_VERSION,
  type EngineState,
  type Exercise,
  type ExerciseNote,
  type Grade,
  type KitInfo,
  type Stats,
} from '../src/shared/protocol';

// ---------- args ----------
function argValue(name: string): string | undefined {
  const i = process.argv.indexOf(`--${name}`);
  return i >= 0 ? process.argv[i + 1] : undefined;
}
const hasFlag = (name: string) => process.argv.includes(`--${name}`);

const ROOT = resolve(argValue('root') ?? resolve(__dirname, '../..'));
const PORT = Number(argValue('port') ?? 7878);
const SD_MS = Number(argValue('sd') ?? 15);
const MISS_P = Number(argValue('miss') ?? 0.05);
const EXTRA_PER_LOOP = Number(argValue('extra') ?? 0.5);
const IDLE_HITS = !hasFlag('no-idle-hits');
const SYSTEM_LATENCY_MS = 6; // simulated constant lateness, so the latency offset control has an effect
const MAX_WINDOW_MS = 150;

// ---------- seeded random ----------
let seed = Number(argValue('seed') ?? Date.now() % 1e9) >>> 0;
function rand(): number {
  seed = (seed + 0x6d2b79f5) >>> 0;
  let t = seed;
  t = Math.imul(t ^ (t >>> 15), t | 1);
  t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
  return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
}
function gauss(): number {
  const u = 1 - rand();
  const v = rand();
  return Math.sqrt(-2 * Math.log(u)) * Math.cos(2 * Math.PI * v);
}
const clamp = (v: number, lo: number, hi: number) => Math.min(hi, Math.max(lo, v));

// ---------- state ----------
function findKits(): KitInfo[] {
  const kits: KitInfo[] = [];
  const dir = join(ROOT, 'kits');
  if (existsSync(dir)) {
    for (const name of readdirSync(dir, { withFileTypes: true })) {
      const file = join(dir, name.name, 'kit.json');
      if (!name.isDirectory() || !existsSync(file)) continue;
      let title = name.name;
      try {
        const j = JSON.parse(readFileSync(file, 'utf8'));
        if (typeof j.name === 'string') title = j.name;
      } catch {
        /* keep folder name */
      }
      kits.push({ name: title, path: relative(ROOT, file) });
    }
  }
  if (kits.length === 0) {
    kits.push({ name: 'Mock Studio Kit', path: 'kits/mock-studio/kit.json' });
    kits.push({ name: 'Mock Jazz Kit', path: 'kits/mock-jazz/kit.json' });
  }
  return kits;
}

function loadPadMap(): Record<string, string> {
  try {
    const j = JSON.parse(readFileSync(join(ROOT, 'kits/pad-map.json'), 'utf8'));
    if (j && typeof j.notes === 'object') return { ...j.notes };
  } catch {
    /* fall through */
  }
  return { '36': 'kick', '38': 'snare', '42': 'hihatClosed', '46': 'hihatOpen', '44': 'hihatPedal', '49': 'crash', '51': 'ride' };
}

const kits = findKits();
const state: EngineState = {
  type: 'state',
  playing: false,
  recording: false,
  bpm: 60,
  loop: true,
  countInBars: 1,
  click: { on: true, volume: 0.8, subdivisions: true },
  groove: { on: false, volume: 0.7 },
  latencyOffsetMs: 0,
  exercise: null,
  kit: kits[0],
  kits,
  mixer: Object.fromEntries([['master', 0.8], ...INSTRUMENTS.map((i) => [i, 1.0])]),
  padMap: loadPadMap(),
};

function resolvePath(p: string): string {
  return isAbsolute(p) ? p : join(ROOT, p);
}

function loadExercise(path: string): Exercise {
  const ex = parseExercise(readFileSync(resolvePath(path), 'utf8'));
  state.exercise = ex;
  state.bpm = ex.defaultBpm;
  return ex;
}

// ---------- clients ----------
const wss = new WebSocketServer({ host: '127.0.0.1', port: PORT });
const send = (ws: WebSocket, msg: object) => ws.readyState === WebSocket.OPEN && ws.send(JSON.stringify(msg));
const broadcast = (msg: object) => {
  const text = JSON.stringify(msg);
  for (const c of wss.clients) if (c.readyState === WebSocket.OPEN) c.send(text);
};
const sendState = () => broadcast(state);

// ---------- transport + simulated drummer ----------
interface Planned {
  instrument: Instrument;
  note: ExerciseNote | null; // null = extra
  loop: number;
  expectedPos: number; // absolute posTicks of the expected note (or of the extra hit)
  errorMs: number; // simulated raw error (before latency compensation)
  miss: boolean;
  done: boolean;
  windowTicks: number;
}

let startPerf = 0;
let startPos = 0;
let timer: NodeJS.Timeout | null = null;
let planned: Planned[] = [];
let generatedLoops = new Set<number>();
let summarizedLoops = new Set<number>();
const bias: Partial<Record<Instrument, number>> = {};
const loopResults = new Map<number, { instrument: Instrument; kind: 'hit' | 'miss' | 'extra'; errorMs?: number }[]>();

function pos(): number {
  const ex = state.exercise!;
  return startPos + (performance.now() - startPerf) / msPerTick(ex, state.bpm);
}

function windowTicksFor(ex: Exercise, note: ExerciseNote): number {
  const L = lengthTicks(ex);
  const same = ex.notes.filter((n) => n.instrument === note.instrument).map((n) => n.tick);
  let nearest = Infinity;
  for (const t of same) {
    if (t === note.tick) continue;
    for (const d of [t - note.tick, t - note.tick + L, t - note.tick - L]) nearest = Math.min(nearest, Math.abs(d));
  }
  const maxTicks = MAX_WINDOW_MS / msPerTick(ex, state.bpm);
  return Math.min(maxTicks, nearest / 2);
}

function generateLoop(loop: number) {
  const ex = state.exercise!;
  if (generatedLoops.has(loop) || (!state.loop && loop > 0)) return;
  generatedLoops.add(loop);
  const L = lengthTicks(ex);
  for (const note of ex.notes) {
    bias[note.instrument] ??= gauss() * 6;
    planned.push({
      instrument: note.instrument,
      note,
      loop,
      expectedPos: loop * L + note.tick,
      errorMs: bias[note.instrument]! + gauss() * SD_MS + SYSTEM_LATENCY_MS,
      miss: rand() < MISS_P,
      done: false,
      windowTicks: windowTicksFor(ex, note),
    });
  }
  // Occasional extra hit, placed away from that instrument's notes.
  if (rand() < EXTRA_PER_LOOP && ex.notes.length) {
    const instrument = ex.notes[Math.floor(rand() * ex.notes.length)].instrument;
    const busy = ex.notes.filter((n) => n.instrument === instrument).map((n) => n.tick);
    for (let tries = 0; tries < 20; tries++) {
      const t = Math.floor(rand() * L);
      const far = busy.every((b) => Math.min(Math.abs(b - t), L - Math.abs(b - t)) * msPerTick(ex, state.bpm) > MAX_WINDOW_MS + 20);
      if (far) {
        planned.push({ instrument, note: null, loop, expectedPos: loop * L + t, errorMs: 0, miss: false, done: false, windowTicks: 0 });
        break;
      }
    }
  }
}

function record(loop: number, r: { instrument: Instrument; kind: 'hit' | 'miss' | 'extra'; errorMs?: number }) {
  const list = loopResults.get(loop) ?? [];
  list.push(r);
  loopResults.set(loop, list);
}

const GM_NOTE: Record<Instrument, number> = {
  kick: 36, snare: 38, snareRim: 37, hihatClosed: 42, hihatOpen: 46, hihatPedal: 44,
  tom1: 48, tom2: 45, tom3: 43, crash: 49, ride: 51, rideBell: 53,
};

function noteFor(instrument: Instrument): number {
  if (state.padMap[String(GM_NOTE[instrument])] === instrument) return GM_NOTE[instrument];
  const e = Object.entries(state.padMap).find(([, v]) => v === instrument);
  return e ? Number(e[0]) : GM_NOTE[instrument];
}

function stats(items: { kind: string; errorMs?: number }[]): Stats {
  const errs = items.filter((i) => i.kind === 'hit').map((i) => i.errorMs!);
  const mean = errs.length ? errs.reduce((s, v) => s + v, 0) / errs.length : 0;
  const sd = errs.length ? Math.sqrt(errs.reduce((s, v) => s + (v - mean) ** 2, 0) / errs.length) : 0;
  return {
    hits: errs.length,
    missed: items.filter((i) => i.kind === 'miss').length,
    extra: items.filter((i) => i.kind === 'extra').length,
    meanErrorMs: Math.round(mean * 10) / 10,
    stdDevMs: Math.round(sd * 10) / 10,
  };
}

function step() {
  const ex = state.exercise;
  if (!ex || !state.playing) return;
  const L = lengthTicks(ex);
  const mpt = msPerTick(ex, state.bpm);
  const p = pos();
  const maxWindow = MAX_WINDOW_MS / mpt;
  const loop = Math.floor(p / L);
  generateLoop(Math.max(0, loop));
  generateLoop(Math.max(0, loop + 1));

  // Fire hits whose time has come, report misses whose window closed.
  for (const h of planned) {
    if (h.done) continue;
    if (h.note === null) {
      if (p >= h.expectedPos) {
        h.done = true;
        const hl = Math.floor(h.expectedPos / L);
        broadcast({
          type: 'hit', instrument: h.instrument, note: noteFor(h.instrument), velocity: 70 + Math.floor(rand() * 40),
          loop: hl, tickInLoop: h.expectedPos - hl * L, expectedTick: null, errorMs: null, grade: 'extra',
        });
        record(h.loop, { instrument: h.instrument, kind: 'extra' });
      }
      continue;
    }
    const reported = Math.round((h.errorMs - state.latencyOffsetMs) * 10) / 10;
    if (h.miss || Math.abs(reported) / mpt > h.windowTicks) {
      if (p > h.expectedPos + h.windowTicks) {
        h.done = true;
        broadcast({ type: 'missed', instrument: h.instrument, loop: h.loop, expectedTick: h.note.tick });
        record(h.loop, { instrument: h.instrument, kind: 'miss' });
      }
      continue;
    }
    // the physical hit happens errorMs after the note; compensation shifts its reported time
    const hitPos = h.expectedPos + reported / mpt;
    if (p >= hitPos) {
      h.done = true;
      const hl = Math.floor(hitPos / L);
      const grade: Grade = Math.abs(reported) <= ON_TIME_MS ? 'onTime' : reported < 0 ? 'early' : 'late';
      broadcast({
        type: 'hit', instrument: h.instrument, note: noteFor(h.instrument),
        velocity: clamp(Math.round(h.note.velocity + gauss() * 8), 1, 127),
        loop: hl, tickInLoop: hitPos - hl * L, expectedTick: h.note.tick,
        errorMs: reported, grade,
      });
      record(h.loop, { instrument: h.instrument, kind: 'hit', errorMs: reported });
    }
  }
  planned = planned.filter((h) => !h.done);

  // Loop summaries once every window of a loop has closed.
  for (const [l, items] of loopResults) {
    if (summarizedLoops.has(l) || p < (l + 1) * L + maxWindow) continue;
    summarizedLoops.add(l);
    const instruments: Record<string, Stats> = {};
    for (const name of new Set(items.map((i) => i.instrument))) {
      instruments[name] = stats(items.filter((i) => i.instrument === name));
    }
    broadcast({ type: 'loopSummary', loop: l, instruments, total: stats(items) });
    loopResults.delete(l);
  }

  // Without loop the transport parks at the end while the last windows close, then stops.
  const shown = state.loop ? p : Math.min(p, L - 1e-3);
  const posLoop = Math.floor(shown / L);
  broadcast({ type: 'position', playing: true, posTicks: shown, loop: posLoop, tickInLoop: shown - posLoop * L, timeMs: performance.now() });

  if (!state.loop && p >= L + maxWindow) stop();
}

function play() {
  const ex = state.exercise;
  if (!ex || state.playing) return;
  state.playing = true;
  startPos = -state.countInBars * barTicks(ex);
  startPerf = performance.now();
  planned = [];
  generatedLoops = new Set();
  summarizedLoops = new Set();
  loopResults.clear();
  timer = setInterval(step, 1000 / 60);
  sendState();
}

function stop() {
  if (!state.playing) return;
  const ex = state.exercise!;
  const p = Math.min(pos(), state.loop ? Infinity : lengthTicks(ex) - 1e-3);
  state.playing = false;
  if (timer) clearInterval(timer);
  timer = null;
  planned = [];
  const L = lengthTicks(ex);
  const loop = Math.floor(p / L);
  broadcast({ type: 'position', playing: false, posTicks: p, loop, tickInLoop: p - loop * L, timeMs: performance.now() });
  sendState();
}

// Noodling while stopped: "free" hits.
function idleHit() {
  if (IDLE_HITS && !state.playing && state.exercise && wss.clients.size > 0) {
    const ns = state.exercise.notes;
    const instrument = ns.length ? ns[Math.floor(rand() * ns.length)].instrument : 'snare';
    broadcast({
      type: 'hit', instrument, note: noteFor(instrument), velocity: 60 + Math.floor(rand() * 60),
      loop: null, tickInLoop: null, expectedTick: null, errorMs: null, grade: 'free',
    });
  }
  setTimeout(idleHit, 900 + rand() * 2200);
}

// ---------- commands ----------
type Cmd = Record<string, unknown> & { type?: unknown };
const num = (v: unknown, lo: number, hi: number, field: string): number => {
  if (typeof v !== 'number' || !Number.isFinite(v) || v < lo || v > hi) throw new Error(`"${field}" must be a number in ${lo}..${hi}`);
  return v;
};
const bool = (v: unknown, field: string): boolean => {
  if (typeof v !== 'boolean') throw new Error(`"${field}" must be true or false`);
  return v;
};

function handle(ws: WebSocket, cmd: Cmd) {
  const ex = state.exercise;
  switch (cmd.type) {
    case 'play':
      if (!ex) throw new Error('no exercise loaded');
      play();
      return;
    case 'stop':
      stop();
      return;
    case 'setBpm': {
      const bpm = num(cmd.bpm, LIMITS.bpm.min, LIMITS.bpm.max, 'bpm');
      if (state.playing && ex) {
        startPos = pos();
        startPerf = performance.now();
      }
      state.bpm = bpm;
      break;
    }
    case 'setLoop':
      state.loop = bool(cmd.on, 'on');
      break;
    case 'setCountIn': {
      const bars = num(cmd.bars, 0, 2, 'bars');
      if (!Number.isInteger(bars)) throw new Error('"bars" must be 0, 1 or 2');
      state.countInBars = bars;
      break;
    }
    case 'setClick':
      if (cmd.on !== undefined) state.click.on = bool(cmd.on, 'on');
      if (cmd.volume !== undefined) state.click.volume = num(cmd.volume, 0, 1, 'volume');
      if (cmd.subdivisions !== undefined) state.click.subdivisions = bool(cmd.subdivisions, 'subdivisions');
      break;
    case 'setGroove':
      if (cmd.on !== undefined) state.groove.on = bool(cmd.on, 'on');
      if (cmd.volume !== undefined) state.groove.volume = num(cmd.volume, 0, 1, 'volume');
      break;
    case 'loadExercise': {
      if (typeof cmd.path !== 'string') throw new Error('"path" must be a string');
      stop();
      try {
        loadExercise(cmd.path);
      } catch (e) {
        throw new Error(`could not load ${cmd.path}: ${(e as Error).message}`);
      }
      break;
    }
    case 'loadKit': {
      if (typeof cmd.path !== 'string') throw new Error('"path" must be a string');
      const kit = state.kits.find((k) => k.path === cmd.path || resolvePath(k.path) === resolvePath(cmd.path as string));
      if (!kit) throw new Error(`unknown kit ${cmd.path}`);
      state.kit = kit;
      break;
    }
    case 'setMixer': {
      const ch = cmd.channel;
      if (typeof ch !== 'string' || (ch !== 'master' && !isInstrument(ch))) throw new Error('"channel" must be "master" or an instrument');
      state.mixer[ch] = num(cmd.volume, LIMITS.mixer.min, LIMITS.mixer.max, 'volume');
      break;
    }
    case 'setLatencyOffset':
      state.latencyOffsetMs = num(cmd.ms, LIMITS.latencyOffsetMs.min, LIMITS.latencyOffsetMs.max, 'ms');
      break;
    case 'startRecording':
      state.recording = true;
      break;
    case 'stopRecording':
      if (state.recording) {
        state.recording = false;
        const stamp = new Date().toISOString().replace(/[:.]/g, '-').slice(0, 19);
        broadcast({ type: 'recordingSaved', path: join(ROOT, 'recordings', `${stamp}-${ex?.id ?? 'take'}.mid`) });
      }
      break;
    case 'setPadMap': {
      const note = num(cmd.note, 0, 127, 'note');
      if (cmd.instrument === null) delete state.padMap[String(note)];
      else if (isInstrument(cmd.instrument)) state.padMap[String(note)] = cmd.instrument;
      else throw new Error('"instrument" must be an instrument name or null');
      break;
    }
    case 'getState':
      send(ws, state);
      return;
    default:
      throw new Error(`unknown command type ${JSON.stringify(cmd.type)}`);
  }
  sendState();
}

let autoplayPending = hasFlag('autoplay');

wss.on('connection', (ws) => {
  send(ws, { type: 'hello', protocol: PROTOCOL_VERSION, sampleRate: 48000, bufferFrames: 64, outputLatencyMs: 2.7 });
  send(ws, state);
  ws.on('message', (data) => {
    if (autoplayPending) {
      // --autoplay: start (with count-in) on the first message from a UI (not on bare port probes)
      autoplayPending = false;
      setTimeout(play, 50);
    }
    let cmd: Cmd;
    try {
      cmd = JSON.parse(String(data));
      if (typeof cmd !== 'object' || cmd === null) throw new Error('not an object');
    } catch {
      send(ws, { type: 'error', message: 'malformed JSON' });
      return;
    }
    try {
      handle(ws, cmd);
    } catch (e) {
      send(ws, { type: 'error', message: (e as Error).message });
    }
  });
});

wss.on('listening', () => {
  const exPath = argValue('exercise') ?? 'exercises/groove-12-8.json';
  try {
    loadExercise(exPath);
  } catch (e) {
    console.error(`[mock] ${(e as Error).message}`);
  }
  if (argValue('bpm')) state.bpm = clamp(Number(argValue('bpm')), LIMITS.bpm.min, LIMITS.bpm.max);
  console.log(`[mock] drums mock engine on ws://127.0.0.1:${PORT}  root=${ROOT}  exercise=${state.exercise?.id ?? 'none'}`);
  setTimeout(idleHit, 1500);
});

wss.on('error', (e) => {
  console.error(`[mock] ${e.message}`);
  process.exit(1);
});

for (const sig of ['SIGINT', 'SIGTERM'] as const) {
  process.on(sig, () => {
    wss.close();
    process.exit(0);
  });
}
