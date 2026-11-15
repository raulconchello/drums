// Controls dock: transport, metronome/groove/latency, mixer, stats card shell.
import { INSTRUMENT_INFO, INSTRUMENTS_BY_STAFF, isInstrument } from '../../shared/instruments';
import { LIMITS, type Command, type EngineState } from '../../shared/protocol';
import { h, throttle } from './dom';

type Send = (cmd: Command) => void;

interface SliderCtl {
  el: HTMLInputElement;
  set(v: number): void;
}

function slider(min: number, max: number, step: number, onInput: (v: number) => void, title = ''): SliderCtl {
  const el = h('input', { type: 'range', min, max, step, title }) as HTMLInputElement;
  let dragging = false;
  const paint = () => el.style.setProperty('--fill', `${((Number(el.value) - min) / (max - min)) * 100}%`);
  el.addEventListener('pointerdown', () => (dragging = true));
  el.addEventListener('pointerup', () => (dragging = false));
  el.addEventListener('change', () => (dragging = false));
  el.addEventListener('input', () => {
    paint();
    onInput(Number(el.value));
  });
  return {
    el,
    set(v: number) {
      if (dragging) return; // don't fight the user's drag with echoes from the engine
      el.value = String(v);
      paint();
    },
  };
}

interface ToggleCtl {
  el: HTMLButtonElement;
  set(on: boolean): void;
}

function toggle(onChange: (on: boolean) => void, title: string): ToggleCtl {
  const el = h('button', { class: 'toggle', title, role: 'switch' }) as HTMLButtonElement;
  el.addEventListener('click', () => onChange(!el.classList.contains('on')));
  return {
    el,
    set(on: boolean) {
      el.classList.toggle('on', on);
      el.setAttribute('aria-checked', String(on));
    },
  };
}

function segmented(options: { label: string; value: number }[], onPick: (v: number) => void, title: string) {
  const el = h('div', { class: 'seg', title });
  const buttons = options.map((o) => {
    const b = h('button', { text: o.label }) as HTMLButtonElement;
    b.addEventListener('click', () => onPick(o.value));
    el.append(b);
    return b;
  });
  return {
    el,
    set(v: number) {
      options.forEach((o, i) => buttons[i].classList.toggle('on', o.value === v));
    },
  };
}

const PLAY_ICON = '<svg viewBox="0 0 24 24"><path d="M7 4.5v15a1 1 0 0 0 1.5.86l12-7.5a1 1 0 0 0 0-1.72l-12-7.5A1 1 0 0 0 7 4.5z"/></svg>';
const STOP_ICON = '<svg viewBox="0 0 24 24"><rect x="5.5" y="5.5" width="13" height="13" rx="2"/></svg>';

/** The beat unit as a small inline SVG note (quarter, dotted quarter, eighth, half...), for "♩. = 60". */
export function beatUnitSvg(beatTicks: number, height = 22): string {
  const kinds: Record<number, [string, boolean]> = {
    240: ['8', false], 360: ['8', true], 480: ['q', false], 720: ['q', true], 960: ['h', false], 1440: ['h', true],
  };
  const [kind, dotted] = kinds[beatTicks] ?? ['q', false];
  const head =
    kind === 'h'
      ? '<ellipse cx="5.2" cy="19" rx="4" ry="2.8" transform="rotate(-22 5.2 19)" fill="none" stroke="currentColor" stroke-width="1.5"/>'
      : '<ellipse cx="5.2" cy="19" rx="4.2" ry="3" transform="rotate(-22 5.2 19)" fill="currentColor"/>';
  const stem = '<line x1="8.9" y1="18" x2="8.9" y2="2.5" stroke="currentColor" stroke-width="1.4"/>';
  const flag = kind === '8' ? '<path d="M8.9 2.5c.4 3 4.6 4.6 4.2 9.2-.1 1-.5 2-.9 2.6.2-3.4-1.6-5.4-3.3-6z" fill="currentColor"/>' : '';
  const dot = dotted ? '<circle cx="15" cy="18.6" r="1.5" fill="currentColor"/>' : '';
  const w = dotted || kind === '8' ? 17 : 12;
  return `<svg class="beat-unit" viewBox="0 0 ${w} 24" width="${(w * height) / 24}" height="${height}" aria-label="beat">${head}${stem}${flag}${dot}</svg>`;
}

export class Controls {
  private state: EngineState | null = null;
  private readonly playBtn: HTMLButtonElement;
  private readonly bpmInput: HTMLInputElement;
  private readonly bpmUnit: HTMLElement;
  private readonly bpmSlider: SliderCtl;
  private readonly loopToggle: ToggleCtl;
  private readonly countIn: ReturnType<typeof segmented>;
  private readonly recBtn: HTMLButtonElement;
  private readonly savedPath: HTMLElement;
  private readonly clickToggle: ToggleCtl;
  private readonly clickVol: SliderCtl;
  private readonly clickVolValue: HTMLElement;
  private readonly subdivToggle: ToggleCtl;
  private readonly grooveToggle: ToggleCtl;
  private readonly grooveVol: SliderCtl;
  private readonly grooveVolValue: HTMLElement;
  private readonly latency: SliderCtl;
  private readonly latencyValue: HTMLElement;
  private readonly mixerList: HTMLElement;
  private mixerRows = new Map<string, { slider: SliderCtl; value: HTMLElement; row: HTMLElement }>();
  private taps: number[] = [];
  readonly statsCard: HTMLElement;
  private readonly sendBpm: (bpm: number) => void;

  constructor(
    dock: HTMLElement,
    private readonly send: Send,
  ) {
    const sendThrottled = throttle((cmd: Command) => send(cmd), 40);
    this.sendBpm = (bpm) => sendThrottled({ type: 'setBpm', bpm: Math.round(Math.min(LIMITS.bpm.max, Math.max(LIMITS.bpm.min, bpm))) });

    // ---------- transport ----------
    this.playBtn = h('button', { class: 'play-btn', title: 'Play / stop (Space)', html: PLAY_ICON }) as HTMLButtonElement;
    this.playBtn.addEventListener('click', () => this.togglePlay());
    this.bpmUnit = h('span', { class: 'bpm-unit' });
    this.bpmInput = h('input', { class: 'bpm-number', type: 'number', min: LIMITS.bpm.min, max: LIMITS.bpm.max, step: 1, title: 'Tempo (↑/↓)' }) as HTMLInputElement;
    this.bpmInput.addEventListener('change', () => {
      const v = Number(this.bpmInput.value);
      if (Number.isFinite(v)) this.sendBpm(v);
    });
    this.bpmInput.addEventListener('keydown', (e) => {
      if (e.key === 'Enter') this.bpmInput.blur();
    });
    const minus = h('button', { class: 'step-btn', text: '−', title: 'Slower (↓, Shift = 5)' });
    minus.addEventListener('click', (e) => this.nudgeBpm((e as MouseEvent).shiftKey ? -5 : -1));
    const plus = h('button', { class: 'step-btn', text: '+', title: 'Faster (↑, Shift = 5)' });
    plus.addEventListener('click', (e) => this.nudgeBpm((e as MouseEvent).shiftKey ? 5 : 1));
    const tap = h('button', { class: 'btn', text: 'Tap', title: 'Tap tempo on the beat (T)' });
    tap.addEventListener('click', () => this.tap());
    this.bpmSlider = slider(LIMITS.bpm.min, LIMITS.bpm.max, 1, (v) => this.sendBpm(v), 'Tempo');

    this.loopToggle = toggle((on) => send({ type: 'setLoop', on }), 'Loop the exercise (L)');
    this.countIn = segmented(
      [0, 1, 2].map((v) => ({ label: String(v), value: v })),
      (bars) => send({ type: 'setCountIn', bars }),
      'Count-in bars',
    );
    this.recBtn = h('button', { class: 'btn rec-btn', title: 'Record a take (R)' }, h('span', { class: 'rec-dot' }), h('span', { class: 'rec-label', text: 'Record' })) as HTMLButtonElement;
    this.recBtn.addEventListener('click', () => this.toggleRecord());
    this.savedPath = h('div', { class: 'saved-path' });

    dock.append(
      h(
        'div',
        { class: 'card transport' },
        h('h2', { text: 'Transport' }),
        h(
          'div',
          { class: 'transport-top' },
          this.playBtn,
          h(
            'div',
            { class: 'bpm-block' },
            h('div', { class: 'bpm-display' }, this.bpmUnit, this.bpmInput, minus, plus),
            h('div', { class: 'row' }, this.bpmSlider.el, tap),
          ),
        ),
        h('div', { class: 'row' }, h('span', { class: 'label', text: 'Loop' }), this.loopToggle.el, h('span', { style: 'flex:1' }), h('span', { class: 'label', style: 'width:auto', text: 'Count-in' }), this.countIn.el),
        h('div', { class: 'row' }, this.recBtn, this.savedPath),
        h('div', {
          class: 'keys-hint',
          html: '<kbd>Space</kbd> play/stop · <kbd>↑</kbd><kbd>↓</kbd> tempo · <kbd>T</kbd> tap · <kbd>L</kbd> loop · <kbd>M</kbd> click · <kbd>G</kbd> groove · <kbd>R</kbd> record',
        }),
      ),
    );

    // ---------- metronome / groove / latency ----------
    this.clickToggle = toggle((on) => send({ type: 'setClick', on }), 'Metronome click (M)');
    this.clickVolValue = h('span', { class: 'value' });
    this.clickVol = slider(0, 1, 0.01, (v) => {
      this.clickVolValue.textContent = pct(v);
      sendThrottled({ type: 'setClick', volume: v });
    }, 'Click volume');
    this.subdivToggle = toggle((on) => send({ type: 'setClick', subdivisions: on }), 'Click the subdivisions too');
    this.grooveToggle = toggle((on) => send({ type: 'setGroove', on }), 'Play the exercise groove (G)');
    this.grooveVolValue = h('span', { class: 'value' });
    this.grooveVol = slider(0, 1, 0.01, (v) => {
      this.grooveVolValue.textContent = pct(v);
      sendThrottled({ type: 'setGroove', volume: v });
    }, 'Groove volume');
    this.latencyValue = h('span', { class: 'value' });
    this.latency = slider(LIMITS.latencyOffsetMs.min, LIMITS.latencyOffsetMs.max, 1, (v) => {
      this.latencyValue.textContent = `${v > 0 ? '+' : ''}${v} ms`;
      sendThrottled({ type: 'setLatencyOffset', ms: v });
    }, 'Latency offset: compensation applied to your hits before scoring');
    const latencyReset = h('button', { class: 'btn', text: '0', title: 'Reset latency offset' });
    latencyReset.addEventListener('click', () => send({ type: 'setLatencyOffset', ms: 0 }));

    dock.append(
      h(
        'div',
        { class: 'card sound' },
        h('h2', { text: 'Metronome & groove' }),
        h('div', { class: 'row' }, h('span', { class: 'label', text: 'Click' }), this.clickToggle.el, this.clickVol.el, this.clickVolValue),
        h('div', { class: 'row' }, h('span', { class: 'label', text: 'Subdivide' }), this.subdivToggle.el),
        h('div', { class: 'row' }, h('span', { class: 'label', text: 'Groove' }), this.grooveToggle.el, this.grooveVol.el, this.grooveVolValue),
        h('h2', { text: 'Latency offset', style: 'margin-top:6px' }),
        h('div', { class: 'row' }, this.latency.el, this.latencyValue, latencyReset),
        h('div', {
          class: 'keys-hint',
          text: 'Shifts your hits before scoring. If every hit is graded late by the same amount, raise it by that amount.',
        }),
      ),
    );

    // ---------- mixer ----------
    this.mixerList = h('div', { class: 'mixer-list' });
    dock.append(h('div', { class: 'card mixer' }, h('h2', { text: 'Mixer' }), this.mixerList));

    // ---------- stats ----------
    this.statsCard = h('div', { class: 'card stats' }, h('h2', {}, h('span', { text: 'Last loop' }), h('span', { class: 'chip stats-loop' })), h('div', { class: 'stats-body' }));
    dock.append(this.statsCard);
  }

  update(state: EngineState, exerciseInstruments: string[]) {
    this.state = state;
    this.playBtn.innerHTML = state.playing ? STOP_ICON : PLAY_ICON;
    this.playBtn.classList.toggle('playing', state.playing);
    if (document.activeElement !== this.bpmInput) this.bpmInput.value = String(Math.round(state.bpm * 10) / 10);
    this.bpmSlider.set(state.bpm);
    const unitKey = String(state.exercise?.beatTicks ?? 480);
    if (this.bpmUnit.dataset.key !== unitKey) {
      this.bpmUnit.dataset.key = unitKey;
      this.bpmUnit.innerHTML = `${beatUnitSvg(state.exercise?.beatTicks ?? 480, 30)}<span>=</span>`;
    }
    this.loopToggle.set(state.loop);
    this.countIn.set(state.countInBars);
    this.recBtn.classList.toggle('recording', state.recording);
    (this.recBtn.querySelector('.rec-label') as HTMLElement).textContent = state.recording ? 'Stop recording' : 'Record';

    this.clickToggle.set(state.click.on);
    this.clickVol.set(state.click.volume);
    this.clickVolValue.textContent = pct(state.click.volume);
    this.subdivToggle.set(state.click.subdivisions);
    this.grooveToggle.set(state.groove.on);
    this.grooveVol.set(state.groove.volume);
    this.grooveVolValue.textContent = pct(state.groove.volume);
    this.latency.set(state.latencyOffsetMs);
    this.latencyValue.textContent = `${state.latencyOffsetMs > 0 ? '+' : ''}${Math.round(state.latencyOffsetMs)} ms`;
    this.updateMixer(state.mixer, exerciseInstruments);
  }

  private updateMixer(mixer: Record<string, number>, used: string[]) {
    const channels = Object.keys(mixer).sort((a, b) => {
      if (a === 'master') return -1;
      if (b === 'master') return 1;
      const ua = used.includes(a) ? 0 : 1;
      const ub = used.includes(b) ? 0 : 1;
      if (ua !== ub) return ua - ub;
      return INSTRUMENTS_BY_STAFF.indexOf(a as never) - INSTRUMENTS_BY_STAFF.indexOf(b as never);
    });
    const key = channels.join(',');
    if (this.mixerList.dataset.key !== key) {
      this.mixerList.dataset.key = key;
      this.mixerList.replaceChildren();
      this.mixerRows.clear();
      for (const ch of channels) {
        const value = h('span', { class: 'value' });
        const s = slider(LIMITS.mixer.min, LIMITS.mixer.max, 0.01, (v) => {
          value.textContent = pct(v);
          this.sendMixer(ch, v);
        }, `${ch} volume`);
        s.el.addEventListener('dblclick', () => this.send({ type: 'setMixer', channel: ch, volume: 1 }));
        const name = ch === 'master' ? 'Master' : isInstrument(ch) ? INSTRUMENT_INFO[ch].short : ch;
        const row = h('div', { class: `row${ch === 'master' ? ' master' : ''}` }, h('span', { class: 'label', text: name }), s.el, value);
        this.mixerList.append(row);
        this.mixerRows.set(ch, { slider: s, value, row });
      }
    }
    for (const [ch, r] of this.mixerRows) {
      r.slider.set(mixer[ch]);
      r.value.textContent = pct(mixer[ch]);
      r.row.classList.toggle('dim', ch !== 'master' && !used.includes(ch));
    }
  }

  private mixerSenders = new Map<string, (v: number) => void>();
  private sendMixer(channel: string, volume: number) {
    let f = this.mixerSenders.get(channel);
    if (!f) {
      f = throttle((v: number) => this.send({ type: 'setMixer', channel, volume: v }), 40);
      this.mixerSenders.set(channel, f);
    }
    f(volume);
  }

  showSavedPath(path: string) {
    this.savedPath.textContent = `Saved ${path}`;
    this.savedPath.title = path;
  }

  // ---------- actions (also used by keyboard shortcuts) ----------
  togglePlay() {
    if (!this.state) return;
    this.send({ type: this.state.playing ? 'stop' : 'play' });
  }

  nudgeBpm(delta: number) {
    if (!this.state) return;
    const v = Math.round(this.state.bpm) + delta;
    this.state = { ...this.state, bpm: Math.min(LIMITS.bpm.max, Math.max(LIMITS.bpm.min, v)) };
    this.bpmInput.value = String(this.state.bpm);
    this.bpmSlider.set(this.state.bpm);
    this.sendBpm(v);
  }

  tap() {
    const now = performance.now();
    if (this.taps.length && now - this.taps[this.taps.length - 1] > 2000) this.taps = [];
    this.taps.push(now);
    this.taps = this.taps.slice(-6);
    if (this.taps.length >= 2) {
      const intervals = this.taps.slice(1).map((t, i) => t - this.taps[i]);
      const avg = intervals.reduce((s, v) => s + v, 0) / intervals.length;
      this.sendBpm(60000 / avg);
    }
  }

  toggleLoop() {
    if (this.state) this.send({ type: 'setLoop', on: !this.state.loop });
  }

  toggleClick() {
    if (this.state) this.send({ type: 'setClick', on: !this.state.click.on });
  }

  toggleGroove() {
    if (this.state) this.send({ type: 'setGroove', on: !this.state.groove.on });
  }

  toggleRecord() {
    if (this.state) this.send({ type: this.state.recording ? 'stopRecording' : 'startRecording' });
  }

  setEnabled(enabled: boolean) {
    document.querySelectorAll<HTMLButtonElement | HTMLInputElement>('.dock button, .dock input').forEach((el) => (el.disabled = !enabled));
  }
}

function pct(v: number): string {
  return `${Math.round(v * 100)}%`;
}
