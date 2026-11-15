// Grade colours and plain-language descriptions of timing.
import { ON_TIME_MS, type Grade, type Stats } from './protocol';

export const GRADE_COLORS = {
  onTime: '#3ad08a',
  amber: '#ffb224',
  red: '#ff4d5e',
  extra: '#b58cff',
  free: '#9aa4b8',
  missed: '#ff4d5e',
} as const;

/** |errorMs| at which early/late reaches full red. */
export const RED_AT_MS = 60;

function hexToRgb(hex: string): [number, number, number] {
  const h = hex.replace('#', '');
  return [0, 2, 4].map((i) => parseInt(h.slice(i, i + 2), 16)) as [number, number, number];
}

function rgbToHex([r, g, b]: [number, number, number]): string {
  return '#' + [r, g, b].map((v) => Math.round(v).toString(16).padStart(2, '0')).join('');
}

export function mixColor(a: string, b: string, t: number): string {
  const k = Math.min(1, Math.max(0, t));
  const ca = hexToRgb(a);
  const cb = hexToRgb(b);
  return rgbToHex([0, 1, 2].map((i) => ca[i] + (cb[i] - ca[i]) * k) as [number, number, number]);
}

/** Colour for an error in ms regardless of grade: green inside the on-time window, then amber -> red. */
export function errorColor(errorMs: number): string {
  const a = Math.abs(errorMs);
  if (a <= ON_TIME_MS) return GRADE_COLORS.onTime;
  return mixColor(GRADE_COLORS.amber, GRADE_COLORS.red, (a - ON_TIME_MS) / (RED_AT_MS - ON_TIME_MS));
}

export function gradeColor(grade: Grade | 'missed', errorMs?: number | null): string {
  switch (grade) {
    case 'onTime':
      return GRADE_COLORS.onTime;
    case 'early':
    case 'late':
      return errorMs == null ? GRADE_COLORS.amber : errorColor(Math.max(Math.abs(errorMs), ON_TIME_MS + 0.001));
    case 'extra':
      return GRADE_COLORS.extra;
    case 'missed':
      return GRADE_COLORS.missed;
    case 'free':
    default:
      return GRADE_COLORS.free;
  }
}

export function formatErrorMs(errorMs: number): string {
  const r = Math.round(errorMs);
  return r > 0 ? `+${r}` : `${r}`;
}

/** "12 ms late", "8 ms early", "on time". */
export function describeError(errorMs: number): string {
  const r = Math.round(Math.abs(errorMs));
  if (r === 0) return 'dead on';
  return `${r} ms ${errorMs < 0 ? 'early' : 'late'}`;
}

/** One plain sentence for a loop's stats of one instrument, e.g. "kick 12 ms late on average". */
export function describeStats(name: string, s: Stats): string {
  if (s.hits === 0) {
    if (s.missed > 0) return `${name}: all ${s.missed} missed`;
    if (s.extra > 0) return `${name}: ${s.extra} extra hit${s.extra === 1 ? '' : 's'}`;
    return `${name}: no hits`;
  }
  const mean = Math.round(Math.abs(s.meanErrorMs));
  let text: string;
  if (mean <= 2) text = `${name} right on the beat on average`;
  else text = `${name} ${mean} ms ${s.meanErrorMs < 0 ? 'early' : 'late'} on average`;
  const sd = Math.round(s.stdDevMs);
  if (s.hits > 1) text += sd <= 8 ? ', very steady' : sd <= 18 ? `, spread ±${sd} ms` : `, uneven (±${sd} ms)`;
  return text;
}
