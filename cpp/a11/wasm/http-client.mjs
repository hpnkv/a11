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

import { A11WasmError } from "./bridge.mjs";
import {StatusCode} from "./status.mjs";

const isNode = typeof process !== "undefined" && !!process.versions?.node;
const bytes = value => value instanceof Uint8Array ? value : new Uint8Array(value);
const status = (code, message) => new A11WasmError(code, message);
const REDIRECT = new Set([301, 302, 303, 307, 308]);

function deadlineSignal(signal, deadline) {
  const controller = new AbortController();
  let timer = null;
  let reason = null;
  const abort = (failure) => {
    reason = failure instanceof A11WasmError ? failure
      : status(StatusCode.CANCELLED, "HTTP request cancelled");
    controller.abort(reason);
  };
  if (signal?.aborted) abort();
  else signal?.addEventListener("abort", abort, { once: true });
  if (deadline !== undefined && deadline !== null) {
    const at = deadline instanceof Date ? deadline.getTime() : deadline;
    if (!Number.isFinite(at)) throw status(StatusCode.INVALID_ARGUMENT, "deadline must be finite");
    const remaining = at - Date.now();
    if (remaining <= 0) {
      reason = status(StatusCode.DEADLINE_EXCEEDED, "HTTP deadline exceeded");
      controller.abort(reason);
    } else {
      timer = setTimeout(() => {
        reason = status(StatusCode.DEADLINE_EXCEEDED, "HTTP deadline exceeded");
        controller.abort(reason);
      }, remaining);
    }
  }
  return {signal: controller.signal, reason: () => reason,
    close() {
      if (timer !== null) clearTimeout(timer);
      signal?.removeEventListener("abort", abort);
    },
    cancel(failure) { abort(failure); },
  };
}

function validateRequest(options, capabilities) {
  if (!options || typeof options.url !== "string") throw status(StatusCode.INVALID_ARGUMENT, "url is required");
  let url;
  try { url = new URL(options.url); }
  catch { throw status(StatusCode.INVALID_ARGUMENT, "url must be an absolute HTTP(S) URL"); }
  if (url.protocol !== "http:" && url.protocol !== "https:") {
    throw status(StatusCode.INVALID_ARGUMENT, "HTTP client needs an http or https URL");
  }
  if (options.httpVersion === "2" && !capabilities.http2) {
    throw status(StatusCode.UNIMPLEMENTED, "this transport cannot select HTTP/2");
  }
  if (options.httpVersion === "1.1" && !capabilities.http1) {
    throw status(StatusCode.UNIMPLEMENTED, "this transport cannot select HTTP/1.1");
  }
  if (options.acceptPushes && !capabilities.pushes) {
    throw status(StatusCode.UNIMPLEMENTED, "this transport cannot receive server pushes");
  }
  if (options.requireTrailers && !capabilities.trailers) {
    throw status(StatusCode.UNIMPLEMENTED, "this transport cannot expose trailers");
  }
  if (options.requireConnectionDetails && !capabilities.connectionDetails) {
    throw status(StatusCode.UNIMPLEMENTED, "this transport cannot expose connection details");
  }
  if (options.requireConcurrentDuplex && !capabilities.concurrentDuplex) {
    throw status(StatusCode.UNIMPLEMENTED, "this transport cannot upload and download concurrently");
  }
  if (options.maxRedirects !== undefined && !capabilities.redirectChain) {
    throw status(StatusCode.UNIMPLEMENTED, "this transport cannot enforce a redirect limit");
  }
  const requestLimit = options.maxRequestBytes ?? 16 * 1024 * 1024;
  const responseLimit = options.maxResponseBytes ?? 16 * 1024 * 1024;
  if (!Number.isSafeInteger(requestLimit) || requestLimit <= 0 ||
      !Number.isSafeInteger(responseLimit) || responseLimit <= 0) {
    throw status(StatusCode.INVALID_ARGUMENT, "HTTP byte limits must be positive integers");
  }
  const method = (options.method ?? "GET").toUpperCase();
  if (!/^[A-Z!#$%&'*+.^_`|~-]+$/.test(method)) throw status(StatusCode.INVALID_ARGUMENT, "invalid HTTP method");
  if ((method === "GET" || method === "HEAD") && options.body != null) {
    throw status(StatusCode.INVALID_ARGUMENT, `${method} does not accept a request body`);
  }
  let headers;
  try { headers = new Headers(options.headers ?? {}); }
  catch { throw status(StatusCode.INVALID_ARGUMENT, "invalid HTTP headers"); }
  return {...options, url, method, headers, requestLimit, responseLimit};
}

function bodySource(body) {
  if (body === undefined || body === null) return null;
  if (typeof body === "string") return [new TextEncoder().encode(body)];
  if (body instanceof Uint8Array || body instanceof ArrayBuffer) return [bytes(body)];
  if (typeof body[Symbol.asyncIterator] === "function" ||
      typeof body[Symbol.iterator] === "function") return body;
  throw status(StatusCode.INVALID_ARGUMENT, "body must be bytes, text, or an iterable of bytes");
}

async function* limitedBody(stream, limit, control, finish) {
  let total = 0;
  try {
    for await (const part of stream) {
      total += part.byteLength;
      if (total > limit) {
        const exceeded = status(StatusCode.RESOURCE_EXHAUSTED,
          `HTTP response exceeded ${limit} bytes`);
        control.cancel(exceeded);
        throw exceeded;
      }
      yield new Uint8Array(part);
    }
  } catch (error) {
    throw control.reason() ?? error;
  } finally {
    finish();
  }
}

async function pumpUpload(target, source, limit, control) {
  let total = 0;
  try {
    if (source !== null) {
      for await (const part of source) {
        if (!(part instanceof Uint8Array)) throw status(StatusCode.INVALID_ARGUMENT, "upload chunks must be Uint8Array");
        total += part.byteLength;
        if (total > limit) throw status(StatusCode.RESOURCE_EXHAUSTED, `HTTP upload exceeded ${limit} bytes`);
        if (!target.write(part)) {
          await new Promise((resolve, reject) => {
            target.once("drain", resolve);
            target.once("error", reject);
          });
        }
      }
    }
    target.end();
  } catch (error) {
    target.destroy(error);
    throw control.reason() ?? error;
  }
}

function headerObject(headers) {
  return Object.fromEntries(Object.entries(headers).filter(([name]) => !name.startsWith(":"))
    .map(([name, value]) => [name.toLowerCase(), Array.isArray(value)
      ? value.join(", ") : String(value)]));
}

function headerFields(raw) {
  if (!Array.isArray(raw)) return null;
  const fields = [];
  for (let at = 0; at < raw.length; at += 2) {
    if (!String(raw[at]).startsWith(":")) fields.push([String(raw[at]), String(raw[at + 1])]);
  }
  return fields;
}

async function nodeHttp2(request, control) {
  if (control.signal.aborted) throw control.reason();
  const http2 = await import("node:http2");
  const {PassThrough} = await import('node:stream');
  const origin = request.url.origin;
  const session = http2.connect(origin, {
    settings: { enablePush: request.acceptPushes !== false },
  });
  const connected = new Promise((resolve, reject) => {
    session.once("connect", resolve);
    session.once("error", reject);
    control.signal.addEventListener("abort", () => reject(control.reason()), {once: true});
  });
  try { await connected; }
  catch (error) { session.destroy(); throw control.reason() ?? error; }
  if (request.url.protocol === "https:" && session.socket.alpnProtocol !== "h2") {
    session.destroy();
    throw status(StatusCode.UNIMPLEMENTED, "peer did not negotiate HTTP/2");
  }
  const headers = {":method": request.method,
    ":path": request.url.pathname + request.url.search};
  for (const [key, value] of request.headers) headers[key.toLowerCase()] = value;
  const stream = session.request(headers, {endStream: false});
  const download = new PassThrough({highWaterMark: 64 * 1024});
  stream.pipe(download);
  stream.on('error', error => download.destroy(error));
  const pushes = [];
  if (request.acceptPushes) {
    session.on("stream", (pushed, requestHeaders) => {
      const pushedDownload = new PassThrough({highWaterMark: 64 * 1024});
      pushed.pipe(pushedDownload);
      pushed.on('error', error => pushedDownload.destroy(error));
      const head = new Promise(resolve => pushed.once('push', value => resolve({
        status: Number(value[':status']), headers: headerObject(value)})));
      const push = {requestHeaders: headerObject(requestHeaders),
        request: {method: requestHeaders[':method'] ?? 'GET',
          scheme: requestHeaders[':scheme'] ?? request.url.protocol.slice(0, -1),
          authority: requestHeaders[':authority'] ?? request.url.host,
          path: requestHeaders[':path'] ?? '/'},
        status: head.then(value => value.status),
        headers: head.then(value => value.headers),
        body: limitedBody(pushedDownload, request.responseLimit, control, () => {}),
        cancel: () => pushed.close()};
      pushes.push(push);
    });
  }
  const head = new Promise((resolve, reject) => {
    stream.once("response", (fields, _flags, raw) => resolve({
      status: Number(fields[":status"]), headers: headerObject(fields),
      fields: headerFields(raw),
    }));
    stream.once("error", reject);
    control.signal.addEventListener("abort", () => reject(control.reason()), {once: true});
  });
  let trailers = {};
  stream.on("trailers", fields => { trailers = headerObject(fields); });
  const source = bodySource(request.body);
  const uploaded = pumpUpload(stream, source, request.requestLimit, control);
  uploaded.catch(() => {});
  let response;
  try { response = await head; }
  catch (error) { stream.close(); session.destroy(); throw control.reason() ?? error; }
  const finish = () => {
    if (control.signal.aborted) session.destroy();
    else session.close();
    control.close();
  };
  return {
    ...response,
    url: request.url.href,
    connection: {httpVersion: "2", secure: request.url.protocol === "https:",
      reused: false, remoteAddress: session.socket.remoteAddress ?? null,
      alpnProtocol: session.socket.alpnProtocol ?? null},
    redirects: [], pushes,
    body: limitedBody(download, request.responseLimit, control, finish),
    trailers: async () => { await uploaded.catch(() => {}); return trailers; },
    discard: () => { stream.close(); session.destroy(); },
    cancel: () => { control.cancel(); stream.close(); session.destroy(); },
  };
}

async function nodeHttp1(request, control) {
  if (control.signal.aborted) throw control.reason();
  if (request.acceptPushes) throw status(StatusCode.UNIMPLEMENTED, "HTTP/1.1 has no server pushes");
  const transport = await import(request.url.protocol === "https:" ? "node:https" : "node:http");
  const outgoing = transport.request(request.url, {method: request.method,
    headers: Object.fromEntries(request.headers), signal: control.signal});
  const head = new Promise((resolve, reject) => {
    outgoing.once("response", resolve);
    outgoing.once("error", reject);
    control.signal.addEventListener("abort", () => reject(control.reason()), {once: true});
  });
  const uploaded = pumpUpload(outgoing, bodySource(request.body),
    request.requestLimit, control);
  uploaded.catch(() => {});
  let incoming;
  try { incoming = await head; }
  catch (error) { outgoing.destroy(); throw control.reason() ?? error; }
  const finish = () => control.close();
  return {
    status: incoming.statusCode,
    headers: headerObject(incoming.headers),
    fields: headerFields(incoming.rawHeaders),
    url: request.url.href,
    connection: {httpVersion: incoming.httpVersion,
      secure: !!incoming.socket.encrypted, reused: !!outgoing.reusedSocket,
      remoteAddress: incoming.socket.remoteAddress ?? null,
      alpnProtocol: incoming.socket.alpnProtocol ?? null},
    redirects: [], pushes: [],
    body: limitedBody(incoming, request.responseLimit, control, finish),
    trailers: async () => { await uploaded.catch(() => {}); return headerObject(incoming.trailers); },
    discard: () => outgoing.destroy(),
    cancel: () => { control.cancel(); outgoing.destroy(); },
  };
}

async function browserFetch(request, control) {
  if (request.httpVersion === "1.1" || request.httpVersion === "2") {
    throw status(StatusCode.UNIMPLEMENTED, "Fetch cannot select or report an HTTP version");
  }
  let body;
  const source = bodySource(request.body);
  if (source !== null) {
    if (typeof request.body === "string" ||
        request.body instanceof Uint8Array ||
        request.body instanceof ArrayBuffer) {
      body = typeof request.body === "string"
        ? request.body : bytes(request.body);
      const length = typeof body === "string"
        ? new TextEncoder().encode(body).byteLength : body.byteLength;
      if (length > request.requestLimit) {
        throw status(StatusCode.RESOURCE_EXHAUSTED, "HTTP upload limit exceeded");
      }
    } else {
      if (!request.streamingUpload) {
        throw status(StatusCode.UNIMPLEMENTED, "this browser cannot stream an HTTP upload");
      }
    let total = 0;
    const iterator = source[Symbol.asyncIterator]?.() ?? source[Symbol.iterator]();
    body = new ReadableStream({async pull(controller) {
      const next = await iterator.next();
      if (next.done) return controller.close();
      const part = next.value;
      if (!(part instanceof Uint8Array)) return controller.error(status(StatusCode.INVALID_ARGUMENT, "upload chunks must be bytes"));
      total += part.byteLength;
      if (total > request.requestLimit) return controller.error(status(StatusCode.RESOURCE_EXHAUSTED, "HTTP upload limit exceeded"));
      controller.enqueue(part);
    }, cancel() { void iterator.return?.(); }});
    }
  }
  let response;
  try {
    response = await fetch(request.url, {method: request.method,
      headers: request.headers, body, signal: control.signal,
      redirect: "follow", ...(body instanceof ReadableStream
        ? {duplex: "half"} : {})});
  } catch (error) {
    if (body instanceof ReadableStream && error instanceof TypeError &&
        !control.signal.aborted) {
      throw status(StatusCode.UNIMPLEMENTED, "Fetch rejected the streamed upload on this connection");
    }
    throw control.reason() ?? error;
  }
  if (response.type === "opaque" || response.type === "opaqueredirect") {
    throw status(StatusCode.UNIMPLEMENTED, "Fetch returned an opaque response");
  }
  const reader = response.body?.getReader();
  async function* chunks() {
    if (!reader) return;
    try {
      for (;;) {
        const next = await reader.read();
        if (next.done) return;
        yield next.value;
      }
    } finally {
      reader.releaseLock();
    }
  }
  return {
    status: response.status, headers: Object.fromEntries(response.headers),
    fields: null, url: response.url,
    connection: {httpVersion: null, secure: null, reused: null,
      remoteAddress: null, alpnProtocol: null},
    redirects: null, pushes: null,
    body: limitedBody(chunks(), request.responseLimit, control, () => control.close()),
    trailers: async () => null,
    cancel: () => control.cancel(),
  };
}

/** Host HTTP with explicit transport capabilities and bounded streaming. */
export class A11HttpClient {
  constructor(options = {}) {
    this.transport = options.transport ?? null;
    let browserUpload = false;
    if (!isNode && typeof Request === "function" &&
        typeof ReadableStream === "function") {
      try {
        const probe = new Request("https://example.invalid/", {method: "POST",
          body: new ReadableStream(), duplex: "half"});
        browserUpload = probe.body !== null;
      } catch { browserUpload = false; }
    }
    this.capabilities = Object.freeze(this.transport?.capabilities ?? (isNode
      ? {http1: true, http2: true, streamingUpload: true,
        streamingDownload: true, concurrentDuplex: true, trailers: true,
        pushes: true, redirectChain: true, connectionDetails: true}
      : {http1: false, http2: false, streamingUpload: browserUpload,
        streamingUploadByOrigin: 'conditional',
        streamingDownload: true, concurrentDuplex: false, trailers: false,
        pushes: false, redirectChain: false, connectionDetails: false}));
  }

  async request(options) {
    const request = validateRequest(options, this.capabilities);
    if (this.transport) return this.transport.request(request);
    const control = deadlineSignal(request.signal, request.deadline);
    if (control.signal.aborted) throw control.reason();
    if (!isNode) return browserFetch({...request,
      streamingUpload: this.capabilities.streamingUpload}, control);
    const redirects = [];
    let current = request;
    const maximum = options.maxRedirects ?? 5;
    for (;;) {
      let response;
      if (current.httpVersion === "2" ||
          (current.httpVersion !== "1.1" && current.url.protocol === "https:")) {
        try { response = await nodeHttp2(current, control); }
        catch (error) {
          if (current.httpVersion === "2" || error?.code !== 12) throw error;
          response = await nodeHttp1(current, control);
        }
      } else response = await nodeHttp1(current, control);
      const location = response.headers.location;
      if (!REDIRECT.has(response.status) || !location || maximum === 0) {
        response.redirects = redirects;
        return response;
      }
      if (redirects.length >= maximum) {
        response.discard();
        throw status(StatusCode.RESOURCE_EXHAUSTED, "HTTP redirect limit exceeded");
      }
      const next = new URL(location, current.url);
      if (next.protocol !== "http:" && next.protocol !== "https:") {
        response.discard();
        throw status(StatusCode.INVALID_ARGUMENT, "redirect left HTTP(S)");
      }
      const rewrite = response.status === 301 || response.status === 302 ||
        response.status === 303;
      if (!rewrite && current.body != null &&
          !(typeof current.body === "string" || current.body instanceof Uint8Array ||
            current.body instanceof ArrayBuffer)) {
        response.discard();
        throw status(StatusCode.UNIMPLEMENTED, "streamed request body cannot be replayed after a 307/308 redirect");
      }
      redirects.push({url: current.url.href, status: response.status, location});
      response.discard();
      current = {...current, url: next,
        method: rewrite ? "GET" : current.method,
        body: rewrite ? null : current.body};
    }
  }
}

export function createHttpClient(options = {}) { return new A11HttpClient(options); }
