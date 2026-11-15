// Themes: built-in dark (default) or the current Omarchy theme's colours (light or dark).
import type { OmarchyTheme } from '../../preload/api';
import type { LaneTheme } from './lanes';
import type { NotationTheme } from './notation';

export type ThemeName = 'dark' | 'omarchy';

const STORAGE_KEY = 'drums.theme';

export function savedTheme(): ThemeName {
  try {
    return localStorage.getItem(STORAGE_KEY) === 'omarchy' ? 'omarchy' : 'dark';
  } catch {
    return 'dark';
  }
}

export function saveTheme(name: ThemeName) {
  try {
    localStorage.setItem(STORAGE_KEY, name);
  } catch {
    /* per-viewer convenience only */
  }
}

const TOKENS = ['--bg', '--panel', '--panel-2', '--panel-3', '--line', '--ink', '--ink-2', '--muted', '--accent', '--accent-ink', '--staff', '--score-ink', '--score-bg'];

export function applyTheme(name: ThemeName, omarchy: OmarchyTheme | null) {
  const root = document.documentElement;
  for (const t of TOKENS) root.style.removeProperty(t);
  root.dataset.mode = 'dark';
  if (name !== 'omarchy' || !omarchy) return;
  const c = omarchy.colors;
  const bg = c.background;
  const fg = c.bright_foreground ?? c.foreground;
  const mix = (a: string, pct: number, b: string) => `color-mix(in srgb, ${a} ${pct}%, ${b})`;
  const set: Record<string, string> = {
    '--bg': bg,
    '--panel': c.dark_background ?? mix(fg, 4, bg),
    '--panel-2': c.darker_background ?? mix(fg, 8, bg),
    '--panel-3': c.lighter_background ?? mix(fg, 12, bg),
    '--line': mix(c.foreground, 18, bg),
    '--ink': fg,
    '--ink-2': c.foreground,
    '--muted': c.muted ?? mix(c.foreground, 60, bg),
    '--accent': c.accent ?? c.blue ?? fg,
    '--accent-ink': omarchy.mode === 'light' ? '#ffffff' : bg,
    '--staff': mix(c.foreground, 60, bg),
    '--score-ink': fg,
    '--score-bg': omarchy.mode === 'light' ? mix('#ffffff', 60, bg) : mix('#000000', 18, bg),
  };
  for (const [k, v] of Object.entries(set)) if (v) root.style.setProperty(k, v);
  root.dataset.mode = omarchy.mode;
}

/** Resolves any CSS colour (var(), color-mix()...) to rgb() so VexFlow and canvas can use it. */
const probeCanvas = document.createElement('canvas');
probeCanvas.width = probeCanvas.height = 1;
const probeCtx = probeCanvas.getContext('2d', { willReadFrequently: true })!;

export function resolveVar(name: string): string {
  const probe = document.createElement('span');
  probe.style.color = `var(${name})`;
  probe.style.display = 'none';
  document.body.append(probe);
  const css = getComputedStyle(probe).color;
  probe.remove();
  probeCtx.clearRect(0, 0, 1, 1);
  probeCtx.fillStyle = '#000';
  probeCtx.fillStyle = css;
  probeCtx.fillRect(0, 0, 1, 1);
  const [r, g, b] = probeCtx.getImageData(0, 0, 1, 1).data;
  return `rgb(${r}, ${g}, ${b})`;
}

export function notationTheme(): NotationTheme {
  return { ink: resolveVar('--score-ink'), staff: resolveVar('--staff'), accent: resolveVar('--accent'), bg: resolveVar('--score-bg') };
}

export function laneTheme(): LaneTheme {
  return {
    bg: resolveVar('--panel'),
    ink: resolveVar('--ink'),
    ink2: resolveVar('--ink-2'),
    muted: resolveVar('--muted'),
    line: resolveVar('--line'),
    accent: resolveVar('--accent'),
    panel2: resolveVar('--panel-2'),
  };
}
