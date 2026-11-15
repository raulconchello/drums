import { readFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { describe, expect, it } from 'vitest';
import { parseExercise } from '../src/shared/exercise';
import { describeVoice, durationFor, planNotation, splitDuration } from '../src/shared/notation-plan';

const load = (p: string) => parseExercise(readFileSync(resolve(__dirname, p), 'utf8'));

describe('durations', () => {
  it('maps ticks to VexFlow durations', () => {
    expect(durationFor(720)).toEqual({ duration: 'q', dots: 1 });
    expect(durationFor(240)).toEqual({ duration: '8', dots: 0 });
    expect(durationFor(300)).toBeNull();
    expect(splitDuration(600).map((d) => d.ticks)).toEqual([480, 120]);
  });
});

describe('planNotation: 12/8 groove (reference image)', () => {
  const [bar] = planNotation(load('../../exercises/groove-12-8.json'));

  it('hands: twelve hi-hat eighths, snare on 5 and 7, beamed in threes', () => {
    expect(describeVoice(bar.hands)).toEqual([
      '8:hihatClosed', '8:hihatClosed', '8:hihatClosed',
      '8:hihatClosed', '8:snare+hihatClosed', '8:hihatClosed',
      '8:snare+hihatClosed', '8:hihatClosed', '8:hihatClosed',
      '8:hihatClosed', '8:hihatClosed', '8:hihatClosed',
    ]);
    expect(bar.hands.beams).toEqual([[0, 1, 2], [3, 4, 5], [6, 7, 8], [9, 10, 11]]);
    expect(bar.hands.events[0].keys[0]).toMatchObject({ key: 'g/5', head: 'x' });
    expect(bar.hands.events[4].keys.map((k) => k.key)).toEqual(['c/5', 'g/5']);
  });

  it('feet: dotted quarter, quarter rest + eighth, (hidden beat), quarter rest + eighth', () => {
    expect(describeVoice(bar.feet)).toEqual(['qd:kick', 'q:rest', '8:kick', 'qd:space', 'q:rest', '8:kick']);
    expect(bar.feet.events.map((e) => e.tick)).toEqual([0, 720, 1200, 1440, 2160, 2640]);
    expect(bar.feet.beams).toEqual([]);
  });

  it('can show every rest instead', () => {
    const [b] = planNotation(load('../../exercises/groove-12-8.json'), { hideFeetBeatRests: false });
    expect(describeVoice(b.feet)[3]).toBe('qd:rest');
  });
});

describe('planNotation: 4/4 grooves', () => {
  it('2 bars of 16ths: beamed per quarter, 8th+16th+16th figures, open hat, accents and ghosts', () => {
    const bars = planNotation(load('fixtures/rock-4-4-16ths.json'));
    expect(bars).toHaveLength(2);
    expect(bars[1].startTick).toBe(1920);
    const hands0 = describeVoice(bars[0].hands);
    expect(hands0).toHaveLength(16);
    expect(hands0[0]).toBe('16:hihatClosed+crash');
    expect(bars[0].hands.beams).toEqual([[0, 1, 2, 3], [4, 5, 6, 7], [8, 9, 10, 11], [12, 13, 14, 15]]);
    expect(bars[0].hands.events[0].keys.find((k) => k.instrument === 'crash')?.accent).toBe(true);
    expect(bars[0].hands.events[15].keys.find((k) => k.instrument === 'snare')?.ghost).toBe(true);
    // kick bar 1: "x...|....|x.x.|...." -> q, (hidden q), 8 8, (hidden q)
    expect(describeVoice(bars[0].feet)).toEqual(['q:kick', 'q:space', '8:kick', '8:kick', 'q:space']);
    // kick bar 2: "x..x|..x.|x...|...." -> 8d 16 | 8r 8 | q | (hidden)
    expect(describeVoice(bars[1].feet)).toEqual(['8d:kick', '16:kick', '8:rest', '8:kick', 'q:kick', 'q:space']);
    expect(bars[1].feet.beams).toEqual([[0, 1]]);
    // last 16th of bar 2 is the open hi-hat
    const last = bars[1].hands.events[15];
    expect(last.keys[0]).toMatchObject({ instrument: 'hihatOpen', open: true });
  });

  it('triplets become 3:2 tuplets with nominal durations', () => {
    const [bar] = planNotation(load('fixtures/shuffle-4-4-triplets.json'));
    // ride on triplet 1 and 3 of beats 1-3: quarter + eighth inside a tuplet
    expect(describeVoice(bar.hands).slice(0, 2)).toEqual(['q:ride', '8:ride']);
    expect(bar.hands.tuplets.length).toBe(4);
    expect(bar.hands.events[1].tick).toBe(320);
    expect(bar.hands.events[1].ticks).toBeCloseTo(160);
  });

  it('empty bars: whole rest for hands, nothing visible for feet', () => {
    const ex = parseExercise({ id: 'e', bars: 2, notes: [{ instrument: 'snare', tick: 0 }, { instrument: 'kick', tick: 0 }] });
    const bars = planNotation(ex);
    expect(describeVoice(bars[1].hands)).toEqual(['w:rest']);
    expect(bars[1].hands.events[0].wholeBar).toBe(true);
    expect(bars[1].feet.events.every((e) => e.kind === 'space')).toBe(true);
    // bar 1 hands: snare quarter on 1 then half rest on 3-4 merged... beat 2 alone stays a quarter rest
    expect(describeVoice(bars[0].hands)).toEqual(['q:snare', 'q:rest', 'h:rest']);
  });
});
