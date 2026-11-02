// Copyright 2026 The A11 Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

import {StatusCode} from './status.mjs';

export function createNativeStore(command, id, scopeId) {
  if (typeof id !== 'string' || !id) throw new TypeError('store id is required');
  const call = (operation, extra = {}, options = {}) =>
    command({operation, id, ...(scopeId ? {scopeId} : {}), ...extra}, options);
  const wait = options => ({
    ...(options.timeoutMs === undefined ? {} : {timeoutMs: options.timeoutMs}),
    ...(options.deadlineMs === undefined ? {} : {deadlineMs: options.deadlineMs}),
  });
  return Object.freeze({
    id,
    ready: () => call('store_size'),
    get: (sequence, options = {}) => call('store_get',
      {sequence, ...wait(options)}, options),
    getByArrival: (order, options = {}) => call('store_get_by_arrival',
      {order, ...wait(options)}, options),
    next: (limit = 1, options = {}) => call('store_next',
      {limit, ...wait(options)}, options),
    putMany: (fragments, options = {}) => call('store_put_many',
      {fragments}, options),
    clearData: (sequence, options = {}) => call('store_clear_data',
      {sequence}, options),
    arrivalSequence: (order, options = {}) => call('store_arrival_sequence',
      {order}, options),
    finalSequence: options => call('store_final_sequence', {}, options),
    size: options => call('store_size', {}, options),
    close: (status = {code: StatusCode.OK, message: ''}, options = {}) =>
      call('store_close', {...status,
        returnExisting: options.returnExisting ?? false}, options),
  });
}
