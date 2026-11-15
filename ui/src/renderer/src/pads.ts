// Pad strip: one pad per instrument; flashes on every hit (free hits while stopped included).
// Click a pad to "learn": the next hit's MIDI note is mapped to that instrument (setPadMap).
import { GRADE_COLORS } from '../../shared/grade';
import { INSTRUMENT_INFO, INSTRUMENTS_BY_STAFF, type Instrument } from '../../shared/instruments';
import { h } from './dom';

export class Pads {
  private pads = new Map<string, { el: HTMLElement; note: HTMLElement; flash: HTMLElement }>();
  learning: Instrument | null = null;

  constructor(
    host: HTMLElement,
    private readonly onLearnChange: (inst: Instrument | null) => void,
  ) {
    for (const inst of INSTRUMENTS_BY_STAFF) {
      const flash = h('div', { class: 'pad-flash' });
      const note = h('div', { class: 'pad-note' });
      const el = h('div', { class: 'pad', title: `${INSTRUMENT_INFO[inst].label} — click, then hit a pad to map it here` }, flash, h('div', { class: 'pad-name', text: INSTRUMENT_INFO[inst].short }), note);
      el.addEventListener('click', () => this.setLearning(this.learning === inst ? null : inst));
      host.append(el);
      this.pads.set(inst, { el, note, flash });
    }
  }

  setLearning(inst: Instrument | null) {
    this.learning = inst;
    for (const [name, p] of this.pads) p.el.classList.toggle('learning', name === inst);
    this.onLearnChange(inst);
  }

  update(padMap: Record<string, string>, used: string[]) {
    for (const [name, p] of this.pads) {
      const notes = Object.entries(padMap)
        .filter(([, v]) => v === name)
        .map(([k]) => k);
      p.note.textContent = notes.length ? notes.slice(0, 3).join(' ') + (notes.length > 3 ? '…' : '') : '—';
      p.note.title = notes.length ? `MIDI notes ${notes.join(', ')}` : 'not mapped';
      p.el.classList.toggle('in-exercise', used.includes(name));
    }
  }

  flash(instrument: string, color: string = GRADE_COLORS.free, velocity = 100) {
    const p = this.pads.get(instrument);
    if (!p) return;
    const strength = 0.25 + 0.4 * Math.min(1, velocity / 127);
    p.flash.style.background = color;
    p.flash.animate([{ opacity: strength }, { opacity: 0 }], { duration: 420, easing: 'ease-out' });
    p.el.animate([{ transform: 'scale(1.04)' }, { transform: 'scale(1)' }], { duration: 160, easing: 'ease-out' });
  }
}
