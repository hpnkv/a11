import assert from 'node:assert/strict';
import {spawn} from 'node:child_process';
import {existsSync} from 'node:fs';
import {mkdtemp, rm} from 'node:fs/promises';
import {createServer} from 'node:net';
import {tmpdir} from 'node:os';
import {join} from 'node:path';
import {fileURLToPath} from 'node:url';
import {createRequire} from 'node:module';

const requireFromSdk = createRequire(new URL('../../../js/package.json', import.meta.url));
const {build} = requireFromSdk('esbuild');
await build({
  entryPoints: [fileURLToPath(new URL('./test-sdk-browser-entry.mjs', import.meta.url))],
  bundle: true, format: 'esm', platform: 'browser',
  outfile: fileURLToPath(new URL('./build/sdk-browser-test.mjs', import.meta.url)),
});
await build({
  entryPoints: [fileURLToPath(new URL('./test-guide-isolation-entry.mjs',
    import.meta.url))],
  bundle: true, format: 'esm', platform: 'browser',
  outfile: fileURLToPath(new URL('./build/guide-isolation-test.mjs',
    import.meta.url)),
});

const chrome = process.env.A11_CHROME_BIN ?? [
  '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome',
  '/usr/bin/google-chrome', '/usr/bin/chromium',
].find(existsSync);
assert.ok(chrome, 'Chrome or Chromium is required for the browser test');

const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
async function freePort() {
  const server = createServer();
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  const port = server.address().port;
  await new Promise(resolve => server.close(resolve));
  return port;
}

async function waitForText(url, expected) {
  const deadline = Date.now() + 15000;
  while (Date.now() < deadline) {
    try {
      const response = await fetch(url);
      const value = await response.json();
      const page = value.find(item => item.type === 'page');
      if (page?.webSocketDebuggerUrl) return page.webSocketDebuggerUrl;
    } catch {}
    await delay(50);
  }
  throw new Error(`Chrome did not expose ${expected}`);
}

async function runPage(socketUrl, url) {
  const socket = new WebSocket(socketUrl);
  await new Promise((resolve, reject) => {
    socket.addEventListener('open', resolve, {once: true});
    socket.addEventListener('error', reject, {once: true});
  });
  let serial = 0;
  const pending = new Map();
  const events = [];
  socket.addEventListener('message', event => {
    const message = JSON.parse(event.data);
    if (message.method === 'Runtime.exceptionThrown') {
      events.push(message.params.exceptionDetails.exception?.description ??
        message.params.exceptionDetails.text);
    }
    if (message.method === 'Log.entryAdded') {
      events.push(message.params.entry.text);
    }
    if (message.method === 'Target.attachedToTarget') {
      void send('Runtime.enable', {}, message.params.sessionId).catch(() => {});
    }
    const item = pending.get(message.id);
    if (!item) return;
    pending.delete(message.id);
    if (message.error) item.reject(new Error(message.error.message));
    else item.resolve(message.result);
  });
  const send = (method, params = {}, sessionId) => new Promise((resolve, reject) => {
    const id = ++serial;
    pending.set(id, {resolve, reject});
    socket.send(JSON.stringify({id, method, params,
      ...(sessionId ? {sessionId} : {})}));
  });
  try {
    await send('Page.enable');
    await send('Runtime.enable');
    await send('Log.enable');
    await send('Target.setAutoAttach', {autoAttach: true,
      waitForDebuggerOnStart: false, flatten: true});
    const navigation = await send('Page.navigate', {url});
    const deadline = Date.now() + 15000;
    let lastValue;
    while (Date.now() < deadline) {
      const evaluation = await send('Runtime.evaluate', {expression:
        'JSON.stringify({url: location.href, text: document.querySelector("#result")?.textContent, body: document.body?.innerText.slice(0, 150)})',
        returnByValue: true});
      const state = JSON.parse(evaluation.result?.value ?? '{}');
      const value = state.text;
      lastValue = state;
      if (value === 'PASS') return;
      if (typeof value === 'string' && value.startsWith('FAIL:')) {
        throw new Error(`${value}; worker events: ${events.join('; ')}`);
      }
      await delay(50);
    }
    throw new Error(`${url} did not finish: ${JSON.stringify({navigation,
      lastValue, events})}`);
  } finally {
    socket.close();
  }
}

const webPort = await freePort();
const debugPort = await freePort();
const profile = await mkdtemp(join(tmpdir(), 'a11-wasm-browser-'));
const server = spawn(process.execPath,
  [new URL('./serve.mjs', import.meta.url).pathname, String(webPort)],
  {stdio: 'ignore'});
const browser = spawn(chrome, ['--headless=new', '--no-sandbox',
  '--disable-gpu', `--user-data-dir=${profile}`,
  `--remote-debugging-port=${debugPort}`, 'about:blank'],
{stdio: 'ignore'});
try {
  const socketUrl = await waitForText(
    `http://127.0.0.1:${debugPort}/json/list`, 'a page');
  for (const page of ['test-http-browser.html', 'test-thread-browser.html',
    'test-native-browser.html', 'test-sdk-browser.html',
    'guides/isolation-test/']) {
    await runPage(socketUrl, `http://127.0.0.1:${webPort}/${page}`);
  }
  console.log('Browser HTTP, native Flow, and SDK graph passed');
} finally {
  browser.kill();
  server.kill();
  await delay(100);
  await rm(profile, {recursive: true, force: true}).catch(() => {});
}
