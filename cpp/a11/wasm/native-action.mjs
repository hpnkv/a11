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

export function createNativeAction(command, node, name,
    id = globalThis.crypto?.randomUUID?.(), sessionId, streamId, onCancel,
    existing = false, initialSettings, initialSchema) {
  if (typeof name !== 'string' || !name) {
    throw new TypeError('action name is required');
  }
  if (typeof id !== 'string' || !id) {
    throw new TypeError('action id is required');
  }
  const portNode = async (port, direction, bindStream) => {
    const result = await command({operation: 'action_port', id,
      name: port, direction,
      ...(bindStream === undefined ? {} : {bindStream})});
    return node(result.id);
  };
  return Object.freeze({
    id, name,
    ready: () => existing ? Promise.resolve({id}) :
      command({operation: 'action_create', id, name,
        ...(sessionId === undefined ? {} : {sessionId}),
        ...(streamId === undefined ? {} : {streamId}),
        ...(initialSettings === undefined ? {} : {settings: initialSettings}),
        ...(initialSchema === undefined ? {} : {schema: initialSchema})}),
    input: (port, bindStream) => portNode(port, 'input', bindStream),
    output: (port, bindStream) => portNode(port, 'output', bindStream),
    port: port => portNode(port, 'either'),
    containsPort: name => command({operation: 'action_contains_port', id,
      name}),
    logNode: async () => {
      const result = await command({operation: 'action_log_node', id});
      return node(result.id);
    },
    log: chunk => command({operation: 'action_log', id, chunk}),
    sessionId: () => command({operation: 'action_session_id', id}),
    streamSourcesNode: async requestPayload => {
      const result = await command({operation: 'action_stream_sources_node',
        id, requestPayload});
      return node(result.id);
    },
    exposeStreamSources: (port, purpose) => command({operation:
      'action_expose_stream_sources', id, port, purpose}),
    registerStream: (purpose, source, producerId, parentProducerId) =>
      command({operation: 'action_register_stream', id, purpose, source,
        producerId, parentProducerId}),
    async node(nodeId) {
      const result = await command({operation: 'action_node', id, nodeId});
      return node(result.id);
    },
    async resolveNode(reference) {
      const result = await command({operation: 'action_resolve_node',
        id, reference});
      return node(result.id);
    },
    run: options => command({operation: 'action_run', id}, options),
    call: options => command({operation: 'action_call', id}, options),
    state: options => command({operation: 'action_state', id}, options),
    headers: options => command({operation: 'action_headers', id}, options),
    schema: () => command({operation: 'action_schema', id}),
    settings: () => command({operation: 'action_settings', id}),
    setSettings: settings => command({
      operation: 'action_set_settings', id, settings}),
    makeNested: async (childName, options = {}) => {
      const child = await command({operation: 'action_make_nested', id,
        name: childName, propagateIo: options.propagateIo ?? true,
        forwardHeaders: options.forwardHeaders ?? true,
        ...(options.id === undefined ? {} : {childId: options.id}),
        ...(options.schema === undefined ? {} : {schema: options.schema})});
      return createNativeAction(command, node, child.name, child.id,
        sessionId, streamId, onCancel, true);
    },
    getHeader: name => command({operation: 'action_get_header', id, name}),
    hasHeader: name => command({operation: 'action_has_header', id, name}),
    setHeader: (name, value, options) => command({operation:
      'action_set_header', id, name, value}, options),
    removeHeader: (name, options) => command({operation:
      'action_remove_header', id, name}, options),
    wait: (options = {}) => command({operation: 'action_wait', id,
      ...(options.timeoutMs === undefined ? {} : {timeoutMs: options.timeoutMs})},
    options),
    waitForDispatch: (options = {}) => command({
      operation: 'action_wait_dispatch', id,
      ...(options.timeoutMs === undefined ? {} : {timeoutMs: options.timeoutMs})},
    options),
    cancel: options => {
      onCancel?.(id);
      return command({operation: 'action_cancel', id}, options);
    },
    release: () => command({operation: 'action_release', id}),
  });
}
