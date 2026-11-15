import { readFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { describe, expect, it } from 'vitest';
import { barTicks, defaultBeatTicks, ExerciseError, lengthTicks, msPerTick, parseExercise } from '../src/shared/exercise';

const repo = resolve(__dirname, '../..');
const groove = () => parseExercise(readFileSync(resolve(repo, 'exercises/groove-12-8.json'), 'utf8'));
const ticksOf = (ex: ReturnType<typeof groove>, instrument: string) =>
  ex.notes.filter((n) => n.instrument === instrument).map((n) => n.tick);

describe('parseExercise: groove-12-8.json', () => {
  const ex = groove();

  it('reads the header', () => {
    expect(ex.id).toBe('groove-12-8-bar23');
    expect(ex.timeSignature).toEqual([12, 8]);
    expect(ex.beatTicks).toBe(720);
    expect(ex.subdivisionTicks).toBe(240);
    expect(ex.bars).toBe(1);
    expect(ex.defaultBpm).toBe(60);
    expect(barTicks(ex)).toBe(2880);
    expect(lengthTicks(ex)).toBe(2880);
  });

  it('expands the grid to ticks', () => {
    expect(ticksOf(ex, 'hihatClosed')).toEqual(Array.from({ length: 12 }, (_, i) => i * 240));
    expect(ticksOf(ex, 'snare')).toEqual([960, 1440]);
    expect(ticksOf(ex, 'kick')).toEqual([0, 1200, 2640]);
    expect(ex.notes).toHaveLength(17);
    expect(ex.notes.every((n) => n.velocity === 100)).toBe(true);
  });

  it('sorts by tick, then engine instrument order', () => {
    expect(ex.notes.slice(0, 2).map((n) => n.instrument)).toEqual(['kick', 'hihatClosed']);
    const ticks = ex.notes.map((n) => n.tick);
    expect([...ticks].sort((a, b) => a - b)).toEqual(ticks);
  });

  it('is idempotent on the canonical form (state.exercise)', () => {
    expect(parseExercise(JSON.parse(JSON.stringify(ex)))).toEqual(ex);
  });
});

describe('parseExercise: format rules', () => {
  it('applies defaults', () => {
    const ex = parseExercise({ id: 'a', notes: [{ instrument: 'kick', tick: 0 }] });
    expect(ex.timeSignature).toEqual([4, 4]);
    expect(ex.beatTicks).toBe(480);
    expect(ex.subdivisionTicks).toBe(480);
    expect(ex.defaultBpm).toBe(80);
    expect(ex.title).toBe('a');
    expect(ex.notes[0].velocity).toBe(100);
  });

  it('defaults beatTicks to a dotted quarter in compound meters', () => {
    expect(defaultBeatTicks([6, 8])).toBe(720);
    expect(defaultBeatTicks([9, 8])).toBe(720);
    expect(defaultBeatTicks([12, 8])).toBe(720);
    expect(defaultBeatTicks([3, 8])).toBe(240);
    expect(defaultBeatTicks([3, 4])).toBe(480);
    expect(defaultBeatTicks([7, 8])).toBe(240);
  });

  it('reads accents, ghosts, separators and merges explicit notes', () => {
    const ex = parseExercise({
      id: 'b',
      timeSignature: [4, 4],
      gridTicks: 120,
      grid: { snare: 'X.g- | ....|x' },
      notes: [{ instrument: 'snare', tick: 0, velocity: 90 }, { instrument: 'crash', tick: 0 }],
    });
    expect(ex.notes.map((n) => [n.instrument, n.tick, n.velocity])).toEqual([
      ['snare', 0, 90],
      ['crash', 0, 100],
      ['snare', 240, 40],
      ['snare', 960, 100],
    ]);
  });

  it('accepts // comments like the docs example', () => {
    const ex = parseExercise('{ "id": "c", // comment\n "notes": [] // more "quoted" text\n}');
    expect(ex.id).toBe('c');
  });

  it('rejects bad input with readable errors', () => {
    expect(() => parseExercise('{')).toThrow(ExerciseError);
    expect(() => parseExercise({ grid: { cowbell: 'x' } })).toThrow(/unknown instrument "cowbell"/);
    expect(() => parseExercise({ gridTicks: 480, grid: { kick: 'x...x' } })).toThrow(/longer than 4 steps/);
    expect(() => parseExercise({ grid: { kick: 'xo' } })).toThrow(/bad character "o"/);
    expect(() => parseExercise({ notes: [{ instrument: 'kick', tick: 1920 }] })).toThrow(/outside the exercise/);
    expect(() => parseExercise({ ppq: 960 })).toThrow(/ppq/);
    expect(() => parseExercise({ notes: [{ instrument: 'kick', tick: 0, velocity: 0 }] })).toThrow(/velocity/);
  });

  it('computes ms per tick from the beat', () => {
    expect(msPerTick({ beatTicks: 720 }, 60)).toBeCloseTo(1000 / 720);
    expect(msPerTick({ beatTicks: 480 }, 120)).toBeCloseTo(500 / 480);
  });
});

describe('parseExercise: UI fixtures', () => {
  it('parses the 2-bar 4/4 16th fixture', () => {
    const ex = parseExercise(readFileSync(resolve(__dirname, 'fixtures/rock-4-4-16ths.json'), 'utf8'));
    expect(ex.bars).toBe(2);
    expect(ticksOf(ex, 'hihatClosed')).toHaveLength(31);
    expect(ticksOf(ex, 'hihatOpen')).toEqual([3720]);
    expect(ticksOf(ex, 'crash')).toEqual([0]);
  });
});
