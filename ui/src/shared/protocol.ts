// Engine <-> UI WebSocket protocol v1 (docs/PROTOCOL.md). Types only, plus a couple of tiny helpers.
import type { Instrument } from './instruments';

export const PROTOCOL_VERSION = 1;
export const DEFAULT_PORT = 7878;

export type Grade = 'onTime' | 'early' | 'late' | 'extra' | 'free';

export interface ExerciseNote {
  instrument: Instrument;
  tick: number;
  velocity: number;
}

/** Canonical exercise (what the engine sends in `state`). */
export interface Exercise {
  id: string;
  title: string;
  timeSignature: [number, number];
  ppq: number;
  beatTicks: number;
  subdivisionTicks: number;
  bars: number;
  defaultBpm: number;
  notes: ExerciseNote[];
}

export interface KitInfo {
  name: string;
  path: string;
}

export interface Stats {
  hits: number;
  missed: number;
  extra: number;
  meanErrorMs: number;
  stdDevMs: number;
}

export interface EngineState {
  type: 'state';
  playing: boolean;
  recording: boolean;
  bpm: number;
  loop: boolean;
  countInBars: number;
  click: { on: boolean; volume: number; subdivisions: boolean };
  groove: { on: boolean; volume: number };
  latencyOffsetMs: number;
  exercise: Exercise | null;
  kit: KitInfo | null;
  kits: KitInfo[];
  mixer: Record<string, number>;
  padMap: Record<string, string>;
}

export interface HelloMsg {
  type: 'hello';
  protocol: number;
  sampleRate: number;
  bufferFrames: number;
  outputLatencyMs: number;
}

export interface PositionMsg {
  type: 'position';
  playing: boolean;
  posTicks: number;
  loop: number;
  tickInLoop: number;
  timeMs: number;
}

export interface HitMsg {
  type: 'hit';
  instrument: Instrument | string;
  note: number;
  velocity: number;
  loop: number | null;
  tickInLoop: number | null;
  expectedTick: number | null;
  errorMs: number | null;
  grade: Grade;
}

export interface MissedMsg {
  type: 'missed';
  instrument: Instrument | string;
  loop: number;
  expectedTick: number;
}

export interface LoopSummaryMsg {
  type: 'loopSummary';
  loop: number;
  instruments: Record<string, Stats>;
  total: Stats;
}

export interface RecordingSavedMsg {
  type: 'recordingSaved';
  path: string;
}

export interface ErrorMsg {
  type: 'error';
  message: string;
}

export type EngineMessage =
  | HelloMsg
  | EngineState
  | PositionMsg
  | HitMsg
  | MissedMsg
  | LoopSummaryMsg
  | RecordingSavedMsg
  | ErrorMsg;

export type Command =
  | { type: 'play' }
  | { type: 'stop' }
  | { type: 'setBpm'; bpm: number }
  | { type: 'setLoop'; on: boolean }
  | { type: 'setCountIn'; bars: number }
  | { type: 'setClick'; on?: boolean; volume?: number; subdivisions?: boolean }
  | { type: 'setGroove'; on?: boolean; volume?: number }
  | { type: 'loadExercise'; path: string }
  | { type: 'loadKit'; path: string }
  | { type: 'setMixer'; channel: string; volume: number }
  | { type: 'setLatencyOffset'; ms: number }
  | { type: 'startRecording' }
  | { type: 'stopRecording' }
  | { type: 'setPadMap'; note: number; instrument: Instrument | null }
  | { type: 'getState' };

export const LIMITS = {
  bpm: { min: 20, max: 300 },
  countInBars: { min: 0, max: 2 },
  volume: { min: 0, max: 1 },
  mixer: { min: 0, max: 1.5 },
  latencyOffsetMs: { min: -50, max: 100 },
} as const;

export const ON_TIME_MS = 15;
