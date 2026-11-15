// Spawns the mock engine and drives it over the real protocol.
import { spawn, type ChildProcess } from 'node:child_process';
import { resolve } from 'node:path';
import { afterAll, beforeAll, describe, expect, it } from 'vitest';
import { WebSocket } from 'ws';
import type { EngineMessage, EngineState } from '../src/shared/protocol';

const PORT = 7900 + Math.floor(Math.random() * 90);
const ui = resolve(__dirname, '..');
let mock: ChildProcess;
let ws: WebSocket;
const inbox: EngineMessage[] = [];

function waitFor<T extends EngineMessage>(pred: (m: EngineMessage) => m is T, timeoutMs = 5000, from = 0): Promise<T> {
  return new Promise((res, rej) => {
    const start = Date.now();
    const tick = () => {
      const found = inbox.slice(from).find(pred);
      if (found) return res(found as T);
      if (Date.now() - start > timeoutMs) return rej(new Error('timeout waiting for message'));
      setTimeout(tick, 10);
    };
    tick();
  });
}

const isState = (m: EngineMessage): m is EngineState => m.type === 'state';
const send = (cmd: object) => ws.send(JSON.stringify(cmd));
async function command(cmd: object): Promise<EngineState> {
  const from = inbox.length;
  send(cmd);
  return waitFor(isState, 3000, from);
}

beforeAll(async () => {
  mock = spawn(resolve(ui, 'node_modules/.bin/tsx'), ['mock/mock-engine.ts', '--port', String(PORT), '--seed', '5', '--no-idle-hits', '--miss', '0.2'], {
    cwd: ui,
    stdio: ['ignore', 'pipe', 'inherit'],
  });
  await new Promise<void>((res) => mock.stdout!.on('data', (d) => String(d).includes('mock engine on') && res()));
  ws = new WebSocket(`ws://127.0.0.1:${PORT}`);
  ws.on('message', (d) => inbox.push(JSON.parse(String(d))));
  await new Promise((res) => ws.once('open', res));
}, 20000);

afterAll(() => {
  ws?.close();
  mock?.kill('SIGTERM');
});

describe('mock engine', () => {
  it('greets with hello + state (canonical exercise)', async () => {
    const hello = await waitFor((m): m is Extract<EngineMessage, { type: 'hello' }> => m.type === 'hello');
    expect(hello.protocol).toBe(1);
    const st = await waitFor(isState);
    expect(st.exercise?.id).toBe('groove-12-8-bar23');
    expect(st.exercise?.notes).toHaveLength(17);
    expect(st.mixer.master).toBeGreaterThan(0);
  });

  it('applies settings commands and echoes state', async () => {
    expect((await command({ type: 'setBpm', bpm: 300 })).bpm).toBe(300);
    expect((await command({ type: 'setCountIn', bars: 0 })).countInBars).toBe(0);
    expect((await command({ type: 'setClick', volume: 0.5, subdivisions: false })).click).toEqual({ on: true, volume: 0.5, subdivisions: false });
    expect((await command({ type: 'setGroove', on: true })).groove.on).toBe(true);
    expect((await command({ type: 'setMixer', channel: 'snare', volume: 1.2 })).mixer.snare).toBe(1.2);
    expect((await command({ type: 'setLatencyOffset', ms: 6 })).latencyOffsetMs).toBe(6);
    expect((await command({ type: 'setPadMap', note: 99, instrument: 'tom1' })).padMap['99']).toBe('tom1');
    expect((await command({ type: 'setPadMap', note: 99, instrument: null })).padMap['99']).toBeUndefined();
  });

  it('rejects bad commands with an error and keeps the connection', async () => {
    const from = inbox.length;
    send({ type: 'setBpm', bpm: 1000 });
    send({ type: 'nope' });
    send({ type: 'loadExercise', path: 'exercises/missing.json' });
    const errors = await new Promise<EngineMessage[]>((res) => setTimeout(() => res(inbox.slice(from).filter((m) => m.type === 'error')), 300));
    expect(errors).toHaveLength(3);
    expect(ws.readyState).toBe(WebSocket.OPEN);
  });

  it('plays: positions, hits, misses and a loop summary', async () => {
    const from = inbox.length;
    expect((await command({ type: 'play' })).playing).toBe(true);
    const summary = await waitFor((m): m is Extract<EngineMessage, { type: 'loopSummary' }> => m.type === 'loopSummary', 6000, from);
    const got = inbox.slice(from);
    const positions = got.filter((m) => m.type === 'position');
    expect(positions.length).toBeGreaterThan(20);
    const hits = got.filter((m): m is Extract<EngineMessage, { type: 'hit' }> => m.type === 'hit');
    expect(hits.length).toBeGreaterThan(5);
    for (const h of hits.filter((h) => h.grade !== 'extra')) {
      expect(['onTime', 'early', 'late']).toContain(h.grade);
      expect(h.expectedTick).not.toBeNull();
      expect(Math.abs(h.errorMs!) <= 15).toBe(h.grade === 'onTime');
    }
    expect(summary.loop).toBe(0);
    const total = summary.total;
    expect(total.hits + total.missed).toBe(17);
    expect(Object.keys(summary.instruments).sort()).toEqual(['hihatClosed', 'kick', 'snare']);
  });

  it('records and stops', async () => {
    expect((await command({ type: 'startRecording' })).recording).toBe(true);
    const from = inbox.length;
    send({ type: 'stopRecording' });
    const saved = await waitFor((m): m is Extract<EngineMessage, { type: 'recordingSaved' }> => m.type === 'recordingSaved', 3000, from);
    expect(saved.path).toMatch(/recordings\/.*\.mid$/);
    const st = await command({ type: 'stop' });
    expect(st.playing).toBe(false);
  });

  it('loads another exercise by path', async () => {
    const st = await command({ type: 'loadExercise', path: 'ui/test/fixtures/rock-4-4-16ths.json' });
    expect(st.exercise?.bars).toBe(2);
    expect(st.bpm).toBe(90);
  });
});
