// WebSocket client for the engine with automatic reconnect.
import type { Command, EngineMessage } from '../../shared/protocol';

export type ConnStatus = 'connecting' | 'open' | 'closed';

export class Connection {
  private ws: WebSocket | null = null;
  private attempt = 0;
  private timer: number | null = null;
  status: ConnStatus = 'connecting';

  constructor(
    private readonly url: string,
    private readonly onMessage: (m: EngineMessage) => void,
    private readonly onStatus: (s: ConnStatus) => void,
  ) {}

  start() {
    this.connect();
  }

  send(cmd: Command): boolean {
    if (!this.ws || this.ws.readyState !== WebSocket.OPEN) return false;
    this.ws.send(JSON.stringify(cmd));
    return true;
  }

  private setStatus(s: ConnStatus) {
    if (this.status === s) return;
    this.status = s;
    this.onStatus(s);
  }

  private connect() {
    this.timer = null;
    this.setStatus(this.attempt === 0 ? 'connecting' : 'closed');
    let ws: WebSocket;
    try {
      ws = new WebSocket(this.url);
    } catch {
      this.scheduleReconnect();
      return;
    }
    this.ws = ws;
    ws.onopen = () => {
      this.attempt = 0;
      this.setStatus('open');
    };
    ws.onmessage = (ev) => {
      let msg: EngineMessage;
      try {
        msg = JSON.parse(String(ev.data));
      } catch {
        console.warn('engine sent invalid JSON', ev.data);
        return;
      }
      if (msg && typeof msg === 'object' && 'type' in msg) this.onMessage(msg);
    };
    ws.onclose = () => {
      if (this.ws !== ws) return;
      this.ws = null;
      this.setStatus('closed');
      this.scheduleReconnect();
    };
    ws.onerror = () => {
      /* onclose follows */
    };
  }

  private scheduleReconnect() {
    if (this.timer !== null) return;
    this.attempt++;
    const delay = Math.min(2000, 250 * 2 ** Math.min(this.attempt, 4));
    this.timer = window.setTimeout(() => this.connect(), delay);
  }
}
