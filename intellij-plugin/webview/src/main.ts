/**
 * The JetBrains host: this bundle's entry point, and the only file here that
 * knows it is running inside JCEF.
 *
 * Everything the page actually shows is `../../webview`, which is shared with the
 * VSCode extension. What is host-specific is the bridge — `A11WebView.kt` defines
 * `window.__a11Bridge`, each of whose functions is backed by a `JBCefJsQuery`
 * that runs the real IDE work and answers with a JSON string — and the fact that
 * it may not be there for a moment yet. So this waits for it, installs it, and
 * mounts whichever view the Kotlin bootstrap asked for.
 *
 * There is no adapter to write: the JetBrains bridge *is* the shape the shared UI
 * asks for, which is why that shape is the one it asks for.
 */

import { setHost, type HostBridge } from '../../../webview/src/bridge.js';
import { mount, mountFailure, viewOf } from '../../../webview/src/mount.js';

/**
 * Wait for the Kotlin side to define the bridge.
 *
 * Normally it is already there: the Kotlin code injects this bundle immediately
 * after defining it. The wait is for the case where it is not, and the timeout is
 * so that a page which will never get one says so rather than sitting blank.
 */
function waitForBridge(timeoutMs = 8_000): Promise<HostBridge> {
  return new Promise((resolve, reject) => {
    if (window.__a11Bridge) return resolve(window.__a11Bridge);
    const started = Date.now();
    const timer = setInterval(() => {
      if (window.__a11Bridge) {
        clearInterval(timer);
        resolve(window.__a11Bridge);
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
    setHost(await waitForBridge());
  } catch (error) {
    mountFailure(root, error);
    return;
  }
  // The handle is unused here for now: the JetBrains side drives a new chat from
  // the page's own button. It is returned all the same, so an action added to the
  // plugin has the same way in the VSCode command has.
  mount(root, viewOf(window.__A11_VIEW));
}

if (document.readyState === 'loading') {
  document.addEventListener('DOMContentLoaded', () => void main());
} else {
  void main();
}
