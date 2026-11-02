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

export function createNativeSession(command, action, attachStream,
    id = globalThis.crypto?.randomUUID?.(), options = {},
    existingAction) {
  if (typeof id !== 'string' || !id) {
    throw new TypeError('session id is required');
  }
  return Object.freeze({
    id,
    ready: () => command({operation: 'session_create', id, options,
      withRecv: options.withRecv ?? false,
      withCallbacks: options.withCallbacks ?? false,
      ...(options.headers === undefined ? {} : {headers: options.headers})}),
    state: options => command({operation: 'session_state', id}, options),
    setDeadline: deadlineMs => command({operation: 'session_set_deadline',
      id, deadlineMs}),
    receiveWithStreamId: (options = {}) => command({
      operation: 'session_receive', id,
      ...(options.timeoutMs === undefined ? {} : {timeoutMs: options.timeoutMs}),
    }, options),
    done: (options = {}) => command({operation: 'session_done', id,
      ...(options.timeoutMs === undefined ? {} : {timeoutMs: options.timeoutMs})},
    options),
    halfClose: options => command({operation: 'session_half_close', id},
      options),
    cancelActions: options => command({operation: 'session_cancel_actions', id},
      options),
    cancelAction: (actionId, options) => command({
      operation: 'session_cancel_action', id, actionId}, options),
    actions: async () => (await command({operation: 'session_actions', id}))
      .map(found => ({id: found.id,
        action: existingAction(found.name, found.id)})),
    existingAction: async actionId => {
      const found = await command({operation: 'session_action', id, actionId});
      return existingAction(found.name, found.id);
    },
    awaitAllActions: (options = {}) => command({
      operation: 'session_await_actions', id,
      ...(options.timeoutMs === undefined ? {} :
        {timeoutMs: options.timeoutMs})}, options),
    abort: (status, options) => command({operation: 'session_abort', id,
      ...(status === undefined ? {} : {status})}, options),
    send: (message, options = {}) => command({operation: 'session_send', id,
      message, ...(options.streamId === undefined ? {} :
        {streamId: options.streamId})}, options),
    dispatchWireMessage: (message, options = {}) => command({
      operation: 'session_dispatch_wire_message', id, message,
      ...(options.streamId === undefined ? {} :
        {streamId: options.streamId})}, options),
    addStream: (stream, mode = 'start') => attachStream(id, stream, mode),
    action: (name, actionId, streamId, settings, schema) =>
      action(name, actionId, id, streamId, settings, schema),
  });
}
