// Timeline lanes: one lane per instrument, time to scale across the whole loop. Easier to read
// timing than notation: grid at subdivisions, expected notes as outlined ticks, hits as dots.
import { barTicks, lengthTicks } from '../../shared/exercise';
import { gradeColor, GRADE_COLORS } from '../../shared/grade';
import { INSTRUMENT_INFO, byStaffOrder, isInstrument, type Instrument } from '../../shared/instruments';
import type { Exercise } from '../../shared/protocol';
import { hideTooltip, showTooltip } from './dom';
import { loopAlpha, type HitRecord, type MissRecord } from './model';
import { hitTooltip } from './notation';

export interface LaneTheme {
  bg: string;
  ink: string;
  ink2: string;
  muted: string;
  line: string;
  accent: string;
  panel2: string;
}

const LABEL_W = 92;
const HEADER_H = 20;
const LANE_H = 30;
const RIGHT_PAD = 14;

export interface LaneFrame {
  tick: number | null;
  hits: HitRecord[];
  misses: MissRecord[];
  currentLoop: number;
  zoom: number;
}

export class Lanes {
  private ctx: CanvasRenderingContext2D;
  private ex: Exercise | null = null;
  private instruments: Instrument[] = [];
  private width = 0;
  private height = 0;
  private dpr = 1;
  private lastFrame: LaneFrame | null = null;
  private hoverTargets: { x: number; y: number; html: string }[] = [];

  constructor(
    private readonly canvas: HTMLCanvasElement,
    private theme: LaneTheme,
  ) {
    this.ctx = canvas.getContext('2d')!;
    canvas.addEventListener('mousemove', (e) => this.onHover(e));
    canvas.addEventListener('mouseleave', hideTooltip);
  }

  setTheme(theme: LaneTheme) {
    this.theme = theme;
  }

  setExercise(ex: Exercise | null) {
    this.ex = ex;
    this.instruments = [];
    if (ex) this.ensureInstruments(ex.notes.map((n) => n.instrument));
  }

  /** Adds lanes for instruments that were hit but are not in the exercise. Returns true if changed. */
  ensureInstruments(names: string[]): boolean {
    const add = names.filter((n): n is Instrument => isInstrument(n) && !this.instruments.includes(n));
    if (add.length === 0) return false;
    this.instruments = [...new Set([...this.instruments, ...add])].sort(byStaffOrder);
    this.resize();
    return true;
  }

  resize() {
    const cssW = this.canvas.parentElement!.clientWidth;
    const cssH = HEADER_H + Math.max(1, this.instruments.length) * LANE_H + 6;
    this.dpr = window.devicePixelRatio || 1;
    if (cssW !== this.width || cssH !== this.height || this.canvas.width !== Math.round(cssW * this.dpr)) {
      this.width = cssW;
      this.height = cssH;
      this.canvas.style.height = `${cssH}px`;
      this.canvas.width = Math.round(cssW * this.dpr);
      this.canvas.height = Math.round(cssH * this.dpr);
    }
    if (this.lastFrame) this.draw(this.lastFrame);
  }

  private xOf(tick: number): number {
    const L = this.ex ? lengthTicks(this.ex) : 1;
    return LABEL_W + (tick / L) * (this.width - LABEL_W - RIGHT_PAD);
  }

  draw(frame: LaneFrame) {
    this.lastFrame = frame;
    const { ctx, theme: t, ex } = this;
    const W = this.width;
    const H = this.height;
    ctx.setTransform(this.dpr, 0, 0, this.dpr, 0, 0);
    ctx.clearRect(0, 0, W, H);
    this.hoverTargets = [];
    if (!ex) return;

    const L = lengthTicks(ex);
    const bt = barTicks(ex);
    const laneY = (i: number) => HEADER_H + i * LANE_H;

    // lane backgrounds + labels
    ctx.font = '600 12px system-ui, sans-serif';
    ctx.textBaseline = 'middle';
    this.instruments.forEach((inst, i) => {
      if (i % 2 === 1) {
        ctx.fillStyle = t.panel2;
        ctx.globalAlpha = 0.45;
        ctx.fillRect(0, laneY(i), W, LANE_H);
        ctx.globalAlpha = 1;
      }
      ctx.fillStyle = ex.notes.some((n) => n.instrument === inst) ? t.ink : t.muted;
      ctx.fillText(INSTRUMENT_INFO[inst].short, 12, laneY(i) + LANE_H / 2);
    });

    // grid
    const top = HEADER_H - 4;
    const bottom = HEADER_H + this.instruments.length * LANE_H;
    ctx.font = '600 10.5px system-ui, sans-serif';
    ctx.textAlign = 'center';
    for (let tick = 0; tick <= L + 0.5; tick += ex.subdivisionTicks) {
      const x = Math.round(this.xOf(tick)) + 0.5;
      const isBar = tick % bt === 0;
      const isBeat = tick % ex.beatTicks === 0 || isBar;
      ctx.strokeStyle = isBar ? t.ink2 : isBeat ? t.muted : t.line;
      ctx.globalAlpha = isBar ? 0.75 : isBeat ? 0.6 : 0.8;
      ctx.lineWidth = isBar ? 1.5 : 1;
      ctx.beginPath();
      ctx.moveTo(x, isBeat ? top : HEADER_H);
      ctx.lineTo(x, bottom);
      ctx.stroke();
      ctx.globalAlpha = 1;
      if (isBeat && tick < L) {
        const beatInBar = Math.round((tick % bt) / ex.beatTicks) + 1;
        ctx.fillStyle = isBar ? t.ink : t.muted;
        ctx.fillText(isBar && ex.bars > 1 ? `${Math.floor(tick / bt) + 1}.${beatInBar}` : String(beatInBar), x, 9);
      }
    }
    ctx.textAlign = 'left';

    const laneOf = new Map(this.instruments.map((inst, i) => [inst as string, i]));
    const cy = (inst: string) => laneY(laneOf.get(inst) ?? 0) + LANE_H / 2;

    // expected notes
    ctx.lineWidth = 1.5;
    for (const n of ex.notes) {
      const x = this.xOf(n.tick);
      const y = cy(n.instrument);
      ctx.strokeStyle = t.ink2;
      ctx.globalAlpha = 0.85;
      roundRect(ctx, x - 3, y - 9, 6, 18, 3);
      ctx.stroke();
    }
    ctx.globalAlpha = 1;

    // misses
    for (const m of frame.misses) {
      const a = loopAlpha(m.loop, frame.currentLoop);
      if (!a || !laneOf.has(m.instrument)) continue;
      const x = this.xOf(m.expectedTick);
      const y = cy(m.instrument);
      ctx.globalAlpha = a;
      ctx.strokeStyle = GRADE_COLORS.missed;
      ctx.lineWidth = 2;
      ctx.beginPath();
      ctx.arc(x, y, 9, 0, Math.PI * 2);
      ctx.stroke();
      this.hoverTargets.push({ x, y, html: `<b>${INSTRUMENT_INFO[m.instrument as Instrument]?.label ?? m.instrument}</b> missed<div class="tt-sub">loop ${m.loop + 1}</div>` });
    }

    // hits: previous loop first so the current loop is on top
    const sorted = [...frame.hits].sort((a, b) => a.loop - b.loop);
    for (const h of sorted) {
      const a = loopAlpha(h.loop, frame.currentLoop);
      if (!a || !laneOf.has(h.instrument)) continue;
      const shown = h.expectedTick != null ? h.expectedTick + (h.tick - h.expectedTick) * frame.zoom : h.tick;
      const x = this.xOf(shown);
      const y = cy(h.instrument);
      const color = gradeColor(h.grade, h.errorMs);
      ctx.globalAlpha = a;
      if (h.expectedTick != null) {
        const ex0 = this.xOf(h.expectedTick);
        ctx.strokeStyle = color;
        ctx.lineWidth = 2.5;
        ctx.beginPath();
        ctx.moveTo(ex0, y);
        ctx.lineTo(x, y);
        ctx.stroke();
      }
      ctx.fillStyle = color;
      ctx.beginPath();
      ctx.arc(x, y, a < 1 ? 4.5 : 6, 0, Math.PI * 2);
      ctx.fill();
      ctx.strokeStyle = t.bg;
      ctx.lineWidth = 1.5;
      ctx.stroke();
      if (a === 1) this.hoverTargets.push({ x, y, html: hitTooltip(h) });
    }
    ctx.globalAlpha = 1;

    // playhead
    if (frame.tick !== null) {
      const x = this.xOf(Math.min(Math.max(frame.tick, 0), L));
      ctx.strokeStyle = t.accent;
      ctx.globalAlpha = 0.18;
      ctx.lineWidth = 7;
      ctx.beginPath();
      ctx.moveTo(x, top);
      ctx.lineTo(x, bottom);
      ctx.stroke();
      ctx.globalAlpha = 1;
      ctx.lineWidth = 2;
      ctx.beginPath();
      ctx.moveTo(x, top);
      ctx.lineTo(x, bottom);
      ctx.stroke();
    }
  }

  private onHover(e: MouseEvent) {
    const r = this.canvas.getBoundingClientRect();
    const x = e.clientX - r.left;
    const y = e.clientY - r.top;
    let best: { d: number; html: string } | null = null;
    for (const t of this.hoverTargets) {
      const d = Math.hypot(t.x - x, t.y - y);
      if (d < 12 && (!best || d < best.d)) best = { d, html: t.html };
    }
    if (best) showTooltip(best.html, e.clientX, e.clientY);
    else hideTooltip();
  }
}

function roundRect(ctx: CanvasRenderingContext2D, x: number, y: number, w: number, h: number, r: number) {
  ctx.beginPath();
  ctx.moveTo(x + r, y);
  ctx.arcTo(x + w, y, x + w, y + h, r);
  ctx.arcTo(x + w, y + h, x, y + h, r);
  ctx.arcTo(x, y + h, x, y, r);
  ctx.arcTo(x, y, x + w, y, r);
  ctx.closePath();
}
