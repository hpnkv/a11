import {createA11Runtime, type A11Runtime, isOk}
  from '../src/index.js';

const activeRuntimes = new Set<A11Runtime>();
let isolationPromise: Promise<boolean> | null = null;

async function isolateGuidePageOnce(): Promise<boolean> {
  if (typeof window === 'undefined' || window.crossOriginIsolated ||
      !('serviceWorker' in navigator)) return true;
  const marker = 'a11-wasm-isolation-reloaded';
  if (sessionStorage.getItem(marker) === '1') return true;
  const script = new URL('../wasm-isolation-sw.js', location.href);
  await navigator.serviceWorker.register(script, {
    scope: new URL('../', location.href).pathname,
  });
  await navigator.serviceWorker.ready;
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
