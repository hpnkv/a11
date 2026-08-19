/**
 * The VSCode host: this bundle's entry point, and the only file here that knows it
 * is running inside a webview.
 *
 * Everything the panel shows is `../../../webview`, shared with the JetBrains
 * plugin. What is host-specific is the channel. A `JBCefJsQuery` is
 * request/response and needs no adapter at all; a webview has two one-way message
 * streams, so a call and its answer have to be paired up. That pairing is the whole
 * of this file, and it is why there are two small host files rather than one with a
 * flag in it — the shape of the channel really is different, and a shared adapter
 * would be a shared abstraction over one case each.
 */

import {setHost, type HostBridge, type HighlightNote} from '../../../webview/src/bridge.js';
import {mount, mountFailure, viewOf, type MountedView} from '../../../webview/src/mount.js';

interface VsCodeApi {
  postMessage(message: unknown): void;
  getState(): unknown;
  setState(state: unknown): void;
}

declare function acquireVsCodeApi(): VsCodeApi;

interface Answer {
  id: number;
  ok: boolean;
  value?: string;
  error?: string;
}

/**
 * A message from the extension that is not an answer: a command of the editor's
 * asking the page to do something the page owns.
 *
 * The other direction of the channel, and the reason the listener below has to
 * tell the two apart. An answer carries an `id`; this carries a `command`.
 */
interface Push {
  command: 'newChat';
}

/**
 * A bridge over `postMessage`, pairing each call with its answer by id.
 *
 * No timeout: the other end is the extension host, and a call that never comes
 * back means it has gone — at which point the panel is being disposed anyway and a
 * rejected promise would be noise on the way out.
 */
function makeBridge(api: VsCodeApi, mounted: () => MountedView | undefined): HostBridge {
  const waiting = new Map<number, {resolve: (value: string) => void; reject: (error: Error) => void}>();
  let next = 1;

  window.addEventListener('message', (event: MessageEvent<Answer | Push>) => {
    const message = event.data;
    if (message && 'command' in message) {
      // A command of the editor's, driving something the page owns. The same
      // action its own button runs, so the two cannot drift.
      if (message.command === 'newChat') mounted()?.newChat?.();
      return;
    }
    const answer = message as Answer;
    if (typeof answer?.id !== 'number') return;
    const held = waiting.get(answer.id);
    if (!held) return;
    waiting.delete(answer.id);
    if (answer.ok) {
      held.resolve(answer.value ?? '');
    } else {
      held.reject(new Error(answer.error ?? 'The A11 extension refused the call.'));
    }
  });

  const call = (method: string, ...args: unknown[]): Promise<string> =>
    new Promise<string>((resolve, reject) => {
      const id = next++;
      waiting.set(id, {resolve, reject});
      api.postMessage({id, method, args});
    });

  return {
    listActions: () => call('listActions'),
    runAction: (name: string, inputs: unknown) => call('runAction', name, inputs),
    getConfig: () => call('getConfig'),
    readFlow: (name: string) => call('readFlow', name),
    suggestOnHighlight: (note: HighlightNote) => call('suggestOnHighlight', note),
    clearSuggestions: (path: string) => call('clearSuggestions', path),
  };
}

function main(): void {
  const root = document.getElementById('app');
  if (!root) return;
  // The handle is read through a closure rather than passed in, because the
  // bridge has to exist before anything is mounted and the mounted view has to
  // exist before a command can reach it.
  let mounted: MountedView | undefined;
  try {
    setHost(makeBridge(acquireVsCodeApi(), () => mounted));
  } catch (error) {
    mountFailure(root, error);
    return;
  }
  mounted = mount(root, viewOf(window.__A11_VIEW));
}

if (document.readyState === 'loading') {
  document.addEventListener('DOMContentLoaded', main);
} else {
  main();
}
