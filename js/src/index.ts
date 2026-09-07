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

/**
 * Public TypeScript surface for building A11 browser and Node agents.
 *
 * @packageDocumentation
 */

export * from './status.js';
export * from './status_codec.js';
export * from './bytes.js';
export * from './data.js';
export * from './serialization.js';

export * from './serial_tags.js';
export * from './wire_values.js';

export * from './chunk_store.js';
export * from './chunk_store_reader.js';
export * from './chunk_store_writer.js';
export * from './async_node.js';

export * from './action_schema.js';
export * from './action_log.js';
export * from './action.js';
export * from './authorization.js';
export * from './action_registry.js';
export * from './action_builtins.js';
export * from './schema_json.js';
export * from './session.js';

export * from './byte_chunking.js';
export * from './sticky_metadata.js';
export * from './wire_stream.js';
export * from './channel_wire_stream.js';
export * from './in_process_wire_stream.js';
export * from './websocket_wire_stream.js';
export * from './signalling.js';
export * from './webrtc_wire_stream.js';
export * from './http_sse_wire_stream.js';

export * from './flow_highlight.js';

export * from './sdk/index.js';
