/**
 * Entry point for the JCEF webview. The Kotlin bootstrap sets `window.__A11_VIEW`
 * to select which surface to mount into `#app`: the chat window or the action
 * explorer. Both share this one bundle.
 *
 * The Kotlin side injects this bundle right after it defines `window.__a11Bridge`
 * (see A11WebView.kt), so the bridge is normally present already; we still wait a
 * short moment defensively, and surface a clear message if it never appears.
 */

import { mountActions } from './actions.js';
import { mountChat } from './chat.js';

function waitForBridge(timeoutMs = 8_000): Promise<void> {
  return new Promise((resolve, reject) => {
    if (window.__a11Bridge) return resolve();
    const started = Date.now();
    const timer = setInterval(() => {
      if (window.__a11Bridge) {
        clearInterval(timer);
        resolve();
      } else if (Date.now() - started > timeoutMs) {
        clearInterval(timer);
        reject(new Error('The A11 Kotlin bridge did not initialize.'));
      }
    }, 25);
  });
}

async function main(): Promise<void> {
  const root = document.getElementById('app');
  if (!root) return;
  try {
    await waitForBridge();
  } catch (error) {
    root.innerHTML = '';
    const message = document.createElement('p');
    message.className = 'error';
    message.style.padding = '16px';
    message.textContent = error instanceof Error ? error.message : String(error);
    root.append(message);
    return;
  }
  if ((window.__A11_VIEW ?? 'chat') === 'actions') {
    mountActions(root);
  } else {
    mountChat(root);
  }
}

if (document.readyState === 'loading') {
  document.addEventListener('DOMContentLoaded', () => void main());
} else {
  void main();
}
