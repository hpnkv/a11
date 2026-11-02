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

export function createNativeNode(command, id, scopeId) {
  if (typeof id !== 'string' || !id) throw new TypeError('node id is required');
  const call = (operation, extra = {}, options = {}) => command({operation, id,
    ...(scopeId ? {scopeId} : {}), ...extra}, options);
  return Object.freeze({
    id,
    ready: () => call('node_get'),
    ref: (options = {}) => call('node_ref', options),
    putChunk: (chunk, options = {}) => call('node_put',
      {chunk, sequence: options.sequence ?? null, final: options.final ?? false},
    options),
    putFragment: (fragment, options = {}) => call('node_put_fragment',
      {fragment}, options),
    finalize: (value = {}, options = {}) => {
      if (value.signal && Object.keys(value).length === 1) {
        options = value;
        value = {};
      }
      return call('node_finalize', value, options);
    },
    nextChunk: (options = {}) => call('node_next',
      { ...(options.timeoutMs === undefined ? {} : {timeoutMs: options.timeoutMs})},
    options),
    nextFragment: (options = {}) => call('node_next_fragment',
      { ...(options.timeoutMs === undefined ? {} : {timeoutMs: options.timeoutMs})},
    options),
    close: options => call('node_close', {}, options),
    abortWithStatus: (status, options) => call('node_abort', {status}, options),
    state: options => call('node_state', {}, options),
    readerOptions: options => call('node_reader_options', {}, options),
    setReaderOptions: options => call('node_set_reader_options', {options}),
    resetReader: options => call('node_reset_reader', {options}),
    writerOptions: options => call('node_writer_options', {}, options),
    setWriterOptions: options => call('node_set_writer_options', {options}),
    isWritable: options => call('node_is_writable', {}, options),
    waitForBufferToDrain: (options = {}) =>
      call('node_wait_for_buffer_to_drain',
        {...(options.timeoutMs === undefined ? {} :
          {timeoutMs: options.timeoutMs})}, options),
    cancelReader: options => call('node_cancel_reader', {}, options),
    cancelWriter: options => call('node_cancel_writer', {}, options),
    cancel: options => call('node_cancel', {}, options),
  });
}
