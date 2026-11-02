import assert from 'node:assert/strict';
import {createServer} from 'node:http';
import {createServer as createHttp2Server} from 'node:http2';
import {createA11WasmRuntime} from './native-index.mjs';
import {StatusCode} from './status.mjs';

const server = createServer((request, response) => {
  if (request.url === '/ndjson') {
    response.writeHead(200, {'content-type': 'application/x-ndjson'});
    response.end('{"n":1}\n{"n":2}\n');
    return;
  }
  if (request.url === '/sse') {
    response.writeHead(200, {'content-type': 'text/event-stream'});
    response.end('event: update\nid: evt-1\ndata: {"n":3}\n\n');
    return;
  }
  if (request.url === '/error') {
    response.writeHead(418, {'content-type': 'text/plain'});
    response.end('teapot');
    return;
  }
  response.writeHead(200, {'content-type': 'text/plain'});
  response.write('first:');
  request.on('data', part => response.write(part));
  request.on('end', () => response.end('done'));
});
await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
const base = `http://127.0.0.1:${server.address().port}`;
const h2 = createHttp2Server();
h2.on('stream', (stream, headers) => {
  if (headers[':path'] === '/push') {
    stream.pushStream({':path': '/asset'}, (error, pushed) => {
      if (error) return;
      pushed.respond({':status': 200, 'content-type': 'text/plain'});
      pushed.end('asset-body');
    });
    stream.respond({':status': 200, 'content-type': 'text/plain'});
    stream.end('parent-body');
    return;
  }
  stream.respond({':status': 200, 'content-type': 'text/plain'},
    {waitForTrailers: true});
  stream.on('wantTrailers', () => stream.sendTrailers({'x-done': 'yes'}));
  stream.write('h2-body');
  stream.on('data', () => {});
  stream.on('end', () => stream.end());
});
await new Promise(resolve => h2.listen(0, '127.0.0.1', resolve));
const h2url = `http://127.0.0.1:${h2.address().port}`;
const decode = chunk => Buffer.from(chunk.data, 'base64').toString();
const httpSource = url => `flow request {
  out status: integer
  out body: bytes stream
  response = run make_http_request(url: ${JSON.stringify(url)})
  response.status_code -> status
  response.body -> body
}`;
const fetchSource = url => `flow fetched {
  out status: integer
  out ok: bool
  out text: string
  response = run web-fetch(url: ${JSON.stringify(url)})
  response.status_code -> status
  response.ok -> ok
  response.text -> text
}`;
const itemsSource = url => `flow items {
  out items: object stream
  response = run web-fetch(url: ${JSON.stringify(url)})
  response.items -> items
}`;

try {
  const runtime = await createA11WasmRuntime();
  try {
    const result = await runtime.runFlow({source: httpSource(base + '/ok'),
      flow: 'request'});
    assert.equal(Number(decode(result.status[0])), 200);
    assert.equal(result.body.map(decode).join(''), 'first:done');
    const directAction = runtime.action('make_http_request', 'direct-http-action');
    await directAction.ready();
    const urlInput = await directAction.input('url');
    const methodInput = await directAction.input('method');
    const optionsInput = await directAction.input('options');
    const bodyInput = await directAction.input('request_body');
    const statusOutput = await directAction.output('status_code');
    const bodyOutput = await directAction.output('body');
    await directAction.run();
    await urlInput.putChunk({data: Buffer.from(JSON.stringify(base + '/ok'))
      .toString('base64'), metadata: {mimetype: 'application/json'}});
    await urlInput.finalize();
    await Promise.all([methodInput.finalize(), optionsInput.finalize(),
      bodyInput.finalize()]);
    await directAction.wait({timeoutMs: 2000});
    assert.equal(Number(decode(await statusOutput.nextChunk({timeoutMs: 2000}))),
      200);
    const directBody = [];
    for (;;) {
      const chunk = await bodyOutput.nextChunk({timeoutMs: 2000});
      if (chunk === null) break;
      directBody.push(decode(chunk));
    }
    assert.equal(directBody.join(''), 'first:done');
    const uploaded = await runtime.runFlow({source: `flow upload {
      out body: bytes stream
      response = run make_http_request(
        url: ${JSON.stringify(base + '/ok')},
        method: "POST",
        options: {request_body: "stream"},
        request_body: std::b64decode("cGF5bG9hZA==")
      )
      response.body -> body
    }`, flow: 'upload'});
    assert.equal(uploaded.body.map(decode).join(''), 'first:payloaddone');
    const error = await runtime.runFlow({source: fetchSource(base + '/error'),
      flow: 'fetched'});
    assert.equal(Number(decode(error.status[0])), 418);
    assert.equal(decode(error.ok[0]), 'false');
    assert.equal(decode(error.text[0]), 'teapot');
    const lines = await runtime.runFlow({source: itemsSource(base + '/ndjson'),
      flow: 'items'});
    assert.deepEqual(lines.items.map(chunk => JSON.parse(decode(chunk))),
      [{n: 1}, {n: 2}]);
    const events = await runtime.runFlow({source: itemsSource(base + '/sse'),
      flow: 'items'});
    assert.deepEqual(JSON.parse(decode(events.items[0])),
      {event: 'update', id: 'evt-1', data: '{"n":3}', json: {n: 3}});
    const direct = await runtime.http.request({url: h2url, method: 'GET',
      headers: {}, body: null, httpVersion: '2', acceptPushes: false});
    const directParts = [];
    for await (const part of direct.body) directParts.push(part);
    assert.equal(Buffer.concat(directParts).toString(), 'h2-body');
    assert.equal((await direct.trailers())['x-done'], 'yes');
    const h2result = await runtime.runFlow({source: `flow h2request {
      out body: bytes stream
      out trailers: object
      response = run make_http_request(
        url: ${JSON.stringify(h2url)},
        options: {http_version: "2"}
      )
      response.body -> body
      response.trailers -> trailers
    }`, flow: 'h2request'});
    assert.equal(h2result.body.map(decode).join(''), 'h2-body');
    assert.equal(JSON.parse(decode(h2result.trailers[0]))['x-done'], 'yes');
    const pushed = await runtime.runFlowWithNodes({source: `flow pushed {
      out records: object stream
      response = run make_http_request(
        url: ${JSON.stringify(h2url + '/push')},
        options: {http_version: "2", accept_pushes: true}
      )
      response.pushes -> records
    }`, flow: 'pushed'});
    assert.equal(pushed.outputs.records.length, 1);
    const pushedRecord = JSON.parse(decode(pushed.outputs.records[0]));
    assert.equal(pushedRecord.path, '/asset');
    const pushedBody = pushed.node(pushedRecord.body);
    assert.equal(decode(await pushedBody.nextChunk()), 'asset-body');
    assert.equal(await pushedBody.nextChunk(), null);
    await pushed.release();
    await assert.rejects(pushedBody.ready(),
      error => error.code === StatusCode.NOT_FOUND);
    const pushBody = await runtime.runFlow({source: `flow pushed_body {
      out body: bytes stream
      response = run make_http_request(
        url: ${JSON.stringify(h2url + '/push')},
        options: {http_version: "2", accept_pushes: true}
      )
      for rec in response.pushes {
        node(rec.body) -> body
      }
    }`, flow: 'pushed_body'});
    assert.equal(pushBody.body.map(decode).join(''), 'asset-body');
    await assert.rejects(runtime.runFlow({source: `flow limited {
      out body: bytes stream
      response = run make_http_request(
        url: ${JSON.stringify(base + '/ok')},
        options: {max_body_bytes: 3}
      )
      response.body -> body
    }`, flow: 'limited'}),
    error => error.code === StatusCode.OUT_OF_RANGE);
    await assert.rejects(runtime.runFlow({source: `flow unsupported {
      out body: bytes stream
      response = run make_http_request(
        url: ${JSON.stringify(base + '/ok')},
        options: {reuse_connection: true}
      )
      response.body -> body
    }`, flow: 'unsupported'}),
    error => error.code === StatusCode.UNIMPLEMENTED);
  } finally { runtime.dispose(); }
  const disabled = await createA11WasmRuntime({actions: {
    make_http_request: false, 'web-fetch': false}});
  try {
    await assert.rejects(disabled.runFlow({source: httpSource(base + '/ok'),
      flow: 'request'}), error => error.code === StatusCode.NOT_FOUND);
  } finally { disabled.dispose(); }
} finally {
  await new Promise(resolve => server.close(resolve));
  await new Promise(resolve => h2.close(resolve));
}
