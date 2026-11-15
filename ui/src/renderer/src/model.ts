// Practice-session data received from the engine: hits, misses and loop summaries.
import { hitDisplayTick } from '../../shared/tickmap';
import type { Grade, HitMsg, LoopSummaryMsg, MissedMsg } from '../../shared/protocol';

export interface HitRecord {
  id: number;
  instrument: string;
  grade: Grade;
  errorMs: number | null;
  expectedTick: number | null;
  /** Display tick in the loop (may be slightly < 0 or > length for hits across the loop seam). */
  tick: number;
  /** Loop the hit belongs to (the loop of its expected note when matched). */
  loop: number;
  velocity: number;
  note: number;
}

export interface MissRecord {
  id: number;
  instrument: string;
  loop: number;
  expectedTick: number;
}

let nextId = 1;

export class Session {
  hits: HitRecord[] = [];
  misses: MissRecord[] = [];
  summaries: LoopSummaryMsg[] = [];
  /** Per-loop count of on-time hits (the summary has no onTime count). */
  onTimeByLoop = new Map<number, number>();

  clear() {
    this.hits = [];
    this.misses = [];
    this.onTimeByLoop.clear();
  }

  clearAll() {
    this.clear();
    this.summaries = [];
  }

  addHit(m: HitMsg, lengthTicks: number): HitRecord | null {
    if (m.grade === 'free' || m.loop == null || m.tickInLoop == null) return null;
    const { tick, loop } = hitDisplayTick({ loop: m.loop, tickInLoop: m.tickInLoop, expectedTick: m.expectedTick }, lengthTicks);
    const rec: HitRecord = {
      id: nextId++,
      instrument: m.instrument,
      grade: m.grade,
      errorMs: m.errorMs,
      expectedTick: m.expectedTick,
      tick,
      loop,
      velocity: m.velocity,
      note: m.note,
    };
    this.hits.push(rec);
    if (m.grade === 'onTime') this.onTimeByLoop.set(loop, (this.onTimeByLoop.get(loop) ?? 0) + 1);
    return rec;
  }

  addMiss(m: MissedMsg): MissRecord {
    const rec = { id: nextId++, instrument: m.instrument, loop: m.loop, expectedTick: m.expectedTick };
    this.misses.push(rec);
    return rec;
  }

  addSummary(m: LoopSummaryMsg) {
    this.summaries = [...this.summaries.filter((s) => s.loop !== m.loop), m].sort((a, b) => a.loop - b.loop).slice(-8);
  }

  /** Forget everything older than the previous loop. */
  prune(currentLoop: number) {
    const keep = currentLoop - 1;
    this.hits = this.hits.filter((h) => h.loop >= keep);
    this.misses = this.misses.filter((m) => m.loop >= keep);
    for (const l of this.onTimeByLoop.keys()) if (l < currentLoop - 10) this.onTimeByLoop.delete(l);
  }

  visibleHits(currentLoop: number): HitRecord[] {
    return this.hits.filter((h) => h.loop >= currentLoop - 1);
  }

  visibleMisses(currentLoop: number): MissRecord[] {
    return this.misses.filter((m) => m.loop >= currentLoop - 1);
  }
}

/** 1 for the current loop (and early hits for the next one), faded for the previous loop. */
export function loopAlpha(loop: number, currentLoop: number): number {
  return loop >= currentLoop ? 1 : loop === currentLoop - 1 ? 0.38 : 0;
}
