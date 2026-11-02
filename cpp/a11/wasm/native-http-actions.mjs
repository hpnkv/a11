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

import {A11WasmError, StatusCode, statusFromThrown} from './status.mjs';

const utf8 = new TextDecoder();
const encode = new TextEncoder();

function bytesFromChunk(chunk) {
  if (chunk === null) return null;
  if (typeof chunk.data !== 'string') {
    throw new A11WasmError(StatusCode.DATA_LOSS,
      'Action input chunk has no byte payload');
  }
  if (typeof Buffer !== 'undefined') return Buffer.from(chunk.data, 'base64');
  const binary = atob(chunk.data);
  return Uint8Array.from(binary, character => character.charCodeAt(0));
}

function chunkFromBytes(bytes, mimetype) {
  let data;
  if (typeof Buffer !== 'undefined') data = Buffer.from(bytes).toString('base64');
  else {
    let binary = '';
    for (let at = 0; at < bytes.length; at += 16_384) {
      binary += String.fromCharCode(...bytes.subarray(at, at + 16_384));
    }
    data = btoa(binary);
  }
  return {data, metadata: {mimetype}};
}

async function input(action, name, type = 'text') {
  const node = await action.input(name);
  const chunk = await node.nextChunk();
  const bytes = bytesFromChunk(chunk);
  if (bytes === null) return null;
  const value = utf8.decode(bytes);
  if (type === 'json' ||
      chunk?.metadata?.mimetype === 'application/json') {
    try { return JSON.parse(value); }
    catch { throw new A11WasmError(StatusCode.INVALID_ARGUMENT,
      `${name} is not JSON`); }
  }
  return value;
}

async function* uploadParts(action) {
  const node = await action.input('request_body');
  for (;;) {
    const bytes = bytesFromChunk(await node.nextChunk());
    if (bytes === null) return;
    yield bytes;
  }
}

async function uploadBody(action, streaming) {
  if (streaming) return uploadParts(action);
  const parts = [];
  let length = 0;
  for await (const part of uploadParts(action)) {
    parts.push(part);
    length += part.length;
  }
  if (length === 0) return null;
  const joined = new Uint8Array(length);
  let offset = 0;
  for (const part of parts) {
    joined.set(part, offset);
    offset += part.length;
  }
  return joined;
}

async function requestFor(action, http, context) {
  const [url, method, options] = await Promise.all([
    input(action, 'url'), input(action, 'method'), input(action, 'options', 'json'),
  ]);
  if (typeof url !== 'string' || !url) {
    throw new A11WasmError(StatusCode.INVALID_ARGUMENT, 'url is required');
  }
  const config = options ?? {};
  if (typeof config !== 'object' || Array.isArray(config)) {
    throw new A11WasmError(StatusCode.INVALID_ARGUMENT,
      'HTTP options must be an object');
  }
  if (config.tls !== undefined || config.reuse_connection === true) {
    throw new A11WasmError(StatusCode.UNIMPLEMENTED,
      'This HTTP transport cannot apply the requested TLS or connection policy');
  }
  const bodyMode = config.request_body ?? 'buffer';
  if (bodyMode !== 'buffer' && bodyMode !== 'stream') {
    throw new A11WasmError(StatusCode.INVALID_ARGUMENT,
      'request_body must be buffer or stream');
  }
  if (config.headers !== undefined &&
      (typeof config.headers !== 'object' || config.headers === null ||
       Array.isArray(config.headers))) {
    throw new A11WasmError(StatusCode.INVALID_ARGUMENT,
      'headers must be an object');
  }
  const headers = {...(config.headers ?? {})};
  if (config.user_agent !== undefined) headers['user-agent'] = config.user_agent;
  const request = {url, method: method ?? 'GET', headers,
    body: await uploadBody(action, bodyMode === 'stream'),
    httpVersion: config.http_version ?? 'auto',
    acceptPushes: config.accept_pushes ?? false,
    maxRedirects: config.max_redirects,
    maxRequestBytes: config.max_body_bytes,
    maxResponseBytes: config.max_body_bytes,
    signal: context.signal};
  if (config.timeout !== undefined) {
    if (typeof config.timeout !== 'number' || !Number.isFinite(config.timeout) ||
        config.timeout < 0) {
      throw new A11WasmError(StatusCode.INVALID_ARGUMENT,
        'timeout must be a nonnegative number of seconds');
    }
    request.deadline = Date.now() + config.timeout * 1000;
  }
  try { return {response: await http.request(request), config}; }
  catch (error) { throw actionFailure(error, 'HTTP action', 'request'); }
}

function outputWriter(action, omitted) {
  const nodes = new Map();
  const node = async name => {
    if (omitted.has(name)) return null;
    if (!nodes.has(name)) nodes.set(name, await action.output(name));
    return nodes.get(name);
  };
  return {
    async write(name, value, mimetype = 'application/json') {
      const target = await node(name);
      if (target === null || value === undefined) return;
      const bytes = mimetype === 'application/octet-stream'
        ? value : encode.encode(mimetype === 'text/plain'
          ? String(value) : JSON.stringify(value));
      await target.putChunk(chunkFromBytes(bytes, mimetype));
    },
    async finish() {
      await Promise.all([...nodes.values()].map(value => value.finalize()));
    },
  };
}

function omittedOutputs(config) {
  if (config.omit === undefined) return new Set();
  if (!Array.isArray(config.omit) ||
      config.omit.some(name => typeof name !== 'string')) {
    throw new A11WasmError(StatusCode.INVALID_ARGUMENT,
      'omit must be an array of output names');
  }
  return new Set(config.omit);
}

function actionFailure(error, name, phase) {
  const status = statusFromThrown(error);
  if (status.code === StatusCode.RESOURCE_EXHAUSTED &&
      /HTTP (response|upload) exceeded/.test(status.message)) {
    return new A11WasmError({code: StatusCode.OUT_OF_RANGE,
      message: `The ${phase === 'request' ? 'request' : 'response'} body exceeds options.max_body_bytes`,
      details: status.details});
  }
  return new A11WasmError({...status,
    message: `${name} ${phase}: ${status.message}`});
}

async function makeHttpRequest(action, http, context) {
  const {response, config} = await requestFor(action, http, context);
  const output = outputWriter(action, omittedOutputs(config));
  let phase = 'headers';
  try {
    await output.write('status_code', response.status);
    await output.write('headers', response.headers);
    await output.write('connection', {url: response.url,
      http_version: response.connection.httpVersion,
      secure: response.connection.secure, reused: response.connection.reused,
      remote_address: response.connection.remoteAddress,
      alpn_protocol: response.connection.alpnProtocol});
    for (const field of response.fields ?? []) await output.write('fields', field);
    for (const redirect of response.redirects ?? []) {
      await output.write('redirects', redirect);
    }
    phase = 'body';
    for await (const bytes of response.body) {
      await output.write('body', bytes, 'application/octet-stream');
    }
    phase = 'trailers';
    const trailers = await response.trailers();
    if (trailers !== null) await output.write('trailers', trailers);
    phase = 'pushes';
    let pushIndex = 0;
    for (const push of response.pushes ?? []) {
      const [headers, status] = await Promise.all([push.headers, push.status]);
      const requested = push.requestHeaders;
      const method = push.request.method;
      const path = push.request.path;
      const scheme = push.request.scheme;
      const authority = push.request.authority;
      const bodyId = `${action.id}#push-${pushIndex++}-body`;
      const bodyNode = await action.node(bodyId);
      await output.write('pushes', {method,
        url: `${scheme}://${authority}${path}`, path,
        status, headers,
        request_headers: requested, body: bodyId});
      try {
        for await (const bytes of push.body) {
          await bodyNode.putChunk(chunkFromBytes(bytes,
            'application/octet-stream'));
        }
        await bodyNode.finalize();
      } catch (error) {
        push.cancel();
        throw error;
      }
    }
    phase = 'finish';
    await output.finish();
  } catch (error) {
    response.cancel();
    throw actionFailure(error, 'make_http_request', phase);
  }
}

async function webFetch(action, http, context) {
  const {response, config} = await requestFor(action, http, context);
  const omitted = omittedOutputs(config);
  const output = outputWriter(action, omitted);
  const type = (response.headers['content-type'] ?? '').split(';')[0].trim()
    .toLowerCase();
  const jsonType = type === 'application/json' || type.endsWith('+json');
  const ndjson = ['application/x-ndjson', 'application/jsonl',
    'application/x-jsonlines', 'application/json-seq'].includes(type);
  const sse = type === 'text/event-stream';
  const wantsItems = !omitted.has('items');
  const wantsWhole = !omitted.has('text') || !omitted.has('json') ||
    (wantsItems && jsonType);
  const textParts = [];
  const decoder = new TextDecoder();
  let lines = '';
  const event = {data: '', event: '', id: '', retry: ''};
  const flushEvent = async () => {
    if (!event.data && !event.event && !event.id) return;
    const record = {event: event.event || 'message',
      data: event.data, id: event.id};
    try { record.json = JSON.parse(event.data); } catch {}
    if (event.retry) record.retry = event.retry;
    await output.write('items', record);
    event.data = '';
    event.event = '';
    event.id = '';
  };
  const readLine = async line => {
    if (line.endsWith('\r')) line = line.slice(0, -1);
    if (ndjson && line) {
      let value = line;
      try { value = JSON.parse(line); } catch {}
      await output.write('items', value);
    } else if (sse) {
      if (!line) return flushEvent();
      if (line.startsWith(':')) return;
      const colon = line.indexOf(':');
      const field = colon < 0 ? line : line.slice(0, colon);
      const value = colon < 0 ? '' : line.slice(colon + 1).replace(/^ /, '');
      if (field === 'data') event.data += (event.data ? '\n' : '') + value;
      else if (field === 'event') event.event = value;
      else if (field === 'id') event.id = value;
      else if (field === 'retry') event.retry = value;
    }
  };
  try {
    await output.write('status_code', response.status);
    await output.write('ok', response.status < 400);
    await output.write('headers', response.headers);
    for await (const bytes of response.body) {
      await output.write('body', bytes, 'application/octet-stream');
      const part = decoder.decode(bytes, {stream: true});
      if (wantsWhole) textParts.push(part);
      if (wantsItems && (ndjson || sse)) {
        lines += part;
        let newline;
        while ((newline = lines.indexOf('\n')) >= 0) {
          const line = lines.slice(0, newline);
          lines = lines.slice(newline + 1);
          await readLine(line);
        }
      }
    }
    const tail = decoder.decode();
    if (wantsWhole) textParts.push(tail);
    if (wantsItems && (ndjson || sse)) lines += tail;
    const text = textParts.join('');
    if (!omitted.has('text')) await output.write('text', text, 'text/plain');
    if (wantsItems && (ndjson || sse)) {
      if (lines.trim()) await readLine(lines);
      if (sse) await flushEvent();
    }
    if (jsonType) {
      let parsed;
      try { parsed = JSON.parse(text); }
      catch { parsed = undefined; }
      if (parsed !== undefined) {
        await output.write('json', parsed);
        if (wantsItems && Array.isArray(parsed)) {
          for (const item of parsed) await output.write('items', item);
        }
      }
    }
    await output.finish();
  } catch (error) {
    response.cancel();
    throw actionFailure(error, 'web-fetch', 'body');
  }
}

export function registerHttpActions(hostActions, runtime, enabled) {
  if (enabled.make_http_request) {
    hostActions.set('make_http_request', {schema: null,
      handler: (action, context) => makeHttpRequest(action, runtime.http, context)});
  }
  if (enabled['web-fetch']) {
    hostActions.set('web-fetch', {schema: null,
      handler: (action, context) => webFetch(action, runtime.http, context)});
  }
}
