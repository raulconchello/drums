// tick -> x mapping by interpolating between rendered note positions.

export interface Anchor {
  tick: number;
  x: number;
}

/** One rendered bar: a tick range, its anchors and the vertical extent of its staff. */
export interface Segment {
  startTick: number;
  endTick: number;
  anchors: Anchor[];
  row: number;
  top: number;
  bottom: number;
  /** Leftmost / rightmost x a marker may use (stave note area). */
  minX: number;
  maxX: number;
}

/** Normalizes anchors: sorted by tick, duplicates averaged. */
export function normalizeAnchors(anchors: Anchor[]): Anchor[] {
  const byTick = new Map<number, number[]>();
  for (const a of anchors) {
    if (!Number.isFinite(a.tick) || !Number.isFinite(a.x)) continue;
    const xs = byTick.get(a.tick) ?? [];
    xs.push(a.x);
    byTick.set(a.tick, xs);
  }
  return [...byTick.entries()]
    .map(([tick, xs]) => ({ tick, x: xs.reduce((s, v) => s + v, 0) / xs.length }))
    .sort((a, b) => a.tick - b.tick);
}

/**
 * Linear interpolation between anchors (sorted by tick). Outside the anchors it extrapolates with the
 * slope of the nearest pair, so slightly early/late hits around the first/last note still get an x.
 */
export function interpolateX(anchors: Anchor[], tick: number): number {
  const n = anchors.length;
  if (n === 0) return NaN;
  if (n === 1) return anchors[0].x;
  let i = 0;
  if (tick <= anchors[0].tick) i = 0;
  else if (tick >= anchors[n - 1].tick) i = n - 2;
  else {
    // binary search for the last anchor with tick <= target
    let lo = 0;
    let hi = n - 1;
    while (hi - lo > 1) {
      const mid = (lo + hi) >> 1;
      if (anchors[mid].tick <= tick) lo = mid;
      else hi = mid;
    }
    i = lo;
  }
  const a = anchors[i];
  const b = anchors[i + 1];
  if (b.tick === a.tick) return a.x;
  return a.x + ((tick - a.tick) / (b.tick - a.tick)) * (b.x - a.x);
}

export interface Located {
  x: number;
  segment: Segment;
  index: number;
}

export class TickMap {
  readonly segments: Segment[];
  readonly lengthTicks: number;

  constructor(segments: Segment[]) {
    this.segments = segments
      .map((s) => ({ ...s, anchors: normalizeAnchors(s.anchors) }))
      .sort((a, b) => a.startTick - b.startTick);
    this.lengthTicks = this.segments.length ? this.segments[this.segments.length - 1].endTick : 0;
  }

  segmentIndexFor(tick: number): number {
    const segs = this.segments;
    if (segs.length === 0) return -1;
    for (let i = 0; i < segs.length; i++) {
      if (tick < segs[i].endTick) return i;
    }
    return segs.length - 1;
  }

  /** x for a tick inside the loop (ticks before 0 / after the end use the first / last bar). */
  locate(tick: number, clamp = true): Located | null {
    const index = this.segmentIndexFor(tick);
    if (index < 0) return null;
    const segment = this.segments[index];
    let x = interpolateX(segment.anchors, tick);
    if (clamp) x = Math.min(segment.maxX, Math.max(segment.minX, x));
    return { x, segment, index };
  }

  /** Like locate() but keeps a hit in the bar of the note it was matched to (for hits across a barline). */
  locateNear(tick: number, nearTick: number, clamp = true): Located | null {
    const index = this.segmentIndexFor(nearTick);
    if (index < 0) return null;
    const segment = this.segments[index];
    let x = interpolateX(segment.anchors, tick);
    if (clamp) x = Math.min(segment.maxX, Math.max(segment.minX, x));
    return { x, segment, index };
  }
}

/**
 * Where to draw a hit: matched hits are placed relative to their expected note, so an early hit on
 * tick 0 that physically happened at the end of the previous loop (or in the count-in) shows just
 * before the first note instead of at the far end of the bar. Returns the display tick (may be
 * < 0 or >= length) and which loop the hit belongs to.
 */
export function hitDisplayTick(
  hit: { loop: number; tickInLoop: number; expectedTick: number | null },
  lengthTicks: number,
): { tick: number; loop: number } {
  if (hit.expectedTick == null || lengthTicks <= 0) return { tick: hit.tickInLoop, loop: hit.loop };
  let diff = hit.tickInLoop - hit.expectedTick;
  let loop = hit.loop;
  if (diff > lengthTicks / 2) {
    // played at the end of a loop for a note at the start of the next one
    diff -= lengthTicks;
    loop += 1;
  } else if (diff < -lengthTicks / 2) {
    // played at the start of a loop for a note at the end of the previous one
    diff += lengthTicks;
    loop -= 1;
  }
  return { tick: hit.expectedTick + diff, loop };
}
