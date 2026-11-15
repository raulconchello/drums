import { describe, expect, it } from 'vitest';
import { splitPosition, TransportClock } from '../src/shared/clock';

describe('TransportClock', () => {
  const mpt = 1000 / 720; // 60 bpm, dotted-quarter beat

  it('extrapolates between messages using the tempo', () => {
    const c = new TransportClock();
    c.update({ playing: true, posTicks: 0 }, mpt, 1000);
    expect(c.position(1000)).toBe(0);
    expect(c.position(1500)).toBeCloseTo(0 + TransportClock.MAX_EXTRAPOLATE_MS / mpt); // capped
    c.update({ playing: true, posTicks: 12 }, mpt, 1016.6);
    expect(c.position(1016.6 + 8)).toBeGreaterThan(12);
  });

  it('smooths small jitter and snaps big jumps', () => {
    const c = new TransportClock();
    c.update({ playing: true, posTicks: 100 }, mpt, 0);
    // message arrives 3 ms late -> reported position is 3 ms behind the prediction
    const predicted = 100 + 16 / mpt;
    c.update({ playing: true, posTicks: predicted - 3 / mpt }, mpt, 16);
    expect(c.position(16)).toBeCloseTo(predicted - (3 / mpt) * TransportClock.PULL);
    // a jump (e.g. restart) snaps
    c.update({ playing: true, posTicks: -2880 }, mpt, 32);
    expect(c.position(32)).toBe(-2880);
  });

  it('holds the position when stopped', () => {
    const c = new TransportClock();
    c.update({ playing: false, posTicks: 500 }, mpt, 0);
    expect(c.position(10_000)).toBe(500);
  });
});

describe('splitPosition', () => {
  it('follows the protocol formula, including the count-in', () => {
    expect(splitPosition(3000, 2880, true)).toEqual({ loop: 1, tickInLoop: 120 });
    expect(splitPosition(-100, 2880, true)).toEqual({ loop: -1, tickInLoop: 2780 });
    expect(splitPosition(3000, 2880, false)).toEqual({ loop: 0, tickInLoop: 2880 });
  });
});
