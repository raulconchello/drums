// Tiny DOM helpers.

type Attrs = Record<string, string | number | boolean | undefined | null | ((e: Event) => void)>;
type Child = Node | string | null | undefined | false;

export function h<K extends keyof HTMLElementTagNameMap>(tag: K, attrs: Attrs = {}, ...children: Child[]): HTMLElementTagNameMap[K] {
  const el = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs)) {
    if (v === undefined || v === null || v === false) continue;
    if (typeof v === 'function') el.addEventListener(k.replace(/^on/, '').toLowerCase(), v as EventListener);
    else if (k === 'class') el.className = String(v);
    else if (k === 'text') el.textContent = String(v);
    else if (k === 'html') el.innerHTML = String(v);
    else if (v === true) el.setAttribute(k, '');
    else el.setAttribute(k, String(v));
  }
  for (const c of children) if (c !== null && c !== undefined && c !== false) el.append(c);
  return el;
}

const SVG_NS = 'http://www.w3.org/2000/svg';

export function svg<K extends keyof SVGElementTagNameMap>(tag: K, attrs: Record<string, string | number> = {}): SVGElementTagNameMap[K] {
  const el = document.createElementNS(SVG_NS, tag);
  for (const [k, v] of Object.entries(attrs)) el.setAttribute(k, String(v));
  return el;
}

export function $(id: string): HTMLElement {
  const el = document.getElementById(id);
  if (!el) throw new Error(`#${id} missing`);
  return el;
}

export function cssVar(name: string, el: Element = document.documentElement): string {
  return getComputedStyle(el).getPropertyValue(name).trim();
}

export function throttle<T extends unknown[]>(fn: (...a: T) => void, ms: number): (...a: T) => void {
  let last = 0;
  let timer: number | null = null;
  let pending: T | null = null;
  return (...a: T) => {
    const now = performance.now();
    pending = a;
    if (now - last >= ms) {
      last = now;
      fn(...a);
      pending = null;
    } else if (timer === null) {
      timer = window.setTimeout(() => {
        timer = null;
        last = performance.now();
        if (pending) fn(...pending);
        pending = null;
      }, ms - (now - last));
    }
  };
}

// ---------- tooltip ----------
const tooltip = () => document.getElementById('tooltip')!;

export function showTooltip(html: string, x: number, y: number) {
  const t = tooltip();
  t.innerHTML = html;
  t.hidden = false;
  const r = t.getBoundingClientRect();
  let left = x + 14;
  let top = y + 14;
  if (left + r.width > window.innerWidth - 8) left = x - r.width - 14;
  if (top + r.height > window.innerHeight - 8) top = y - r.height - 14;
  t.style.left = `${Math.max(8, left)}px`;
  t.style.top = `${Math.max(8, top)}px`;
}

export function hideTooltip() {
  tooltip().hidden = true;
}

// ---------- toasts ----------
export function toast(html: string, kind: 'info' | 'error' = 'info', ms = 5000) {
  const box = document.getElementById('toasts')!;
  const el = h('div', { class: `toast ${kind}`, html });
  box.append(el);
  while (box.children.length > 4) box.firstElementChild?.remove();
  window.setTimeout(() => el.remove(), ms);
}

export function escapeHtml(s: string): string {
  return s.replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' })[c]!);
}
