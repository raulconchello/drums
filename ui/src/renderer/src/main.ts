// Renderer entry: wires the engine connection to the notation, lanes, pads, controls and stats.
import type { AppInfo, EngineProcessStatus, ExerciseEntry, OmarchyTheme } from '../../preload/api';
import { splitPosition, TransportClock } from '../../shared/clock';
import { barTicks, exerciseInstruments, lengthTicks, msPerTick } from '../../shared/exercise';
import { GRADE_COLORS, gradeColor } from '../../shared/grade';
import { INSTRUMENT_INFO } from '../../shared/instruments';
import type { Command, EngineMessage, EngineState, Exercise, HelloMsg } from '../../shared/protocol';
import { Connection, type ConnStatus } from './connection';
import { beatUnitSvg, Controls } from './controls';
import { $, escapeHtml, h, toast } from './dom';
import { Lanes } from './lanes';
import { Session } from './model';
import { NotationView } from './notation';
import { Pads } from './pads';
import { renderStats } from './stats';
import { applyTheme, laneTheme, notationTheme, savedTheme, saveTheme, type ThemeName } from './theme';

const api = window.drums;

const fallbackInfo: AppInfo = {
  repoRoot: '',
  wsUrl: 'ws://127.0.0.1:7878',
  engine: 'external',
  engineBinary: '',
  screenshot: null,
  theme: null,
};

// ---------- app state ----------
let info: AppInfo = fallbackInfo;
let engineStatus: EngineProcessStatus = 'external';
let connStatus: ConnStatus = 'connecting';
let hello: HelloMsg | null = null;
let state: EngineState | null = null;
let exerciseKey = '';
let notation: NotationView | null = null;
let notationLoopFlag: boolean | null = null;
let currentLoop = 0;
let marksDirty = true;
let showLabels = false;
let hideFeetRests = true;
let laneZoom = 1;
let themeName: ThemeName = savedTheme();
let omarchy: OmarchyTheme | null = null;
let exercises: ExerciseEntry[] = [];
let firstStateAt = 0;
let screenshotSent = false;
let recordingSince = 0;
let countInKey = '';
const startedAt = performance.now();

const session = new Session();
const clock = new TransportClock();

// ---------- DOM ----------
const notationHost = $('notation');
const countInEl = $('countin');
const offlineEl = $('offline');
const exSelect = $('exercise-select') as HTMLSelectElement;
const kitSelect = $('kit-select') as HTMLSelectElement;
const themeBtn = $('theme-btn') as HTMLButtonElement;
const connEl = $('conn');
const loopChip = $('loop-chip');
const transportChip = $('transport-chip');
const recChip = $('rec-chip');
const recTime = $('rec-time');

let conn: Connection | null = null;
const send = (cmd: Command) => {
  if (!conn?.send(cmd)) toast('Not connected to the engine', 'error', 2500);
};
const controls = new Controls($('dock'), send);
const lanes = new Lanes($('lanes') as HTMLCanvasElement, laneTheme());
const pads = new Pads($('pads'), (inst) => {
  if (inst) toast(`Hit the pad you want to use as <b>${escapeHtml(INSTRUMENT_INFO[inst].label)}</b>… (Esc cancels)`, 'info', 4000);
});
renderStats(controls.statsCard, [], new Map());

// ---------- legend + view options ----------
function buildLegend() {
  const legend = $('legend');
  const item = (color: string, text: string, cls = '') => h('span', {}, h('i', { class: cls, style: cls ? '' : `background:${color}` }), text);
  const labels = h('input', { type: 'checkbox' }) as HTMLInputElement;
  labels.addEventListener('change', () => {
    showLabels = labels.checked;
    marksDirty = true;
  });
  const rests = h('input', { type: 'checkbox' }) as HTMLInputElement;
  rests.addEventListener('change', () => {
    hideFeetRests = !rests.checked;
    rebuildNotation();
  });
  const zoom = h('select', { title: 'Magnify timing errors in the lanes' }) as HTMLSelectElement;
  for (const z of [1, 2, 4]) zoom.append(h('option', { value: z, text: z === 1 ? 'to scale' : `errors ×${z}` }));
  zoom.addEventListener('change', () => (laneZoom = Number(zoom.value)));
  legend.append(
    item(GRADE_COLORS.onTime, 'on time (±15 ms)'),
    h('span', {}, h('i', { style: `background:linear-gradient(90deg, ${GRADE_COLORS.amber}, ${GRADE_COLORS.red})` }), 'early / late (amber → red)'),
    item(GRADE_COLORS.extra, 'extra'),
    item('', 'missed', 'ring'),
    h('span', { class: 'legend-hint' }, h('label', {}, labels, 'ms labels'), h('label', {}, rests, 'all rests'), h('label', {}, 'lanes', zoom)),
  );
}
buildLegend();

// ---------- theme ----------
function updateThemeButton() {
  themeBtn.hidden = !omarchy;
  themeBtn.textContent = themeName === 'omarchy' && omarchy ? `Theme: ${omarchy.name}` : 'Theme: dark';
}

function setTheme(name: ThemeName) {
  themeName = name;
  saveTheme(name);
  applyTheme(name, omarchy);
  lanes.setTheme(laneTheme());
  updateThemeButton();
  rebuildNotation();
}

themeBtn.addEventListener('click', () => setTheme(themeName === 'dark' ? 'omarchy' : 'dark'));

// ---------- notation ----------
function rebuildNotation() {
  const ex = state?.exercise;
  if (!ex) {
    notationHost.replaceChildren();
    notation = null;
    return;
  }
  try {
    notation = new NotationView(notationHost, ex, { theme: notationTheme(), loop: state!.loop, hideFeetRests });
    notationLoopFlag = state!.loop;
  } catch (e) {
    console.error(e);
    notationHost.replaceChildren(h('div', { class: 'stats-empty', text: `Could not draw this exercise: ${(e as Error).message}` }));
    notation = null;
  }
  marksDirty = true;
}

function exerciseSignature(ex: Exercise | null): string {
  return ex ? JSON.stringify([ex.id, ex.timeSignature, ex.bars, ex.beatTicks, ex.notes]) : '';
}

// ---------- engine messages ----------
function onMessage(m: EngineMessage) {
  switch (m.type) {
    case 'hello':
      hello = m;
      if (m.protocol !== 1) toast(`Engine speaks protocol ${m.protocol}; this UI expects 1`, 'error', 8000);
      updateConnText();
      break;
    case 'state':
      onState(m);
      break;
    case 'position':
      if (state?.exercise) clock.update(m, msPerTick(state.exercise, state.bpm), performance.now());
      break;
    case 'hit': {
      const learning = pads.learning;
      if (learning && Number.isInteger(m.note)) {
        send({ type: 'setPadMap', note: m.note, instrument: learning });
        toast(`MIDI note ${m.note} → <b>${escapeHtml(INSTRUMENT_INFO[learning].label)}</b>`);
        pads.setLearning(null);
      }
      if (m.grade === 'free') {
        pads.flash(m.instrument, GRADE_COLORS.free, m.velocity);
        break;
      }
      pads.flash(m.instrument, gradeColor(m.grade, m.errorMs), m.velocity);
      if (state?.exercise) {
        session.addHit(m, lengthTicks(state.exercise));
        if (lanes.ensureInstruments([m.instrument])) marksDirty = true;
        marksDirty = true;
      }
      break;
    }
    case 'missed':
      session.addMiss(m);
      marksDirty = true;
      break;
    case 'loopSummary':
      session.addSummary(m);
      renderStats(controls.statsCard, session.summaries, session.onTimeByLoop);
      break;
    case 'recordingSaved':
      controls.showSavedPath(m.path);
      toast(`Recording saved<br><code>${escapeHtml(m.path)}</code>`, 'info', 9000);
      break;
    case 'error':
      toast(escapeHtml(m.message), 'error', 6000);
      break;
  }
}

function onState(m: EngineState) {
  const prev = state;
  state = m;
  if (!firstStateAt) firstStateAt = performance.now();
  const sig = exerciseSignature(m.exercise);
  const used = m.exercise ? exerciseInstruments(m.exercise) : [];

  if (sig !== exerciseKey) {
    exerciseKey = sig;
    session.clearAll();
    renderStats(controls.statsCard, [], new Map());
    lanes.setExercise(m.exercise);
    rebuildNotation();
  } else if (notationLoopFlag !== m.loop) {
    rebuildNotation();
  }

  if (m.playing && !prev?.playing) {
    // new take: forget the marks of the previous run (stats history stays)
    session.clear();
    currentLoop = 0;
    marksDirty = true;
  }
  if (m.recording && !prev?.recording) recordingSince = performance.now();

  controls.update(m, used);
  pads.update(m.padMap, used);
  updateHeader();
  updatePickers();
}

function updateHeader() {
  const ex = state?.exercise;
  $('ex-title').textContent = ex ? ex.title : 'No exercise loaded';
  $('ex-meta').innerHTML = ex
    ? [
        `${ex.timeSignature[0]}/${ex.timeSignature[1]}`,
        `${beatUnitSvg(ex.beatTicks, 15)} = ${Math.round(state!.bpm)}`,
        `${ex.bars} bar${ex.bars === 1 ? '' : 's'}`,
        state!.loop ? 'looping' : 'plays once',
        state!.kit?.name ? `kit: ${escapeHtml(state!.kit.name)}` : '',
      ]
        .filter(Boolean)
        .join('<span class="sep">·</span>')
    : '';
  transportChip.textContent = state?.playing ? '▶ PLAYING' : '■ STOPPED';
  transportChip.classList.toggle('playing', !!state?.playing);
  recChip.hidden = !state?.recording;
}

function updatePickers() {
  // exercises
  const currentId = state?.exercise?.id;
  const known = exercises.find((e) => e.id === currentId);
  const optsKey = JSON.stringify([exercises.map((e) => e.file), known ? '' : currentId]);
  if (exSelect.dataset.key !== optsKey) {
    exSelect.dataset.key = optsKey;
    exSelect.replaceChildren();
    if (currentId && !known) exSelect.append(h('option', { value: '', text: `${state?.exercise?.title ?? currentId}` }));
    for (const e of exercises) exSelect.append(h('option', { value: e.file, text: e.title, disabled: e.invalid }));
    if (!currentId && exercises.length === 0) exSelect.append(h('option', { value: '', text: 'No exercises found' }));
  }
  exSelect.value = known ? known.file : '';

  // kits
  const kits = state?.kits ?? [];
  const kitKey = JSON.stringify(kits);
  if (kitSelect.dataset.key !== kitKey) {
    kitSelect.dataset.key = kitKey;
    kitSelect.replaceChildren(...kits.map((k) => h('option', { value: k.path, text: k.name })));
    if (kits.length === 0) kitSelect.append(h('option', { value: '', text: 'No kits' }));
  }
  if (state?.kit) kitSelect.value = state.kit.path;
}

exSelect.addEventListener('change', () => {
  if (exSelect.value) send({ type: 'loadExercise', path: exSelect.value });
});
kitSelect.addEventListener('change', () => {
  if (kitSelect.value) send({ type: 'loadKit', path: kitSelect.value });
});
$('open-exercise').addEventListener('click', async () => {
  const path = await api?.openExerciseDialog();
  if (path) send({ type: 'loadExercise', path });
});

// ---------- connection status ----------
function onConnStatus(s: ConnStatus) {
  connStatus = s;
  if (s !== 'open') hello = null;
  updateConnText();
  controls.setEnabled(s === 'open');
  if (s === 'open') send({ type: 'getState' });
}

function updateConnText() {
  connEl.dataset.status = connStatus;
  const text = connEl.querySelector('.conn-text')!;
  if (connStatus === 'open') {
    text.innerHTML = hello
      ? `Engine<span class="conn-detail"> · ${(hello.sampleRate / 1000).toFixed(1)} kHz · ${hello.bufferFrames} frames · ${hello.outputLatencyMs.toFixed(1)} ms out</span>`
      : 'Engine connected';
    connEl.title =
      `${info.wsUrl}${engineStatus === 'spawned' ? ' (started by the UI)' : ''}` +
      (hello ? `\n${hello.sampleRate} Hz · ${hello.bufferFrames} frames · output latency ${hello.outputLatencyMs.toFixed(1)} ms` : '');
    offlineEl.hidden = true;
    return;
  }
  text.textContent = connStatus === 'connecting' ? 'Connecting…' : 'Engine offline · retrying';
  connEl.title = info.wsUrl;
  offlineEl.hidden = false;
  const hint =
    engineStatus === 'missing'
      ? `No engine is running and <code>engine/build/drums-engine</code> is not built.<br>Start the simulator with <code>npm run mock</code> in <code>ui/</code>, or build the engine.`
      : engineStatus === 'exited'
        ? 'The engine process exited. Check the terminal output, then restart the app.'
        : engineStatus === 'spawned'
          ? performance.now() - startedAt > 6000
            ? `The engine was started (<code>engine/build/drums-engine</code>) but does not answer on <code>${escapeHtml(info.wsUrl)}</code>.<br>Was it built with the control server (drums_core)? Its output is in the terminal that started the UI.`
            : 'Starting the engine…'
          : `Waiting for the engine on <code>${escapeHtml(info.wsUrl)}</code>…`;
  offlineEl.innerHTML = `<div>${hint}</div>`;
}

// ---------- keyboard ----------
function typingTarget(t: EventTarget | null): boolean {
  if (!(t instanceof HTMLElement)) return false;
  if (t instanceof HTMLSelectElement) return true;
  if (t instanceof HTMLInputElement) return t.type === 'number' || t.type === 'text';
  return t.isContentEditable;
}

window.addEventListener('keydown', (e) => {
  if (e.ctrlKey || e.metaKey || e.altKey) return;
  const typing = typingTarget(e.target);
  if (e.key === 'Escape' && pads.learning) {
    pads.setLearning(null);
    return;
  }
  if (typing) return;
  const onRange = e.target instanceof HTMLInputElement && e.target.type === 'range';
  switch (e.key) {
    case ' ':
      e.preventDefault();
      if (!e.repeat) controls.togglePlay();
      break;
    case 'ArrowUp':
    case 'ArrowDown':
      if (onRange) return;
      e.preventDefault();
      controls.nudgeBpm((e.key === 'ArrowUp' ? 1 : -1) * (e.shiftKey ? 5 : 1));
      break;
    case 't':
    case 'T':
      controls.tap();
      break;
    case 'l':
    case 'L':
      controls.toggleLoop();
      break;
    case 'm':
    case 'M':
      controls.toggleClick();
      break;
    case 'g':
    case 'G':
      controls.toggleGroove();
      break;
    case 'r':
    case 'R':
      controls.toggleRecord();
      break;
  }
});
// Keep Space from also "clicking" a focused button.
window.addEventListener('keyup', (e) => {
  if (e.key === ' ' && !typingTarget(e.target)) e.preventDefault();
});

// ---------- resize ----------
let lastWidth = 0;
let resizeTimer = 0;
new ResizeObserver(() => {
  const w = notationHost.clientWidth;
  requestAnimationFrame(() => lanes.resize());
  if (Math.abs(w - lastWidth) < 2) return;
  lastWidth = w;
  window.clearTimeout(resizeTimer);
  resizeTimer = window.setTimeout(rebuildNotation, 90);
}).observe(notationHost);

// ---------- frame loop ----------
function renderCountIn(pos: number, ex: Exercise) {
  const bt = barTicks(ex);
  const beatsPerBar = Math.max(1, Math.round(bt / ex.beatTicks));
  const countInTicks = (state?.countInBars ?? 1) * bt;
  const beatIndex = Math.max(0, Math.floor((pos + countInTicks) / ex.beatTicks));
  const bar = Math.floor(beatIndex / beatsPerBar);
  const now = beatIndex % beatsPerBar;
  const key = `${bar}:${now}:${beatsPerBar}`;
  if (key === countInKey) return;
  countInKey = key;
  const bars = state?.countInBars ?? 1;
  const beats = Array.from({ length: beatsPerBar }, (_, i) =>
    h('div', { class: `countin-beat${i < now ? ' past' : i === now ? ' now' : ''}`, text: String(i + 1) }),
  );
  countInEl.replaceChildren(
    h('div', { class: 'countin-label', text: bars > 1 ? `Count-in · bar ${bar + 1} of ${bars}` : 'Count-in' }),
    h('div', { class: 'countin-beats' }, ...beats),
  );
}

let frames = 0;
function frame() {
  frames++;
  const now = performance.now();
  const ex = state?.exercise;
  if (ex && notation) {
    const L = lengthTicks(ex);
    let tick: number | null = null;
    let counting = false;
    if (clock.playing) {
      const pos = clock.position(now);
      if (pos < 0) {
        counting = true;
        renderCountIn(pos, ex);
      } else {
        const sp = splitPosition(pos, L, state!.loop);
        tick = sp.tickInLoop;
        if (state!.loop && sp.loop !== currentLoop && sp.loop >= 0) {
          currentLoop = sp.loop;
          session.prune(currentLoop);
          marksDirty = true;
        }
      }
    }
    if (countInEl.hidden === counting) countInEl.hidden = !counting;
    if (!counting) countInKey = '';
    notation.setPlayhead(tick);
    if (marksDirty) {
      notation.renderMarks(session.visibleHits(currentLoop), session.visibleMisses(currentLoop), currentLoop, showLabels);
      marksDirty = false;
    }
    lanes.draw({ tick, hits: session.visibleHits(currentLoop), misses: session.visibleMisses(currentLoop), currentLoop, zoom: laneZoom });
    loopChip.textContent = clock.playing && !counting && state!.loop ? `LOOP ${currentLoop + 1}` : '';
  } else {
    countInEl.hidden = true;
  }
  if (connStatus !== 'open' && engineStatus === 'spawned' && frames % 60 === 0) updateConnText();
  if (state?.recording) {
    const s = Math.floor((now - recordingSince) / 1000);
    recTime.textContent = `${Math.floor(s / 60)}:${String(s % 60).padStart(2, '0')}`;
  }

  if (info.screenshot && !screenshotSent) {
    const sinceData = firstStateAt ? now - firstStateAt : 0;
    const ready =
      connStatus === 'open' && state && firstStateAt
        ? (state.playing && sinceData > info.screenshot.waitMs) || (!state.playing && sinceData > 2500)
        : now - startedAt > info.screenshot.waitMs; // no engine: capture the offline screen
    if (ready) {
      screenshotSent = true;
      console.log(`screenshot ready: ${Math.round(sinceData)} ms after first state, ${frames} frames`);
      requestAnimationFrame(() => api?.screenshotReady());
    }
  }
  requestAnimationFrame(frame);
}

// ---------- start ----------
async function start() {
  if (api) {
    info = await api.appInfo();
    engineStatus = info.engine;
    api.onEngineStatus((s) => {
      engineStatus = s;
      updateConnText();
    });
    [exercises, omarchy] = await Promise.all([api.listExercises(), api.omarchyTheme()]);
    if (info.theme) themeName = info.theme;
  }
  applyTheme(themeName, omarchy);
  lanes.setTheme(laneTheme());
  updateThemeButton();
  updatePickers();
  updateConnText();
  controls.setEnabled(false);
  conn = new Connection(info.wsUrl, onMessage, onConnStatus);
  conn.start();
  requestAnimationFrame(frame);
}

void start();
