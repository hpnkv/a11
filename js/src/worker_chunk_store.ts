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

import type {ChunkStore, Deadline} from './runtime_options.js';
import {type A11Runtime} from './runtime_backend.js';
import {NodeFragment, validateName} from './data.js';
import {invalidArgumentError, isOk, isStatus, statusFromUnknown,
  type Status, type StatusOr} from './status.js';
import {checkedDeadline, failure, fromRaw, toRaw} from './fragment_bridge.js';
import {statusFromBridgeValue} from './status_bridge.js';

export interface WorkerStoreHandle {
  get(sequence: number, options?: {deadlineMs?: number}):
    Promise<Record<string, unknown>>;
  getByArrival(order: number, options?: {deadlineMs?: number}):
    Promise<Record<string, unknown>>;
  next(limit?: number, options?: {deadlineMs?: number}):
    Promise<Array<Record<string, unknown> | null>>;
  putMany(fragments: Array<Record<string, unknown>>): Promise<number[]>;
  clearData(sequence: number): Promise<Record<string, unknown>>;
  arrivalSequence(order: number): Promise<number>;
  finalSequence(): Promise<number | null>;
  size(): Promise<number>;
  close(status: {code: number; message: string}, options?:
    {returnExisting?: boolean}): Promise<{code: number; message: string}>;
}

function deadlineOptions(value: Deadline | undefined):
    StatusOr<{deadlineMs?: number}> {
  const checked = checkedDeadline(value);
  if (!isOk(checked)) return checked;
  if (checked === null) return {};
  const epoch = checked instanceof Date ? checked.getTime() : checked;
  if (!Number.isSafeInteger(Math.ceil(epoch))) {
    return invalidArgumentError('Deadline is outside the supported epoch range.');
  }
  return {deadlineMs: Math.ceil(epoch)};
}

/** The public LocalChunkStore API over the native C++ ChunkStore instance. */
export class WorkerChunkStore implements ChunkStore {
  private constructor(id: string, private readonly native: WorkerStoreHandle) {
    this.id = id;
  }

  private readonly id: string;

  getId(): StatusOr<string> { return this.id; }

  static create(id: string,
      options?: {runtime: A11Runtime}): StatusOr<WorkerChunkStore> {
    if (options?.runtime === undefined) {
      return invalidArgumentError(
        'LocalChunkStore.create requires an initialized runtime.');
    }
    return options.runtime.createChunkStore(id);
  }

  static fromHandle(id: string, native: WorkerStoreHandle):
      StatusOr<WorkerChunkStore> {
    const status = validateName(id);
    return isOk(status) ? new WorkerChunkStore(id, native) : status;
  }

  async get(sequence: number, deadline?: Deadline):
      Promise<StatusOr<NodeFragment>> {
    const options = deadlineOptions(deadline);
    if (!isOk(options)) return options;
    try { return fromRaw(await this.native.get(sequence, options)); }
    catch (error) { return failure(error, 'Native store read failed.'); }
  }

  async getByArrivalOrder(order: number, deadline?: Deadline):
      Promise<StatusOr<NodeFragment>> {
    const options = deadlineOptions(deadline);
    if (!isOk(options)) return options;
    try { return fromRaw(await this.native.getByArrival(order, options)); }
    catch (error) { return failure(error, 'Native arrival read failed.'); }
  }

  async next(deadline?: Deadline, limit = 1):
      Promise<StatusOr<Array<NodeFragment | null>>> {
    const options = deadlineOptions(deadline);
    if (!isOk(options)) return options;
    try {
      const raw = await this.native.next(limit, options);
      const result: Array<NodeFragment | null> = [];
      for (const item of raw) {
        if (item === null) result.push(null);
        else {
          const fragment = fromRaw(item);
          if (!isOk(fragment)) return fragment;
          result.push(fragment);
        }
      }
      return result;
    } catch (error) {
      return failure(error, 'Native store cursor failed.');
    }
  }

  async put(fragment: NodeFragment): Promise<StatusOr<number>> {
    const values = await this.putMany([fragment]);
    return isOk(values) ? values[0]! : values;
  }

  async putMany(fragments: readonly NodeFragment[]):
      Promise<StatusOr<number[]>> {
    const raw: Array<Record<string, unknown>> = [];
    for (const fragment of fragments) {
      const value = toRaw(fragment);
      if (!isOk(value)) return value;
      if (value.id !== this.getId()) {
        return invalidArgumentError('Fragment belongs to another node.');
      }
      raw.push(value);
    }
    try { return await this.native.putMany(raw); }
    catch (error) { return failure(error, 'Native store write failed.'); }
  }

  async clearData(sequence: number): Promise<StatusOr<NodeFragment>> {
    try { return fromRaw(await this.native.clearData(sequence)); }
    catch (error) { return failure(error, 'Native store clear failed.'); }
  }

  async getSeqForArrivalOrder(order: number): Promise<StatusOr<number>> {
    try { return await this.native.arrivalSequence(order); }
    catch (error) { return failure(error, 'Native arrival lookup failed.'); }
  }

  async getFinalSeq(): Promise<StatusOr<number | null>> {
    try { return await this.native.finalSequence(); }
    catch (error) { return failure(error, 'Native final sequence lookup failed.'); }
  }

  async closeWritesWithStatus(status: Status,
      returnStatusIfAlreadyClosed = false): Promise<Status> {
    if (!isStatus(status)) return invalidArgumentError('status must be an A11 Status.');
    try {
      return statusFromBridgeValue(await this.native.close(status,
        {returnExisting: returnStatusIfAlreadyClosed}),
      'Native store close failed.');
    } catch (error) {
      return failure(error, 'Native store close failed.');
    }
  }

  async size(): Promise<StatusOr<number>> {
    try { return await this.native.size(); }
    catch (error) { return statusFromUnknown(error, 'Native store size failed.'); }
  }
}
