import {createA11Runtime, type A11Runtime, isOk}
  from '../src/index.js';

const activeRuntimes = new Set<A11Runtime>();
let isolationPromise: Promise<boolean> | null = null;

async function isolateGuidePageOnce(): Promise<boolean> {
  if (typeof window === 'undefined') return true;
  if (new URLSearchParams(location.search).has('blog-scene')) return true;
  if (!('serviceWorker' in navigator)) {
    if (!window.crossOriginIsolated) {
      throw new Error('The browser cannot enable cross-origin isolation.');
    }
    return true;
  }
  const legacyScope = new URL('/guides/', location.href).href;
  const registrations = await navigator.serviceWorker.getRegistrations();
  const legacy = registrations.filter(registration =>
    registration.scope === legacyScope &&
    registration.active?.scriptURL.endsWith('/wasm-isolation-sw.js'));
  const script = new URL('/wasm-isolation-sw.js', location.href);
  const controller = navigator.serviceWorker.controller?.scriptURL;
  if (window.crossOriginIsolated && legacy.length === 0 &&
      (!controller || controller === script.href)) return true;
  const marker = 'a11-wasm-isolation-root-reloaded';
  if (sessionStorage.getItem(marker) === '1') {
    throw new Error('The guide page could not enable cross-origin isolation.');
  }
  await Promise.all(legacy.map(registration => registration.unregister()));
  const registration = await navigator.serviceWorker.register(script, {
    scope: '/',
  });
  const worker = registration.installing ?? registration.waiting ??
    registration.active;
  if (worker && worker.state !== 'activated') {
    await new Promise<void>(resolve => {
      worker.addEventListener('statechange', () => {
        if (worker.state === 'activated') resolve();
      });
    });
  }
  sessionStorage.setItem(marker, '1');
  location.reload();
  return false;
}

function isolateGuidePage(): Promise<boolean> {
  isolationPromise ??= isolateGuidePageOnce();
  return isolationPromise;
}

if (typeof window !== 'undefined') void isolateGuidePage().catch(() => {});

/** Resolve packaged assets beside browser bundles or the Node test bundle. */
export async function createDemoRuntime(): Promise<A11Runtime> {
  if (!await isolateGuidePage()) {
    return new Promise<A11Runtime>(() => {});
  }
  const assetsBaseUrl = typeof window === 'undefined'
    ? new URL('../../../dist/wasm/', import.meta.url)
    : new URLSearchParams(location.search).has('blog-scene')
      ? new URL('/a11-wasm-flow-14/', location.href)
      : new URL('./wasm/', import.meta.url);
  const runtime = await createA11Runtime({wasm: {assetsBaseUrl}});
  if (!isOk(runtime)) throw new Error(`A11 runtime: ${runtime.message}`);
  activeRuntimes.add(runtime);
  return runtime;
}

/** Release browser guide runtimes when a Node integration run ends. */
export async function disposeDemoRuntimes(): Promise<void> {
  await Promise.all([...activeRuntimes].map(runtime => runtime.dispose()));
  activeRuntimes.clear();
}
