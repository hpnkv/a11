/**
 * The Flow language, as a language server this talks to.
 *
 * `a11-flow serve --protocol lsp` answers diagnostics with their quick fixes,
 * semantic tokens, formatting, completion, hover, document symbols and
 * go-to-declaration. All of that arrives here with no language knowledge on this
 * side: what follows is a client, and the one thing it has to get right is the
 * arithmetic offsets are counted in.
 *
 * **Which arithmetic.** The language reads bytes; a JavaScript string is indexed
 * in UTF-16 code units. For ASCII the two agree, which makes this the easiest
 * thing in the whole protocol to get wrong — it works on every example file and
 * then colours the first document with prose in it a column to the left of
 * itself. The service converts, and `a11flow/setOffsets`-style negotiation is not
 * needed over LSP because the protocol already specifies UTF-16; the server is
 * told once at initialisation which it is being asked for.
 */

import * as vscode from 'vscode';
import {
  LanguageClient,
  State,
  type LanguageClientOptions,
  type ServerOptions,
  TransportKind,
} from 'vscode-languageclient/node';

/** The language id this extension contributes, and the scheme documents come on. */
export const FLOW_LANGUAGE = 'a11flow';

export class FlowServer {
  private client: LanguageClient | undefined;

  /**
   * Where the client writes what it saw.
   *
   * Passed in rather than made here, and owned by the caller: a server is replaced
   * on every restart, and a channel per server would leak one into the Output
   * dropdown each time. It also has to outlive a *failed* start, since the reason
   * the start failed is written to it.
   */
  constructor(
    private readonly executable: string,
    private readonly log: vscode.OutputChannel,
  ) {}

  /** Show what the client and the server have said. */
  showLog(): void {
    this.log.show(true);
  }

  /** Start the server and attach it to `.flow` documents. */
  async start(): Promise<void> {
    const server: ServerOptions = {
      run: {
        command: this.executable,
        args: ['serve', '--protocol', 'lsp'],
        transport: TransportKind.stdio,
      },
      debug: {
        command: this.executable,
        args: ['serve', '--protocol', 'lsp'],
        transport: TransportKind.stdio,
      },
    };

    const client: LanguageClientOptions = {
      documentSelector: [
        {scheme: 'file', language: FLOW_LANGUAGE},
        {scheme: 'untitled', language: FLOW_LANGUAGE},
      ],
      // The protocol's own answer to the offset question. Saying it explicitly
      // rather than relying on the default is the point: this is the field that
      // decides whether a document with a `§` in it is coloured in the right
      // place, and a default is a thing that changes.
      initializationOptions: {offsets: 'utf16'},
      synchronize: {
        fileEvents: vscode.workspace.createFileSystemWatcher('**/*.flow'),
      },
      outputChannel: this.log,
    };

    this.client = new LanguageClient(
      'a11flow',
      'A11 Flow',
      server,
      client,
    );
    await this.client.start();
  }

  /**
   * Stop the server, whatever state it is in.
   *
   * `LanguageClient.stop()` *throws* for a client that is not `Running` — "Client
   * is not running and can't be stopped. It's current state is: startFailed", and
   * the same for `starting`, which is the state during a handshake that is about to
   * fail. Both are exactly the states a caller wants to stop from, so a restart
   * that called it unconditionally turned one failure into a second, more confusing
   * one. Only a client that is *up* gets stopped; anything else is disposed and
   * forgotten.
   *
   * Never throws. A caller stopping a server is on its way somewhere else, and a
   * failure to close something already closed is not news.
   */
  async stop(): Promise<void> {
    const client = this.client;
    this.client = undefined;
    if (!client) return;
    try {
      if (client.state === State.Running) {
        await client.stop();
      } else {
        // Including `Starting`: a client mid-handshake refuses to be stopped just
        // as a failed one does.
        await client.dispose();
      }
    } catch {
      // Already gone, or gone in a way that cannot be undone. Either way there is
      // nothing left for this call to achieve.
    }
  }

  get running(): boolean {
    return this.client?.state === State.Running;
  }

  /** What went wrong, for a caller that wants to say so. */
  get state(): string {
    switch (this.client?.state) {
      case State.Running:
        return 'running';
      case State.Starting:
        return 'starting';
      case State.Stopped:
        return 'stopped';
      default:
        return 'not started';
    }
  }

  /**
   * Tell the server what the world outside these documents contains.
   *
   * Said once per change rather than on every keystroke, which is what makes it a
   * notification: a catalogue of a hundred actions has no business travelling
   * with each completion request.
   */
  async setContext(catalogue: unknown): Promise<void> {
    await this.client?.sendNotification('a11flow/setContext', catalogue);
  }

  /**
   * Read `paths` for the actions they declare, and fold the result into what the
   * server knows.
   *
   * A request rather than a notification because the answer is worth having: how
   * many actions were found, and whether a cap stopped the walk before the tree
   * ran out.
   */
  async scan(paths: readonly string[]): Promise<ScanResult | undefined> {
    if (!this.client) return undefined;
    return (await this.client.sendRequest('a11flow/scan', {paths})) as ScanResult;
  }

  /**
   * One request to the *JSON* half of the same service, for a question the LSP
   * has no method for.
   *
   * Used by the fragment checker: a flow inside a Python string is not a document
   * the server has, so it is asked about as text. Everything about `.flow` files
   * goes through the client above.
   */
  async request(payload: Record<string, unknown>): Promise<unknown> {
    if (!this.client) return undefined;
    return this.client.sendRequest('a11flow/relay', payload);
  }
}

/** What `a11flow/scan` answers. */
export interface ScanResult {
  actions: number;
  files_read: number;
  reached_file_limit: boolean;
  too_large: string[];
}
