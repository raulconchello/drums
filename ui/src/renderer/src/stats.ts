// Per-loop stats from `loopSummary`: per-instrument table in plain words + trend of the last 8 loops.
import { describeStats, errorColor } from '../../shared/grade';
import { INSTRUMENT_INFO, byStaffOrder, isInstrument } from '../../shared/instruments';
import type { LoopSummaryMsg, Stats } from '../../shared/protocol';
import { escapeHtml, h, hideTooltip, showTooltip, svg } from './dom';

const label = (name: string) => (isInstrument(name) ? INSTRUMENT_INFO[name].label : name);

function meanWords(s: Stats): string {
  if (s.hits === 0) return '—';
  const m = Math.round(s.meanErrorMs);
  const dir = Math.abs(m) <= 2 ? 'on the beat' : `${Math.abs(m)} ms ${m < 0 ? 'early' : 'late'}`;
  return s.hits > 1 ? `${dir} <span class="sd">±${Math.round(s.stdDevMs)}</span>` : dir;
}

function sparkline(values: (number | null)[]): SVGSVGElement {
  const W = 72;
  const H = 22;
  const lim = Math.max(20, ...values.map((v) => Math.abs(v ?? 0)));
  const el = svg('svg', { viewBox: `0 0 ${W} ${H}`, width: W, height: H });
  const y = (v: number) => H / 2 - (v / lim) * (H / 2 - 3);
  const x = (i: number) => 3 + (i / Math.max(1, values.length - 1)) * (W - 6);
  el.append(svg('line', { x1: 0, x2: W, y1: H / 2, y2: H / 2, stroke: 'var(--line)', 'stroke-width': 1 }));
  const pts = values.map((v, i) => (v == null ? null : `${x(i).toFixed(1)},${y(v).toFixed(1)}`)).filter(Boolean);
  if (pts.length > 1) el.append(svg('polyline', { points: pts.join(' '), fill: 'none', stroke: 'var(--ink-2)', 'stroke-width': 1.5, 'stroke-linejoin': 'round' }));
  const lastI = values.length - 1;
  const last = values[lastI];
  if (last != null) el.append(svg('circle', { cx: x(lastI), cy: y(last), r: 3, fill: errorColor(last) }));
  return el;
}

function trendChart(summaries: LoopSummaryMsg[], onTime: Map<number, number>): SVGSVGElement {
  const W = 380;
  const H = 96;
  const left = 50;
  const right = 6;
  const top = 6;
  const plotH = 56;
  const lim = Math.max(30, ...summaries.map((s) => Math.abs(s.total.meanErrorMs) + s.total.stdDevMs));
  const el = svg('svg', { viewBox: `0 0 ${W} ${H}`, class: 'trend', role: 'img', 'aria-label': 'Average timing error over the last loops' });
  const y = (v: number) => top + plotH / 2 - (v / lim) * (plotH / 2);
  const n = 8;
  const x = (i: number) => left + ((i + 0.5) / n) * (W - left - right);

  // recessive grid + axis labels
  for (const v of [lim, 0, -lim]) {
    el.append(svg('line', { x1: left, x2: W - right, y1: y(v), y2: y(v), stroke: 'var(--line)', 'stroke-width': v === 0 ? 1.4 : 1, 'stroke-dasharray': v === 0 ? '' : '3 4' }));
    const t = svg('text', { x: left - 6, y: y(v) + 3.5, 'text-anchor': 'end', 'font-size': 10, fill: 'var(--muted)' });
    t.textContent = v === 0 ? '0 ms' : v > 0 ? `+${Math.round(v)} late` : `${Math.round(v)} early`;
    el.append(t);
  }

  const shown = summaries.slice(-n);
  shown.forEach((s, i) => {
    const cx = x(i + (n - shown.length));
    const m = s.total.meanErrorMs;
    const sd = s.total.stdDevMs;
    const color = errorColor(m);
    const g = svg('g', {});
    if (s.total.hits > 1) {
      g.append(svg('line', { x1: cx, x2: cx, y1: y(m + sd), y2: y(m - sd), stroke: 'var(--ink-2)', 'stroke-width': 2, 'stroke-linecap': 'round', opacity: 0.55 }));
    }
    g.append(svg('circle', { cx, cy: y(m), r: 5, fill: color, stroke: 'var(--panel)', 'stroke-width': 2 }));
    const loopT = svg('text', { x: cx, y: top + plotH + 13, 'text-anchor': 'middle', 'font-size': 10, fill: 'var(--muted)' });
    loopT.textContent = `#${s.loop + 1}`;
    const expected = s.total.hits + s.total.missed;
    const onTimeN = onTime.get(s.loop);
    const accT = svg('text', { x: cx, y: top + plotH + 27, 'text-anchor': 'middle', 'font-size': 10.5, 'font-weight': 600, fill: 'var(--ink-2)' });
    accT.textContent = expected && onTimeN != null ? `${Math.round((onTimeN / expected) * 100)}%` : expected ? `${Math.round((s.total.hits / expected) * 100)}%` : '';
    // generous hit target for the tooltip
    const hit = svg('rect', { x: cx - 16, y: top, width: 32, height: plotH + 30, fill: 'transparent' });
    const tip =
      `<b>Loop ${s.loop + 1}</b> · ${escapeHtml(describeStats('average', s.total))}` +
      `<div class="tt-sub">${s.total.hits} hits · ${s.total.missed} missed · ${s.total.extra} extra` +
      (expected && onTimeN != null ? ` · ${onTimeN} of ${expected} on time` : '') +
      `</div>`;
    hit.addEventListener('mousemove', (e) => showTooltip(tip, e.clientX, e.clientY));
    hit.addEventListener('mouseleave', hideTooltip);
    g.append(loopT, accT, hit);
    el.append(g);
  });
  const cap = svg('text', { x: 4, y: top + plotH + 27, 'font-size': 10, fill: 'var(--muted)' });
  cap.textContent = 'on time';
  el.append(cap);
  return el;
}

export function renderStats(card: HTMLElement, summaries: LoopSummaryMsg[], onTime: Map<number, number>) {
  const body = card.querySelector('.stats-body') as HTMLElement;
  const loopChip = card.querySelector('.stats-loop') as HTMLElement;
  body.replaceChildren();
  const last = summaries[summaries.length - 1];
  if (!last) {
    loopChip.textContent = '';
    body.append(
      h('div', {
        class: 'stats-empty',
        text: 'Play a few loops. After each loop you will see, per instrument, how early or late you were on average and how steady.',
      }),
    );
    return;
  }
  loopChip.textContent = `loop ${last.loop + 1}`;
  const t = last.total;
  const expected = t.hits + t.missed;
  const onTimeN = onTime.get(last.loop);
  body.append(
    h(
      'div',
      { class: 'stats-total' },
      h('span', { text: describeStats('Overall', t) + '.' }),
      h('br'),
      h('span', {
        class: 'num',
        style: 'color: var(--ink-2)',
        text:
          `${t.hits} hit${t.hits === 1 ? '' : 's'} · ${t.missed} missed · ${t.extra} extra` +
          (expected && onTimeN != null ? ` · ${onTimeN}/${expected} on time` : ''),
      }),
    ),
  );

  const names = Object.keys(last.instruments).sort((a, b) =>
    isInstrument(a) && isInstrument(b) ? byStaffOrder(a, b) : a.localeCompare(b),
  );
  const table = h(
    'table',
    { class: 'stats-table' },
    h('tr', {}, h('th', { text: '' }), h('th', { text: 'On average' }), h('th', { text: 'Hit' }), h('th', { text: 'Miss' }), h('th', { text: 'Extra' }), h('th', { text: 'Last 8' })),
  );
  for (const name of names) {
    const s = last.instruments[name];
    const series = summaries.map((sum) => (sum.instruments[name]?.hits ? sum.instruments[name].meanErrorMs : null));
    const pill = h('span', { class: 'err-pill', title: describeStats(label(name), s) });
    pill.append(h('i', { style: `background:${s.hits ? errorColor(s.meanErrorMs) : 'var(--muted)'}` }), h('span', { html: meanWords(s) }));
    const sparkCell = h('td', {});
    sparkCell.append(sparkline(series));
    table.append(
      h(
        'tr',
        { title: describeStats(label(name), s) },
        h('td', { class: 'name', text: label(name) }),
        h('td', {}, pill),
        h('td', { text: String(s.hits) }),
        h('td', { text: String(s.missed), style: s.missed ? 'color: var(--bad)' : '' }),
        h('td', { text: String(s.extra), style: s.extra ? 'color: var(--extra)' : '' }),
        sparkCell,
      ),
    );
  }
  body.append(table);
  body.append(h('div', { class: 'trend-caption' }, h('span', { text: 'Average error per loop (dot) and spread (bar), % on time' }), h('span', { text: `last ${Math.min(8, summaries.length)} loops` })));
  body.append(trendChart(summaries, onTime));
}
