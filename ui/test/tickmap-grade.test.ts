import { describe, expect, it } from 'vitest';
import { describeError, describeStats, errorColor, GRADE_COLORS, gradeColor, mixColor } from '../src/shared/grade';
import { hitDisplayTick, interpolateX, normalizeAnchors, TickMap, type Segment } from '../src/shared/tickmap';

describe('tick -> x mapping', () => {
  const anchors = [
    { tick: 0, x: 100 },
    { tick: 240, x: 150 },
    { tick: 720, x: 230 },
  ];

  it('interpolates between note positions', () => {
    expect(interpolateX(anchors, 0)).toBe(100);
    expect(interpolateX(anchors, 120)).toBe(125);
    expect(interpolateX(anchors, 480)).toBe(190);
    expect(interpolateX(anchors, 720)).toBe(230);
  });

  it('extrapolates outside the anchors with the edge slopes', () => {
    expect(interpolateX(anchors, -24)).toBe(95);
    expect(interpolateX(anchors, 960)).toBe(270);
    expect(interpolateX([{ tick: 5, x: 42 }], 100)).toBe(42);
    expect(interpolateX([], 1)).toBeNaN();
  });

  it('normalizes unsorted and duplicate anchors', () => {
    expect(normalizeAnchors([{ tick: 240, x: 150 }, { tick: 0, x: 100 }, { tick: 240, x: 152 }])).toEqual([
      { tick: 0, x: 100 },
      { tick: 240, x: 151 },
    ]);
  });

  const seg = (startTick: number, x0: number, row: number): Segment => ({
    startTick,
    endTick: startTick + 1920,
    anchors: [
      { tick: startTick, x: x0 },
      { tick: startTick + 1920, x: x0 + 400 },
    ],
    row,
    top: row * 100,
    bottom: row * 100 + 80,
    minX: x0 - 10,
    maxX: x0 + 405,
  });
  const map = new TickMap([seg(1920, 520, 0), seg(0, 120, 0), seg(3840, 120, 1)]);

  it('finds the bar segment and clamps to its note area', () => {
    expect(map.lengthTicks).toBe(5760);
    expect(map.locate(960)).toMatchObject({ x: 320, index: 0 });
    expect(map.locate(1920)).toMatchObject({ x: 520, index: 1 });
    expect(map.locate(3840 + 480)?.segment.row).toBe(1);
    expect(map.locate(-500)?.x).toBe(110);
    expect(map.locate(-500, false)?.x).toBeCloseTo(15.83, 1);
  });

  it('places a hit relative to the note it matched, across the loop seam', () => {
    // early hit for tick 0 that physically landed at the end of the previous loop
    expect(hitDisplayTick({ loop: 2, tickInLoop: 2860, expectedTick: 0 }, 2880)).toEqual({ tick: -20, loop: 3 });
    // late hit for the last note that landed in the next loop
    expect(hitDisplayTick({ loop: 3, tickInLoop: 10, expectedTick: 2860 }, 2880)).toEqual({ tick: 2890, loop: 2 });
    expect(hitDisplayTick({ loop: 1, tickInLoop: 1210, expectedTick: 1200 }, 2880)).toEqual({ tick: 1210, loop: 1 });
    expect(hitDisplayTick({ loop: 1, tickInLoop: 500, expectedTick: null }, 2880)).toEqual({ tick: 500, loop: 1 });
  });
});

describe('grade colours', () => {
  it('uses fixed colours for onTime, extra, free and missed', () => {
    expect(gradeColor('onTime', 3)).toBe(GRADE_COLORS.onTime);
    expect(gradeColor('extra', null)).toBe(GRADE_COLORS.extra);
    expect(gradeColor('free', null)).toBe(GRADE_COLORS.free);
    expect(gradeColor('missed')).toBe(GRADE_COLORS.missed);
  });

  it('goes amber -> red with |errorMs| for early and late', () => {
    expect(gradeColor('late', 15.5)).toBe(mixColor(GRADE_COLORS.amber, GRADE_COLORS.red, 0.5 / 45));
    expect(gradeColor('early', -16)).toBe(gradeColor('late', 16));
    expect(gradeColor('late', 60)).toBe(GRADE_COLORS.red);
    expect(gradeColor('early', -200)).toBe(GRADE_COLORS.red);
    const mid = gradeColor('late', 37.5);
    expect(mid).toBe(mixColor(GRADE_COLORS.amber, GRADE_COLORS.red, 0.5));
    expect(mid).not.toBe(GRADE_COLORS.amber);
    expect(mid).not.toBe(GRADE_COLORS.red);
    // grade wins over a small error value from the engine
    expect(gradeColor('late', 10)).not.toBe(GRADE_COLORS.onTime);
    expect(errorColor(10)).toBe(GRADE_COLORS.onTime);
  });

  it('mixes hex colours', () => {
    expect(mixColor('#000000', '#ffffff', 0.5)).toBe('#808080');
    expect(mixColor('#102030', '#102030', 0.3)).toBe('#102030');
  });
});

describe('plain-language stats', () => {
  it('describes errors and loop stats', () => {
    expect(describeError(-8.4)).toBe('8 ms early');
    expect(describeError(12)).toBe('12 ms late');
    expect(describeStats('kick', { hits: 3, missed: 0, extra: 0, meanErrorMs: 12.2, stdDevMs: 5 })).toBe(
      'kick 12 ms late on average, very steady',
    );
    expect(describeStats('snare', { hits: 2, missed: 0, extra: 0, meanErrorMs: -7, stdDevMs: 25 })).toBe(
      'snare 7 ms early on average, uneven (±25 ms)',
    );
    expect(describeStats('hi-hat', { hits: 12, missed: 0, extra: 0, meanErrorMs: 1, stdDevMs: 12 })).toBe(
      'hi-hat right on the beat on average, spread ±12 ms',
    );
    expect(describeStats('kick', { hits: 0, missed: 3, extra: 0, meanErrorMs: 0, stdDevMs: 0 })).toBe('kick: all 3 missed');
  });
});
