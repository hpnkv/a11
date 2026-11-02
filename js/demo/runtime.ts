/** One initialized A11 runtime for the browser guide examples. */
import type {A11Runtime} from '../src/index.js';
import {createDemoRuntime} from './wasm_runtime.js';

let runtimePromise: Promise<A11Runtime> | null = null;

export function demoRuntime(): Promise<A11Runtime> {
  runtimePromise ??= createDemoRuntime().catch(error => {
    runtimePromise = null;
    throw error;
  });
  return runtimePromise;
}
