/**
 * Which surface to put in the page, once a host has been installed.
 *
 * The last thing every host's entry point calls. Kept here rather than in each
 * host so that "there are two views and this is how you choose between them" is
 * stated once: a host that grew a third view by writing its own `if` would be a
 * host whose views drifted from the other's.
 */

import { mountActions } from './actions.js';
import { mountChat } from './chat.js';

/** The surfaces this UI has. */
export type View = 'chat' | 'actions';

/**
 * What a host can ask of a surface after it is mounted.
 *
 * The page owns its own affordances — the chat has a "+ New chat" button — and an
 * editor may want the same thing as a command in its palette or its menus. This is
 * how it gets it *without* a second implementation: one action, reachable from the
 * button and from the host.
 *
 * Everything is optional, because not every surface has every action, and a host
 * that asks for one a surface does not have should do nothing rather than fail.
 */
export interface MountedView {
  /** Start a fresh conversation. The chat has this; the explorer does not. */
  newChat?: () => void;
}

/** The view named, or the chat, which is what an unset host means. */
export function viewOf(name: string | undefined): View {
  return name === 'actions' ? 'actions' : 'chat';
}

/** Put `view` into `root`, and hand back what the host may then ask of it. */
export function mount(root: HTMLElement, view: View): MountedView {
  return view === 'actions' ? mountActions(root) : mountChat(root);
}

/**
 * Show why nothing could be mounted.
 *
 * A host that cannot reach its editor is a real state — a JCEF query that never
 * initialised, a webview whose extension host went away — and a blank panel is
 * the one response that tells nobody anything.
 */
export function mountFailure(root: HTMLElement, error: unknown): void {
  root.innerHTML = '';
  const message = document.createElement('p');
  message.className = 'error';
  message.style.padding = '16px';
  message.textContent = error instanceof Error ? error.message : String(error);
  root.append(message);
}
