import assert from 'node:assert/strict';
import { createServer as createHttp2Server } from 'node:http2';
import { createServer as createHttp1Server } from 'node:http';
import { createHttpClient } from './http-client.mjs';
import {StatusCode} from './status.mjs';

const encoder = new TextEncoder();
const decoder = new TextDecoder();
async function collect(body) {
  const parts = [];
  for await (const part of body) parts.push(part);
  return decoder.decode(Buffer.concat(parts));
}

const h2 = createHttp2Server();
h2.on('stream', (stream, headers) => {
  if (headers[':path'] === '/slow') {
    setTimeout(() => {
      if (stream.destroyed) return;
      stream.respond({':status': 200});
      stream.end('slow');
    }, 50);
    return;
  }
  if (headers[':path'] === '/push') {
    stream.pushStream({':path': '/asset'}, (error, pushed) => {
      if (error) throw error;
      pushed.respond({':status': 200, 'content-type': 'text/plain'});
      pushed.end('asset');
    });
    stream.respond({':status': 200});
    stream.end('parent');
    return;
  }
  if (headers[':path'] === '/redirect') {
    stream.respond({':status': 302, location: '/ok'});
    stream.end();
    return;
  }
  if (headers[':path'] === '/error') {
    stream.respond({':status': 418});
    stream.end('teapot');
    return;
  }
  stream.respond({':status': 200, 'content-type': 'text/plain'},
    {waitForTrailers: true});
  stream.on('wantTrailers', () => stream.sendTrailers({'x-done': 'yes'}));
  stream.write('first:');
  let uploaded = '';
  stream.on('data', part => { uploaded += part; });
  stream.on('end', () => stream.end(uploaded || 'ok'));
});
await new Promise(resolve => h2.listen(0, '127.0.0.1', resolve));
const h2url = `http://127.0.0.1:${h2.address().port}`;
const client = createHttpClient();
assert.equal(client.capabilities.http2, true);
const response = await client.request({url: h2url + '/ok', method: 'POST',
  httpVersion: '2', body: (async function* () {
    yield encoder.encode('one');
    await new Promise(resolve => setTimeout(resolve, 10));
    yield encoder.encode('two');
  })(), requireConcurrentDuplex: true});
assert.equal(response.status, 200);
assert.equal(response.connection.httpVersion, '2');
assert.equal(await collect(response.body), 'first:onetwo');
assert.equal((await response.trailers())['x-done'], 'yes');
const redirect = await client.request({url: h2url + '/redirect', httpVersion: '2'});
assert.equal(await collect(redirect.body), 'first:ok');
assert.equal(redirect.redirects.length, 1);
const error = await client.request({url: h2url + '/error', httpVersion: '2'});
assert.equal(error.status, 418);
assert.equal(await collect(error.body), 'teapot');
const pushed = await client.request({url: h2url + '/push', httpVersion: '2',
  acceptPushes: true});
assert.equal(await collect(pushed.body), 'parent');
assert.equal(pushed.pushes.length, 1);
assert.equal((await pushed.pushes[0].headers)['content-type'], 'text/plain');
assert.equal(await collect(pushed.pushes[0].body), 'asset');
const limited = await client.request({url: h2url + '/ok', httpVersion: '2',
  maxResponseBytes: 3});
await assert.rejects(collect(limited.body),
  error => error.code === StatusCode.RESOURCE_EXHAUSTED);
const cancelled = new AbortController();
cancelled.abort();
await assert.rejects(client.request({url: h2url + '/ok', httpVersion: '2',
  signal: cancelled.signal}), error => error.code === StatusCode.CANCELLED);
await assert.rejects(client.request({url: h2url + '/slow', httpVersion: '2',
  deadline: Date.now() + 5}),
error => error.code === StatusCode.DEADLINE_EXCEEDED);
await assert.rejects(client.request({url: h2url + '/ok',
  httpVersion: '1.1', acceptPushes: true}),
error => error.code === StatusCode.UNIMPLEMENTED);
await new Promise(resolve => h2.close(resolve));

const h1 = createHttp1Server((request, response) => {
  response.writeHead(200, {'content-type': 'text/plain'});
  response.end('h1');
});
await new Promise(resolve => h1.listen(0, '127.0.0.1', resolve));
const h1response = await client.request({url: `http://127.0.0.1:${h1.address().port}`});
assert.equal(h1response.connection.httpVersion, '1.1');
assert.equal(await collect(h1response.body), 'h1');
await new Promise(resolve => h1.close(resolve));
console.log('Node HTTP/2 and HTTP/1 streaming passed');
