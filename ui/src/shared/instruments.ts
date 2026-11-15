// Instrument vocabulary (mirrors engine/core/include/drums/types.h) and how each one is drawn.

export const INSTRUMENTS = [
  'kick',
  'snare',
  'snareRim',
  'hihatClosed',
  'hihatOpen',
  'hihatPedal',
  'tom1',
  'tom2',
  'tom3',
  'crash',
  'ride',
  'rideBell',
] as const;

export type Instrument = (typeof INSTRUMENTS)[number];

export function isInstrument(name: unknown): name is Instrument {
  return typeof name === 'string' && (INSTRUMENTS as readonly string[]).includes(name);
}

export type NoteHead = 'normal' | 'x' | 'diamond';
export type VoiceName = 'hands' | 'feet';

export interface InstrumentInfo {
  label: string;
  short: string;
  /** VexFlow key (percussion clef = treble positions). */
  key: string;
  head: NoteHead;
  /** Draw a small "o" above (open hi-hat). */
  open: boolean;
  voice: VoiceName;
  /** VexFlow staff line: 0 = C4, 1 = bottom line (E4), 3 = middle line, 5 = top line (F5). */
  line: number;
  /** Top-to-bottom order for lanes and pads (matches the staff, high to low). */
  order: number;
}

// Standard drum notation positions.
export const INSTRUMENT_INFO: Record<Instrument, InstrumentInfo> = {
  crash: { label: 'Crash', short: 'Crash', key: 'a/5', head: 'x', open: false, voice: 'hands', line: 6, order: 0 },
  hihatOpen: { label: 'Hi-hat open', short: 'HH open', key: 'g/5', head: 'x', open: true, voice: 'hands', line: 5.5, order: 1 },
  hihatClosed: { label: 'Hi-hat', short: 'Hi-hat', key: 'g/5', head: 'x', open: false, voice: 'hands', line: 5.5, order: 2 },
  ride: { label: 'Ride', short: 'Ride', key: 'f/5', head: 'x', open: false, voice: 'hands', line: 5, order: 3 },
  rideBell: { label: 'Ride bell', short: 'Bell', key: 'f/5', head: 'diamond', open: false, voice: 'hands', line: 5, order: 4 },
  tom1: { label: 'Tom 1', short: 'Tom 1', key: 'e/5', head: 'normal', open: false, voice: 'hands', line: 4.5, order: 5 },
  tom2: { label: 'Tom 2', short: 'Tom 2', key: 'd/5', head: 'normal', open: false, voice: 'hands', line: 4, order: 6 },
  snare: { label: 'Snare', short: 'Snare', key: 'c/5', head: 'normal', open: false, voice: 'hands', line: 3.5, order: 7 },
  snareRim: { label: 'Snare rim', short: 'Rim', key: 'c/5', head: 'x', open: false, voice: 'hands', line: 3.5, order: 8 },
  tom3: { label: 'Floor tom', short: 'Floor', key: 'a/4', head: 'normal', open: false, voice: 'hands', line: 2.5, order: 9 },
  kick: { label: 'Kick', short: 'Kick', key: 'f/4', head: 'normal', open: false, voice: 'feet', line: 1.5, order: 10 },
  hihatPedal: { label: 'Hi-hat pedal', short: 'HH pedal', key: 'd/4', head: 'x', open: false, voice: 'feet', line: 0.5, order: 11 },
};

/** Index in the engine's enum order (used for sorting notes at the same tick, like the C++ core). */
export function instrumentIndex(i: Instrument): number {
  return INSTRUMENTS.indexOf(i);
}

export function byStaffOrder(a: Instrument, b: Instrument): number {
  return INSTRUMENT_INFO[a].order - INSTRUMENT_INFO[b].order;
}

/** Instruments in staff order (top to bottom). */
export const INSTRUMENTS_BY_STAFF: Instrument[] = [...INSTRUMENTS].sort(byStaffOrder);
