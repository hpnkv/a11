/**
 * Copyright 2026 The A11 Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

import {unimplementedError, type StatusOr} from '../../status.js';
import {statusFromBridgeFailure} from './status_bridge.js';

/** The worker owns Flow state; this interface is only its transport boundary. */
export interface FlowRunnerBridge {
  runFlow(request: Record<string, unknown>, options?: {signal?: AbortSignal;
    onOutput?: (port: string, chunk: Record<string, unknown>) => void}):
    Promise<Record<string, unknown>>;
  dispose(): void | Promise<void>;
  store(id: string): unknown;
}

let toolingService: {flow(request: Record<string, unknown>):
  Record<string, unknown>} | null = null;

export function installFlowToolingService(service: {flow(request:
    Record<string, unknown>): Record<string, unknown>}): void {
  toolingService = service;
}

export function currentFlowToolingService(): typeof toolingService {
  return toolingService;
}

/** Native Flow language tooling is a separate, synchronous WASM module. */
export function flowToolBridge(
    service: {flow(request: Record<string, unknown>): Record<string, unknown>},
    request: Record<string, unknown>): StatusOr<Record<string, unknown>> {
  try { return service.flow(request); }
  catch (error) { return statusFromBridgeFailure(error, 'WASM Flow tooling failed.'); }
}

export async function runFlowBridge(runner: FlowRunnerBridge | null,
    request: Record<string, unknown>, options: {signal?: AbortSignal;
      onOutput?: (port: string, chunk: Record<string, unknown>) => void} = {}):
    Promise<StatusOr<Record<string, unknown>>> {
  if (runner === null) return unimplementedError('Native Flow is not initialized.');
  try { return await runner.runFlow(request, options); }
  catch (error) { return statusFromBridgeFailure(error, 'WASM Flow execution failed.'); }
}
