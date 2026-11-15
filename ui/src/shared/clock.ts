// Smooth transport position between ~60 Hz `position` messages: extrapolate with the tempo and
// gently pull towards each reported position (snapping on big jumps: start, seek, tempo change).

export interface ClockSample {
  playing: boolean;
  posTicks: number;
}

export class TransportClock {
  private basePos = 0;
  private baseTime = 0;
  private lastMsgAt = -Infinity;
  private msPerTick = 1;
  playing = false;

  /** Max time to keep extrapolating without news from the engine. */
  static readonly MAX_EXTRAPOLATE_MS = 250;
  /** Errors above this snap instead of being smoothed. */
  static readonly SNAP_MS = 40;
  static readonly PULL = 0.2;

  update(sample: ClockSample, msPerTick: number, now: number) {
    if (!sample.playing) {
      this.playing = false;
      this.basePos = sample.posTicks;
      this.baseTime = now;
      this.lastMsgAt = now;
      return;
    }
    const tempoChanged = Math.abs(msPerTick - this.msPerTick) > 1e-9;
    const predicted = this.raw(now);
    const errTicks = sample.posTicks - predicted;
    if (!this.playing || tempoChanged || Math.abs(errTicks) * msPerTick > TransportClock.SNAP_MS) {
      this.basePos = sample.posTicks;
    } else {
      this.basePos = predicted + errTicks * TransportClock.PULL;
    }
    this.baseTime = now;
    this.msPerTick = msPerTick;
    this.lastMsgAt = now;
    this.playing = true;
  }

  private raw(now: number): number {
    return this.basePos + (now - this.baseTime) / this.msPerTick;
  }

  /** Estimated posTicks at `now`. */
  position(now: number): number {
    if (!this.playing) return this.basePos;
    return this.raw(Math.min(now, this.lastMsgAt + TransportClock.MAX_EXTRAPOLATE_MS));
  }
}

/** Splits a continuous position into loop / tick in loop (protocol formula). */
export function splitPosition(posTicks: number, lengthTicks: number, looping: boolean): { loop: number; tickInLoop: number } {
  if (lengthTicks <= 0) return { loop: 0, tickInLoop: 0 };
  if (!looping && posTicks >= lengthTicks) return { loop: 0, tickInLoop: lengthTicks };
  const loop = Math.floor(posTicks / lengthTicks);
  return { loop, tickInLoop: posTicks - loop * lengthTicks };
}
