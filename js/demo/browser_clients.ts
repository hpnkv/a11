import {
  ActionPortSchema,
  ActionRegistry,
  ActionSchema,
  HttpSseClientWireStream,
  Session,
  StreamMode,
  isOk,
  type Status,
  type WireMessage,
  type WireStream,
} from '../src/index.js';

const echoSchema = new ActionSchema({
  name: 'echo',
  description: 'Return the supplied text unchanged.',
  inputs: { input: new ActionPortSchema({ name: 'input', type: 'text/plain', required: true }) },
  outputs: { output: new ActionPortSchema({ name: 'output', type: 'text/plain', required: true }) },
});

type Direction = 'sent' | 'received';
interface WireEvent { direction: Direction; at: Date; message: WireMessage; bytes: number }

const need = <T>(value: T | Status): T => {
  if (!isOk(value)) throw new Error(`${value.code}: ${value.message}`);
  return value as T;
};

class ObservedStream implements WireStream {
  constructor(private readonly inner: WireStream, private readonly observe: (event: WireEvent) => void) {}
  getId(): string { return this.inner.getId(); }
  send(message: WireMessage): Status { this.record('sent', message); return this.inner.send(message); }
  start(onMessage?: Parameters<WireStream['start']>[0], onDone?: Parameters<WireStream['start']>[1]): Promise<Status> {
    return this.inner.start(async (message) => { if (message) this.record('received', message); return onMessage?.(message); }, onDone);
  }
  accept(onMessage?: Parameters<WireStream['accept']>[0], onDone?: Parameters<WireStream['accept']>[1]): Promise<Status> { return this.inner.accept(onMessage, onDone); }
  halfClose(headers?: Parameters<WireStream['halfClose']>[0]): Status { return this.inner.halfClose(headers); }
  drainOutgoingMessages(): Promise<Status> { return this.inner.drainOutgoingMessages(); }
  abort(status: Parameters<WireStream['abort']>[0]): Status { return this.inner.abort(status); }
  setDeadline(deadline?: Parameters<WireStream['setDeadline']>[0]): Status { return this.inner.setDeadline(deadline); }
  getDeadline(): number | null { return this.inner.getDeadline(); }
  getStatus(): Status { return this.inner.getStatus(); }
  getTrailers(): ReturnType<WireStream['getTrailers']> { return this.inner.getTrailers(); }
  getImpl(): unknown | null { return this.inner.getImpl(); }
  wait(): Promise<Status> { return this.inner.wait(); }
  private record(direction: Direction, message: WireMessage): void {
    const packed = message.toMsgpack();
    this.observe({ direction, at: new Date(), message, bytes: isOk(packed) ? packed.byteLength : message.approxBytes });
  }
}

class EchoDemo {
  private session: Session | null = null;
  private stream: ObservedStream | null = null;
  private selected: WireEvent | null = null;
  private readonly events: WireEvent[] = [];
  private readonly log = document.querySelector<HTMLDivElement>('#echo-wire-log')!;
  private readonly details = document.querySelector<HTMLDivElement>('#echo-wire-details')!;
  private readonly errors = document.querySelector<HTMLDivElement>('#echo-errors')!;
  private readonly messages = document.querySelector<HTMLDivElement>('#echo-messages')!;
  private readonly server = document.querySelector<HTMLInputElement>('#echo-server')!;

  async connect(): Promise<void> {
    this.errors.textContent = '';
    this.session?.halfClose();
    const registry = new ActionRegistry();
    need(registry.register('echo', echoSchema));
    this.session = need(Session.create({ actionRegistry: registry, noStreamTimeoutMs: null }));
    const base = new URL(this.server.value);
    const prefix = base.pathname.replace(/\/$/, '');
    const transport = need(HttpSseClientWireStream.create(base.origin, {
      connectEndpoint: `${prefix}/connect`,
      messageEndpoint: `${prefix}/streams/{id}/message`,
    }));
    this.stream = new ObservedStream(transport, (event) => this.addEvent(event));
    need(await this.session.addStream(this.stream, StreamMode.START));
  }

  async send(text: string): Promise<void> {
    try {
      if (!this.session || !this.stream) await this.connect();
      const registry = this.session!.getActionRegistry()!;
      const action = need(registry.makeAction('echo', {
        nodeMap: this.session!.getNodeMap(), stream: this.stream!, session: this.session!,
      }));
      need(await action.call());
      const input = need(await action.getInput('input'));
      need(await input.putFinal(text));
      need(await action.waitForDispatch(10_000));
      need(await action.wait(30_000));
      const output = need(await action.getOutput('output', false));
      const reply = need(await output.next({ timeoutMs: 10_000 }));
      this.addBubble(String(reply), 'reply');
    } catch (error) { this.showError(error); }
  }

  async reconnect(): Promise<void> { try { await this.connect(); } catch (error) { this.showError(error); } }
  halfClose(): void { try { if (this.session) need(this.session.halfClose()); } catch (error) { this.showError(error); } }
  private addBubble(text: string, kind: 'request' | 'reply'): void {
    const bubble = document.createElement('div'); bubble.className = `echo-bubble ${kind}`; bubble.textContent = text; this.messages.append(bubble);
  }
  addRequest(text: string): void { this.addBubble(text, 'request'); void this.send(text); }
  private addEvent(event: WireEvent): void { this.events.push(event); this.renderLog(); this.select(event); }
  private renderLog(): void {
    this.log.replaceChildren(...this.events.map((event) => {
      const row = document.createElement('button'); row.type = 'button'; row.className = 'echo-wire-row';
      row.textContent = `${event.direction === 'sent' ? '→' : '←'}  ${event.at.toLocaleTimeString([], { hour12: false })}`;
      row.onclick = () => this.select(event); return row;
    }));
  }
  private select(event: WireEvent): void {
    this.selected = event;
    const actions = event.message.actions.map((action) => action.name || action.id).join(', ') || 'none';
    const nodes = event.message.nodeFragments.map((fragment) => fragment.id).join(', ') || 'none';
    this.details.innerHTML = `<dl><dt>Actions</dt><dd>${escapeHtml(actions)}</dd><dt>Node fragments</dt><dd>${escapeHtml(nodes)}</dd><dt>Encoded size</dt><dd>${event.bytes} bytes</dd></dl>`;
  }
  private showError(error: unknown): void { this.errors.textContent = error instanceof Error ? error.message : String(error); }
}

const escapeHtml = (value: string): string => value.replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' })[c]!);
const root = document.querySelector('#echo-demo');
if (root) {
  const demo = new EchoDemo();
  document.querySelector<HTMLFormElement>('#echo-form')!.onsubmit = (event) => {
    event.preventDefault(); const input = document.querySelector<HTMLInputElement>('#echo-input')!;
    if (input.value.trim()) { demo.addRequest(input.value); input.value = ''; }
  };
  document.querySelector<HTMLButtonElement>('#echo-half-close')!.onclick = () => demo.halfClose();
  document.querySelector<HTMLButtonElement>('#echo-reconnect')!.onclick = () => void demo.reconnect();
}
