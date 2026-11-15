// VexFlow drum notation + overlay layers (hit markers, missed rings, playhead).
import {
  Articulation,
  BarlineType,
  Beam,
  Dot,
  Formatter,
  GhostNote,
  Modifier,
  Parenthesis,
  Renderer,
  Stave,
  StaveNote,
  Tuplet,
  Voice,
  type SVGContext,
  type Note,
} from 'vexflow';
import { barTicks } from '../../shared/exercise';
import { gradeColor, describeError, formatErrorMs, GRADE_COLORS } from '../../shared/grade';
import { INSTRUMENT_INFO, isInstrument } from '../../shared/instruments';
import { planNotation, type PlannedBar, type PlannedVoice } from '../../shared/notation-plan';
import type { Exercise } from '../../shared/protocol';
import { TickMap, type Anchor, type Segment } from '../../shared/tickmap';
import { escapeHtml, hideTooltip, showTooltip, svg } from './dom';
import { loopAlpha, type HitRecord, type MissRecord } from './model';

export interface NotationTheme {
  ink: string;
  staff: string;
  accent: string;
  bg: string;
}

export interface NotationOptions {
  theme: NotationTheme;
  loop: boolean;
  hideFeetRests: boolean;
}

const LINE = 10; // VexFlow staff space
const SPACE_ABOVE = 4.6; // staff spaces above the top line (beams)
const EXTRA_ABOVE = 1.8; // more when accents / open hi-hat marks sit above the beams
const SPACE_BELOW = 4.4;
const MARGIN_X = 10;
const PAD_L = 8; // inside the note area, before tick 0
const PAD_R = 2;

const HEAD_CODE: Record<string, string> = { normal: '', x: '/x2', diamond: '/d2' };

function chunk<T>(arr: T[], n: number): T[][] {
  const out: T[][] = [];
  for (let i = 0; i < arr.length; i += n) out.push(arr.slice(i, i + n));
  return out;
}

const clamp = (v: number, lo: number, hi: number) => Math.min(hi, Math.max(lo, v));

interface BuiltVoice {
  notes: Note[];
  beams: Beam[];
  tuplets: Tuplet[];
}

function buildVoice(pv: PlannedVoice, stemDir: 1 | -1): BuiltVoice {
  const restKey = stemDir === 1 ? 'b/4' : 'e/4';
  const notes: Note[] = pv.events.map((e) => {
    if (e.kind === 'space') return new GhostNote({ duration: e.duration, dots: e.dots });
    if (e.kind === 'rest') {
      const r = new StaveNote({
        keys: [e.wholeBar ? 'd/5' : restKey],
        duration: e.duration,
        dots: e.dots,
        type: 'r',
        clef: 'percussion',
        align_center: !!e.wholeBar,
      });
      if (e.dots) Dot.buildAndAttach([r], { all: true });
      return r;
    }
    const n = new StaveNote({
      keys: e.keys.map((k) => k.key + HEAD_CODE[k.head]),
      duration: e.duration,
      dots: e.dots,
      stem_direction: stemDir,
      clef: 'percussion',
    });
    if (e.dots) Dot.buildAndAttach([n], { all: true });
    e.keys.forEach((k, i) => {
      if (k.open) n.addModifier(new Articulation('ah').setPosition(Modifier.Position.ABOVE), i);
      if (k.accent) n.addModifier(new Articulation('a>').setPosition(stemDir === 1 ? Modifier.Position.ABOVE : Modifier.Position.BELOW), i);
      if (k.ghost) {
        n.addModifier(new Parenthesis(Modifier.Position.LEFT), i);
        n.addModifier(new Parenthesis(Modifier.Position.RIGHT), i);
      }
    });
    return n;
  });
  const beams = pv.beams.map((idx) => new Beam(idx.map((i) => notes[i] as StaveNote)));
  const tuplets = pv.tuplets.map(
    (idx) =>
      new Tuplet(
        idx.map((i) => notes[i] as StaveNote),
        { num_notes: 3, notes_occupied: 2, location: stemDir === 1 ? Tuplet.LOCATION_TOP : Tuplet.LOCATION_BOTTOM, bracketed: true },
      ),
  );
  return { notes, beams, tuplets };
}

function makeStave(x: number, y: number, width: number, bar: PlannedBar, rowStart: boolean, ex: Exercise, opts: NotationOptions, above = SPACE_ABOVE): Stave {
  const st = new Stave(x, y, width, {
    space_above_staff_ln: above,
    space_below_staff_ln: SPACE_BELOW,
    fill_style: opts.theme.staff,
  });
  if (rowStart) st.addClef('percussion');
  if (bar.index === 0) st.addTimeSignature(`${ex.timeSignature[0]}/${ex.timeSignature[1]}`);
  if (opts.loop && bar.index === 0) st.setBegBarType(BarlineType.REPEAT_BEGIN);
  if (bar.index === ex.bars - 1) st.setEndBarType(opts.loop ? BarlineType.REPEAT_END : BarlineType.END);
  st.setDefaultLedgerLineStyle({ strokeStyle: opts.theme.staff, lineWidth: 1.4 });
  return st;
}

export class NotationView {
  readonly svg: SVGSVGElement;
  readonly map: TickMap;
  readonly scale: number;
  private readonly staves: Stave[] = [];
  private readonly back: SVGGElement;
  private readonly front: SVGGElement;
  private readonly playheadGroup: SVGGElement;
  private readonly playheadLine: SVGLineElement;
  private readonly playheadGlow: SVGLineElement;
  private readonly tips = new Map<string, string>();

  constructor(
    host: HTMLElement,
    readonly exercise: Exercise,
    opts: NotationOptions,
  ) {
    host.replaceChildren();
    const hostW = Math.max(480, host.clientWidth);
    const scale = clamp(hostW / 820, 1.0, 1.9);
    this.scale = scale;
    const logicalW = hostW / scale;
    const plan = planNotation(exercise, { hideFeetBeatRests: opts.hideFeetRests });
    const bt = barTicks(exercise);

    // Bars per row from note density, so 16ths stay readable.
    const cols = Math.max(1, ...plan.map((b) => new Set([...b.hands.events, ...b.feet.events].map((e) => e.tick)).size));
    const minBarW = 36 + cols * 21;
    const barsPerRow = clamp(Math.floor((logicalW - 2 * MARGIN_X - 70) / minBarW), 1, Math.min(4, plan.length));
    const rows = chunk(plan, barsPerRow);
    const marksAbove = plan.some((b) => b.hands.events.some((e) => e.keys.some((k) => k.accent || k.open)));
    const above = SPACE_ABOVE + (marksAbove ? EXTRA_ABOVE : 0);
    const ROW_H = (above + 4 + SPACE_BELOW) * LINE + 8;
    const height = rows.length * ROW_H + 4;

    const renderer = new Renderer(host as HTMLDivElement, Renderer.Backends.SVG);
    renderer.resize(hostW, Math.ceil(height * scale));
    const ctx = renderer.getContext() as SVGContext;
    ctx.scale(scale, scale);
    this.svg = ctx.svg;
    this.svg.setAttribute('preserveAspectRatio', 'xMinYMin meet');
    const { ink } = opts.theme;

    const segments: Segment[] = [];
    // Width taken by clef/time signature/barlines per bar, then one note-area width for every bar
    // of every row, so time is drawn to the same scale everywhere.
    const extrasOf = (bar: PlannedBar, rowStart: boolean) => {
      const probe = makeStave(0, 0, 600, bar, rowStart, exercise, opts);
      return probe.getNoteStartX() - probe.getX() + (probe.getX() + probe.getWidth() - probe.getNoteEndX());
    };
    const extras = rows.map((row) => row.map((bar, i) => extrasOf(bar, i === 0)));
    const plainExtra = Math.max(12, ...extras.flatMap((row) => row.slice(1)));
    // Full rows fill the width; a shorter last row keeps the smallest bar width (no stretching).
    const fullRowArea = (row: number[]) => (logicalW - 2 * MARGIN_X - row[0] - (barsPerRow - 1) * plainExtra) / barsPerRow;
    const minArea = Math.min(...extras.map(fullRowArea));

    rows.forEach((row, r) => {
      const y = 2 + r * ROW_H;
      const noteArea = row.length === barsPerRow ? fullRowArea(extras[r]) : minArea;
      let x = MARGIN_X;
      row.forEach((bar, i) => {
        const stave = makeStave(x, y, noteArea + extras[r][i], bar, i === 0, exercise, opts, above);
        x += stave.getWidth();
        this.staves[bar.index] = stave;

        const hands = buildVoice(bar.hands, 1);
        const feet = buildVoice(bar.feet, -1);
        const voices = [hands, feet].map((b) =>
          new Voice({ num_beats: exercise.timeSignature[0], beat_value: exercise.timeSignature[1] })
            .setMode(Voice.Mode.SOFT)
            .addTickables(b.notes),
        );
        const noteStart = stave.getNoteStartX();
        const noteEnd = stave.getNoteEndX();
        new Formatter().joinVoices(voices).format(voices, Math.max(50, noteEnd - noteStart - 12));

        // Time-proportional spacing: early/late offsets look the same everywhere in the bar.
        const inner = noteEnd - noteStart - PAD_L - PAD_R;
        const relX = (tick: number) => PAD_L + ((tick - bar.startTick) / bt) * inner;
        [bar.hands, bar.feet].forEach((pv, v) => {
          pv.events.forEach((e, k) => {
            const tc = (v === 0 ? hands : feet).notes[k].getTickContext();
            tc.setX(relX(e.tick));
          });
        });

        ctx.setFillStyle(ink);
        ctx.setStrokeStyle(ink);
        stave.setContext(ctx).draw();
        voices.forEach((voice) => voice.draw(ctx, stave));
        [...hands.beams, ...feet.beams].forEach((b) => b.setContext(ctx).draw());
        [...hands.tuplets, ...feet.tuplets].forEach((t) => t.setContext(ctx).draw());

        // Anchors: centres of the drawn note heads, plus the bar edges on the same linear scale.
        const anchors: Anchor[] = [];
        const offsets: number[] = [];
        [bar.hands, bar.feet].forEach((pv, v) => {
          pv.events.forEach((e, k) => {
            if (e.kind !== 'note') return;
            const n = (v === 0 ? hands : feet).notes[k] as StaveNote;
            const cx = (n.getNoteHeadBeginX() + n.getNoteHeadEndX()) / 2;
            anchors.push({ tick: e.tick, x: cx });
            offsets.push(cx - (noteStart + relX(e.tick)));
          });
        });
        const off = offsets.length ? offsets.reduce((s, o) => s + o, 0) / offsets.length : 18;
        const absX = (tick: number) => noteStart + relX(tick) + off;
        anchors.push({ tick: bar.startTick, x: absX(bar.startTick) }, { tick: bar.endTick, x: absX(bar.endTick) });
        segments.push({
          startTick: bar.startTick,
          endTick: bar.endTick,
          anchors,
          row: r,
          top: stave.getYForLine(0) - 2.6 * LINE,
          bottom: stave.getYForLine(4) + 2.6 * LINE,
          minX: noteStart - 2,
          maxX: stave.getX() + stave.getWidth() + 4,
        });
      });
    });
    this.map = new TickMap(segments);

    // Overlay layers: halos behind the notes, markers in front, playhead on top.
    this.back = svg('g', { class: 'hits-back' });
    this.svg.insertBefore(this.back, this.svg.firstChild);
    this.front = svg('g', { class: 'hits-front' });
    this.svg.append(this.front);
    this.playheadGroup = svg('g', { class: 'playhead', visibility: 'hidden' });
    this.playheadGlow = svg('line', { stroke: opts.theme.accent, 'stroke-width': 7, 'stroke-opacity': 0.18, 'stroke-linecap': 'round' });
    this.playheadLine = svg('line', { stroke: opts.theme.accent, 'stroke-width': 2, 'stroke-linecap': 'round' });
    this.playheadGroup.append(this.playheadGlow, this.playheadLine);
    this.svg.append(this.playheadGroup);

    this.front.addEventListener('mousemove', (e) => {
      const t = (e.target as Element).closest('[data-tip]');
      const html = t ? this.tips.get(t.getAttribute('data-tip')!) : undefined;
      if (html) showTooltip(html, e.clientX, e.clientY);
      else hideTooltip();
    });
    this.front.addEventListener('mouseleave', hideTooltip);
  }

  lineY(barIndex: number, instrument: string): number {
    const stave = this.staves[barIndex] ?? this.staves[0];
    const line = isInstrument(instrument) ? INSTRUMENT_INFO[instrument].line : 3;
    return stave.getYForNote(line);
  }

  setPlayhead(tick: number | null) {
    if (tick === null) {
      this.playheadGroup.setAttribute('visibility', 'hidden');
      return;
    }
    const loc = this.map.locate(tick, false);
    if (!loc) return;
    const x = clamp(loc.x, loc.segment.minX, loc.segment.maxX).toFixed(2);
    for (const l of [this.playheadLine, this.playheadGlow]) {
      l.setAttribute('x1', x);
      l.setAttribute('x2', x);
      l.setAttribute('y1', String(loc.segment.top));
      l.setAttribute('y2', String(loc.segment.bottom));
    }
    this.playheadGroup.setAttribute('visibility', 'visible');
  }

  /** Redraws hit markers and missed rings for the current and previous loop. */
  renderMarks(hits: HitRecord[], misses: MissRecord[], currentLoop: number, showLabels: boolean) {
    this.back.replaceChildren();
    this.front.replaceChildren();
    this.tips.clear();
    const L = this.map.lengthTicks;

    for (const m of misses) {
      const alpha = loopAlpha(m.loop, currentLoop);
      if (alpha === 0) continue;
      const loc = this.map.locate(m.expectedTick);
      if (!loc) continue;
      const y = this.lineY(loc.index, m.instrument);
      const id = `m${m.id}`;
      this.tips.set(id, `<b>${escapeHtml(label(m.instrument))}</b> missed<div class="tt-sub">loop ${m.loop + 1} · expected note not played</div>`);
      this.front.append(
        svg('circle', { cx: loc.x, cy: y, r: 8.5, fill: 'none', stroke: GRADE_COLORS.missed, 'stroke-width': 2.4, opacity: alpha }),
        svg('circle', { cx: loc.x, cy: y, r: 10, fill: 'transparent', 'data-tip': id, class: 'hit-target' }),
      );
    }

    for (const h of hits) {
      const alpha = loopAlpha(h.loop, currentLoop);
      if (alpha === 0) continue;
      const near = h.expectedTick ?? clamp(h.tick, 0, L - 1);
      const loc = this.map.locateNear(h.tick, near);
      if (!loc) continue;
      const y = this.lineY(loc.index, h.instrument);
      const color = gradeColor(h.grade, h.errorMs);
      if (alpha < 1) {
        // previous loop: a small faded dot only
        this.back.append(svg('circle', { cx: loc.x, cy: y, r: 3.2, fill: color, opacity: 0.55 }));
        continue;
      }
      if (h.expectedTick != null) {
        const exp = this.map.locate(h.expectedTick);
        if (exp && Math.abs(exp.x - loc.x) > 1.5) {
          this.back.append(
            svg('line', { x1: exp.x, y1: y, x2: loc.x, y2: y, stroke: color, 'stroke-width': 2.6, 'stroke-linecap': 'round', opacity: 0.85 }),
          );
        }
      }
      this.back.append(svg('circle', { cx: loc.x, cy: y, r: 6.8, fill: color, opacity: 0.42 }));
      this.front.append(
        svg('line', { x1: loc.x, y1: y - 7, x2: loc.x, y2: y + 7, stroke: color, 'stroke-width': 2, 'stroke-linecap': 'round' }),
      );
      if (showLabels && h.errorMs != null && alpha === 1) {
        const above = isInstrument(h.instrument) ? INSTRUMENT_INFO[h.instrument].voice === 'hands' : true;
        const t = svg('text', {
          x: loc.x,
          y: above ? y - 12 : y + 20,
          'text-anchor': 'middle',
          'font-size': 8.5,
          'font-family': 'system-ui, sans-serif',
          'font-weight': 600,
          fill: color,
        });
        t.textContent = formatErrorMs(h.errorMs);
        this.front.append(t);
      }
      const id = `h${h.id}`;
      this.tips.set(id, hitTooltip(h));
      this.front.append(svg('circle', { cx: loc.x, cy: y, r: 9, fill: 'transparent', 'data-tip': id, class: 'hit-target' }));
    }
  }
}

function label(instrument: string): string {
  return isInstrument(instrument) ? INSTRUMENT_INFO[instrument].label : instrument;
}

export function hitTooltip(h: HitRecord): string {
  const what =
    h.grade === 'extra'
      ? 'extra hit (no note expected)'
      : h.errorMs == null
        ? h.grade
        : h.grade === 'onTime'
          ? `on time (${describeError(h.errorMs)})`
          : describeError(h.errorMs);
  return `<b>${escapeHtml(label(h.instrument))}</b> · ${escapeHtml(what)}<div class="tt-sub">loop ${h.loop + 1} · velocity ${h.velocity} · MIDI ${h.note}</div>`;
}
