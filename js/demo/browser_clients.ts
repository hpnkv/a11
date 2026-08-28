import {
  ActionPortSchema,
  ActionRegistry,
  ActionSchema,
  HttpSseClientWireStream,
  Session,
  StatusCode,
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
  if (!isOk(value)) throw new Error(`${StatusCode[value.code]}: ${value.message}`);
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
    const transport = need(HttpSseClientWireStream.create(base.origin, {
      connectEndpoint: base.pathname.replace(/\/$/, ''),
      messageEndpoint: '/streams/{id}/message',
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
      need(await input.finalize(text));
      need(await action.waitForDispatch(10_000));
      const output = need(await action.getOutput('output', false));
      const reply = need(await output.next({ timeoutMs: 10_000 }));
      need(await action.wait(30_000));
      this.addBubble(String(reply), 'reply');
      window.dispatchEvent(
        new CustomEvent('a11:example-succeeded', {detail: {example: 'echo'}}),
      );
    } catch (error) { this.showError(error); }
  }

  async reconnect(): Promise<void> { try { await this.connect(); } catch (error) { this.showError(error); } }
  halfClose(): void { try { if (this.session) need(this.session.halfClose()); } catch (error) { this.showError(error); } }
  private addBubble(text: string, kind: 'request' | 'reply'): void {
    const bubble = document.createElement('div'); bubble.className = `echo-bubble ${kind}`; bubble.textContent = text; this.messages.append(bubble);
    this.messages.scrollTop = this.messages.scrollHeight;
  }
  addRequest(text: string): void { this.addBubble(text, 'request'); void this.send(text); }
  private addEvent(event: WireEvent): void { this.events.push(event); this.renderLog(); this.select(event); }
  private renderLog(): void {
    this.log.replaceChildren(...this.events.map((event) => {
      const row = document.createElement('button'); row.type = 'button'; row.className = 'echo-wire-row';
      const arrow = document.createElement('span');
      arrow.className = `echo-wire-arrow ${event.direction}`;
      arrow.textContent = event.direction === 'sent' ? '→' : '←';
      arrow.setAttribute('aria-label', event.direction);
      const timestamp = document.createElement('time');
      timestamp.className = 'echo-wire-time';
      timestamp.dateTime = event.at.toISOString();
      timestamp.textContent = event.at.toLocaleTimeString([], { hour12: false });
      const summary = document.createElement('span');
      summary.className = 'echo-wire-summary';
      summary.textContent = messageSummary(event.message);
      row.append(arrow, timestamp, summary);
      row.onclick = () => this.select(event); return row;
    }));
    this.log.scrollTop = this.log.scrollHeight;
  }
  private select(event: WireEvent): void {
    const actions = event.message.actions.map((action) => action.name || action.id).join(', ') || 'none';
    const list = document.createElement('dl');
    const actionsTerm = document.createElement('dt'); actionsTerm.textContent = 'Actions';
    const actionsValue = document.createElement('dd'); actionsValue.textContent = actions;
    const nodesTerm = document.createElement('dt'); nodesTerm.textContent = 'Node fragments';
    const nodesValue = document.createElement('dd');
    if (event.message.nodeFragments.length === 0) {
      nodesValue.textContent = 'none';
    } else {
      for (const [index, fragment] of event.message.nodeFragments.entries()) {
        if (index > 0) nodesValue.append(document.createTextNode(', '));
        const name = document.createElement('span');
        name.className = 'echo-fragment'; name.tabIndex = 0; name.textContent = fragment.id;
        name.dataset.preview = fragmentPreview(fragment);
        nodesValue.append(name);
      }
    }
    const sizeTerm = document.createElement('dt'); sizeTerm.textContent = 'Encoded size';
    const sizeValue = document.createElement('dd'); sizeValue.textContent = `${event.bytes} bytes`;
    list.append(actionsTerm, actionsValue, nodesTerm, nodesValue, sizeTerm, sizeValue);
    this.details.replaceChildren(list);
  }
  private showError(error: unknown): void { this.errors.textContent = error instanceof Error ? error.message : String(error); }
}

const counted = (count: number, singular: string, plural: string): string =>
  `${count} ${count === 1 ? singular : plural}`;

const messageSummary = (message: WireMessage): string => {
  const parts: string[] = [];
  if (message.actions.length > 0) parts.push(counted(message.actions.length, 'action call', 'action calls'));
  if (message.nodeFragments.length > 0) parts.push(counted(message.nodeFragments.length, 'node fragment', 'node fragments'));
  return parts.join(', ') || 'control message';
};

const fragmentPreview = (fragment: WireMessage['nodeFragments'][number]): string => {
  const result = fragment.getChunk();
  if (!isOk(result)) {
    const reference = fragment.getNodeRef();
    return isOk(reference)
      ? `Node reference: ${reference.id}\nOffset: ${reference.offset}\nLength: ${reference.length ?? 'to end'}`
      : 'Fragment payload is unavailable.';
  }
  const mimetype = result.mimetype || '(not set)';
  const metadata = result.metadata;
  const attributes = metadata === null || metadata.attributes.size === 0
    ? 'none'
    : [...metadata.attributes].map(([name, value]) => `${name} (${value.byteLength} bytes)`).join(', ');
  const bytes = result.data.subarray(0, 100);
  const value = result.mimetype.startsWith('text/')
    ? new TextDecoder().decode(bytes)
    : [...bytes].map((byte) => `\\x${byte.toString(16).padStart(2, '0')}`).join('');
  const truncated = result.data.byteLength > 100 ? `... (${result.data.byteLength - 100} more bytes)` : '';
  const timestamp = metadata?.timestamp?.toISOString() ?? 'none';
  const reference = result.ref === '' ? '' : `\nChunk reference: ${result.ref}`;
  return `MIME type: ${mimetype}\nTimestamp: ${timestamp}\nAttributes: ${attributes}${reference}\nData: ${value}${truncated}`;
};
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
