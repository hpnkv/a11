import assert from 'node:assert/strict';
import http from 'node:http';
import test from 'node:test';

import { WebSocketServer } from 'ws';

import {
  Chunk,
  HttpSseClientWireStream,
  InProcessWireStream,
  NodeFragment,
  SignallingMessage,
  SignallingMessageType,
  StatusCode,
  TurnRelayType,
  TurnServer,
  WebRtcWireStream,
  WebSocketSignallingClient,
  WebSocketWireStream,
  WireMessage,
  WireStreamWithRecv,
  isOk,
  unavailableError,
  okStatus,
} from '../dist/index.js';

function messageWithByte(id, byte) {
  return new WireMessage({
    nodeFragments: [new NodeFragment({
      id,
      data: new Chunk({ data: new Uint8Array([byte]) }),
      seq: 0,
      continued: false,
    })],
  });
}

test('in-process streams preserve messages, trailers, and abort details', async () => {
  const pair = InProcessWireStream.createPair();
  assert.equal(isOk(pair), true);
  const sender = WireStreamWithRecv.create(pair[0]);
  const receiver = WireStreamWithRecv.create(pair[1]);
  assert.equal(isOk(sender), true);
  assert.equal(isOk(receiver), true);
  const started = await Promise.all([sender.start(), receiver.accept()]);
  assert.ok(started.every(isOk));

  assert.equal(isOk(sender.send(messageWithByte('node', 7))), true);
  const received = await receiver.receive(1000);
  assert.equal(isOk(received), true);
  assert.equal(received.nodeFragments[0].data.data[0], 7);

  const trailers = new Map([['x-a11-test', new Uint8Array([8, 9])]]);
  assert.equal(isOk(sender.halfClose(trailers)), true);
  assert.equal(await receiver.receive(1000), null);
  assert.deepEqual(receiver.getTrailers().get('x-a11-test'), new Uint8Array([8, 9]));
  assert.equal(isOk(receiver.halfClose()), true);
  assert.ok((await Promise.all([pair[0].wait(), pair[1].wait()])).every(isOk));

  const abortedPair = InProcessWireStream.createPair();
  assert.equal(isOk(abortedPair), true);
  const left = WireStreamWithRecv.create(abortedPair[0]);
  const right = WireStreamWithRecv.create(abortedPair[1]);
  assert.equal(isOk(left), true);
  assert.equal(isOk(right), true);
  assert.ok((await Promise.all([left.start(), right.accept()])).every(isOk));
  const failure = unavailableError('peer unavailable', [{ retry_after_ms: 25 }]);
  assert.equal(isOk(left.abort(failure)), true);
  const remoteFailure = await right.receive(1000);
  assert.equal(isOk(remoteFailure), false);
  assert.equal(remoteFailure.code, StatusCode.UNAVAILABLE);
  assert.equal(remoteFailure.message, 'peer unavailable');
  assert.deepEqual(remoteFailure.details, [{ retry_after_ms: 25 }]);
});

test('WebSocket client runs the A11 channel framing over Node ws', async (t) => {
  const server = new WebSocketServer({ port: 0 });
  await new Promise((resolve) => server.once('listening', resolve));
  t.after(async () => {
    for (const client of server.clients) client.terminate();
    await new Promise((resolve) => server.close(resolve));
  });
  server.on('connection', (socket) => {
    socket.on('message', (data) => socket.send(data, { binary: true }));
  });
  const address = server.address();
  assert.equal(typeof address, 'object');
  const stream = WebSocketWireStream.createClient(
    `ws://127.0.0.1:${address.port}`,
  );
  assert.equal(isOk(stream), true);
  const received = WireStreamWithRecv.create(stream);
  assert.equal(isOk(received), true);
  assert.equal(isOk(await received.start()), true);

  assert.equal(isOk(received.send(messageWithByte('echo', 33))), true);
  const echo = await received.receive(2000);
  assert.equal(isOk(echo), true);
  assert.equal(echo.nodeFragments[0].data.data[0], 33);
  assert.equal(isOk(received.halfClose()), true);
  assert.equal(await received.receive(2000), null);
  assert.equal(isOk(await stream.wait()), true);
});

test('HTTP SSE client receives events and uses its POST backchannel', async (t) => {
  let eventResponse = null;
  const posted = [];
  const initial = messageWithByte('incoming', 44).toJson();
  assert.equal(isOk(initial), true);

  const server = http.createServer(async (request, response) => {
    if (request.method === 'POST' && request.url === '/connect') {
      eventResponse = response;
      response.writeHead(200, {
        'content-type': 'text/event-stream',
        'cache-control': 'no-cache',
        'x-a11-stream-id': 'fixture-stream',
      });
      response.write(`data: ${initial}\n\n`);
      return;
    }
    if (
      request.method === 'POST' &&
      request.url === '/streams/fixture-stream/message'
    ) {
      const chunks = [];
      for await (const chunk of request) chunks.push(chunk);
      const body = Buffer.concat(chunks).toString('utf8');
      posted.push(body);
      const message = WireMessage.fromJson(body);
      response.writeHead(isOk(message) ? 204 : 400);
      response.end();
      if (isOk(message) && message.isHalfClose && eventResponse !== null) {
        eventResponse.write('data: {}\n\n');
        eventResponse.end();
      }
      return;
    }
    response.writeHead(404);
    response.end();
  });
  await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
  t.after(async () => {
    eventResponse?.end();
    await new Promise((resolve) => server.close(resolve));
  });
  const address = server.address();
  assert.equal(typeof address, 'object');
  const stream = HttpSseClientWireStream.create(
    `http://127.0.0.1:${address.port}`,
  );
  assert.equal(isOk(stream), true);
  const received = WireStreamWithRecv.create(stream);
  assert.equal(isOk(received), true);
  assert.equal(isOk(await received.start()), true);
  assert.equal(stream.getId(), 'fixture-stream');

  const incoming = await received.receive(2000);
  assert.equal(isOk(incoming), true);
  assert.equal(incoming.nodeFragments[0].data.data[0], 44);

  assert.equal(isOk(received.send(messageWithByte('outgoing', 55))), true);
  for (let attempt = 0; attempt < 100 && posted.length === 0; ++attempt) {
    await new Promise((resolve) => setTimeout(resolve, 5));
  }
  assert.ok(posted.some((body) => body.includes('outgoing')));

  assert.equal(isOk(received.halfClose()), true);
  const drained = received.drainOutgoingMessages();
  assert.equal(await received.receive(2000), null);
  assert.equal(isOk(await drained), true);
  assert.ok(posted.some((body) => body === '{}'));
});

test('WebSocket signalling client validates and routes peer messages', async (t) => {
  const server = new WebSocketServer({ port: 0 });
  await new Promise((resolve) => server.once('listening', resolve));
  const sentByClient = [];
  t.after(async () => {
    for (const socket of server.clients) socket.terminate();
    await new Promise((resolve) => server.close(resolve));
  });
  server.on('connection', (socket) => {
    socket.on('message', (data) => sentByClient.push(data.toString()));
    socket.send(JSON.stringify({
      type: 'candidate',
      from: 'peer',
      to: 'client',
      candidate: 'candidate:fixture',
      mid: '0',
    }));
  });
  const address = server.address();
  assert.equal(typeof address, 'object');
  const received = [];
  const client = await WebSocketSignallingClient.connect(
    `ws://127.0.0.1:${address.port}/{id}`,
    'client',
    (message) => { received.push(message); },
  );
  assert.equal(isOk(client), true);
  for (let attempt = 0; attempt < 100 && received.length === 0; ++attempt) {
    await new Promise((resolve) => setTimeout(resolve, 5));
  }
  assert.equal(received.length, 1);
  assert.equal(received[0].candidate, 'candidate:fixture');

  const sent = client.send(new SignallingMessage({
    type: SignallingMessageType.CANDIDATE,
    sender: 'client',
    recipient: 'peer',
    candidate: 'candidate:client',
    mid: '0',
  }));
  assert.equal(isOk(sent), true);
  for (let attempt = 0; attempt < 100 && sentByClient.length === 0; ++attempt) {
    await new Promise((resolve) => setTimeout(resolve, 5));
  }
  assert.equal(sentByClient.length, 1);
  assert.match(sentByClient[0], /candidate:client/);
  assert.equal(isOk(client.close()), true);
});

test('signalling descriptions round-trip every supported SDP type', () => {
  for (const descriptionType of ['offer', 'answer', 'pranswer', 'rollback']) {
    const original = new SignallingMessage({
      type: SignallingMessageType.DESCRIPTION,
      sender: 'client',
      recipient: 'peer',
      description: `fixture-${descriptionType}`,
      descriptionType,
    });
    const encoded = original.toJson();
    assert.equal(isOk(encoded), true);
    const decoded = SignallingMessage.fromJson(encoded);
    assert.equal(isOk(decoded), true);
    assert.equal(decoded.descriptionType, descriptionType);
  }
});

test('WebRTC client configuration is browser-native and exception-safe', () => {
  const turn = TurnServer.fromString(
    'turns:user:password@[2001:db8::1]:5349?transport=tcp',
  );
  assert.equal(isOk(turn), true);
  assert.equal(turn.relayType, TurnRelayType.TCP);
  const ice = turn.toIceServer();
  assert.equal(isOk(ice), true);
  assert.equal(ice.urls, 'turn:[2001:db8::1]:5349?transport=tcp');

  const signalling = {
    send: () => okStatus(),
    setOnMessage: () => okStatus(),
    close: () => okStatus(),
    getIdentity: () => 'client',
    isConnected: () => true,
    getStatus: () => okStatus(),
  };
  const stream = WebRtcWireStream.createClient('peer', signalling, {
    peerConnectionFactory: () => { throw new TypeError('factory exploded'); },
  });
  assert.equal(isOk(stream), false);
  assert.equal(stream.code, StatusCode.UNKNOWN);
});

test('WebRTC validates TURN fields and fulfilled RTC factory values', () => {
  const signalling = {
    send: () => okStatus(),
    setOnMessage: () => okStatus(),
    close: () => okStatus(),
    getIdentity: () => 'client',
    isConnected: () => true,
    getStatus: () => okStatus(),
  };

  const malformedPeer = WebRtcWireStream.createClient('peer', signalling, {
    peerConnectionFactory: () => ({ unexpected: true }),
  });
  assert.equal(isOk(malformedPeer), false);
  assert.equal(malformedPeer.code, StatusCode.INVALID_ARGUMENT);

  const connectionWithMalformedChannel = {
    createDataChannel: () => ({ unexpected: true }),
    addEventListener() {},
    async createOffer() { return { type: 'offer', sdp: 'unused' }; },
    async setLocalDescription() {},
    async setRemoteDescription() {},
    async addIceCandidate() {},
    close() {},
  };
  const malformedChannel = WebRtcWireStream.createClient('peer', signalling, {
    peerConnectionFactory: () => connectionWithMalformedChannel,
  });
  assert.equal(isOk(malformedChannel), false);
  assert.equal(malformedChannel.code, StatusCode.INVALID_ARGUMENT);

  const hostileTurn = new TurnServer({ hostname: 'turn.example.test' });
  Object.defineProperty(hostileTurn, 'hostname', {
    get() { throw new TypeError('hostile TURN getter'); },
  });
  const turnValidation = hostileTurn.validate();
  assert.equal(isOk(turnValidation), false);
  assert.equal(turnValidation.code, StatusCode.INVALID_ARGUMENT);
  const ice = hostileTurn.toIceServer();
  assert.equal(isOk(ice), false);
  assert.equal(ice.code, StatusCode.INVALID_ARGUMENT);
});

test('WebRTC signalling and bufferedAmount failures settle as statuses', async () => {
  class OpenDataChannel {
    binaryType = '';
    bufferedAmountLowThreshold = 0;
    bufferedAmount = 0;
    readyState = 'open';
    label = 'a11-open-fixture';
    listeners = new Map();

    addEventListener(type, listener) {
      const listeners = this.listeners.get(type) ?? [];
      listeners.push(listener);
      this.listeners.set(type, listeners);
    }

    send() {}
    close() { this.readyState = 'closed'; }
  }

  class OpenPeerConnection {
    connectionState = 'connected';
    localDescription = null;
    channel = new OpenDataChannel();

    createDataChannel() { return this.channel; }
    addEventListener() {}
    async createOffer() { return { type: 'offer', sdp: 'unused' }; }
    async setLocalDescription() {}
    async setRemoteDescription() {}
    async addIceCandidate() {}
    close() { this.connectionState = 'closed'; }
  }

  let onSignal = null;
  const signalling = {
    send: () => okStatus(),
    setOnMessage(callback) { onSignal = callback; return okStatus(); },
    close: () => okStatus(),
    getIdentity: () => 'client',
    isConnected: () => true,
    getStatus: () => okStatus(),
  };
  let connection = new OpenPeerConnection();
  let stream = WebRtcWireStream.createClient('peer', signalling, {
    peerConnectionFactory: () => connection,
    desiredChannels: 1,
  });
  assert.equal(isOk(stream), true);
  const malformedSignal = await onSignal({ unexpected: true });
  assert.equal(isOk(malformedSignal), false);
  assert.equal(malformedSignal.code, StatusCode.INVALID_ARGUMENT);
  const failedStart = await stream.start();
  assert.equal(isOk(failedStart), false);
  assert.equal(failedStart.code, StatusCode.INVALID_ARGUMENT);

  connection = new OpenPeerConnection();
  stream = WebRtcWireStream.createClient('peer', signalling, {
    peerConnectionFactory: () => connection,
    desiredChannels: 1,
  });
  assert.equal(isOk(stream), true);
  assert.equal(isOk(await stream.start()), true);
  connection.channel.bufferedAmount = Number.NaN;
  assert.equal(isOk(stream.send(messageWithByte('invalid-buffer', 67))), true);
  const terminal = await stream.wait();
  assert.equal(isOk(terminal), false);
  assert.equal(terminal.code, StatusCode.INTERNAL);
});

test('WebRTC client completes offer, answer, ICE, and data-channel startup', async () => {
  class FakeDataChannel {
    binaryType = '';
    bufferedAmountLowThreshold = 0;
    bufferedAmount = 0;
    readyState = 'connecting';
    label = 'a11-fixture';
    sent = [];
    listeners = new Map();

    addEventListener(type, listener) {
      const listeners = this.listeners.get(type) ?? [];
      listeners.push(listener);
      this.listeners.set(type, listeners);
    }

    emit(type, event = {}) {
      for (const listener of this.listeners.get(type) ?? []) listener(event);
    }

    send(data) {
      this.sent.push(new Uint8Array(data));
    }

    close() {
      if (this.readyState === 'closed') return;
      this.readyState = 'closed';
      this.emit('close');
    }
  }

  class FakePeerConnection {
    connectionState = 'new';
    localDescription = null;
    remoteDescription = null;
    candidates = [];
    listeners = new Map();
    channel = new FakeDataChannel();

    createDataChannel() { return this.channel; }

    addEventListener(type, listener) {
      const listeners = this.listeners.get(type) ?? [];
      listeners.push(listener);
      this.listeners.set(type, listeners);
    }

    emit(type, event = {}) {
      for (const listener of this.listeners.get(type) ?? []) listener(event);
    }

    async createOffer() { return { type: 'offer', sdp: 'fixture-offer' }; }

    async setLocalDescription(description) {
      this.localDescription = description;
      this.emit('icecandidate', {
        candidate: { candidate: 'candidate:client', sdpMid: '0' },
      });
    }

    async setRemoteDescription(description) {
      this.remoteDescription = description;
      this.connectionState = 'connected';
      this.channel.readyState = 'open';
      queueMicrotask(() => this.channel.emit('open'));
    }

    async addIceCandidate(candidate) { this.candidates.push(candidate); }

    close() {
      this.connectionState = 'closed';
    }
  }

  const connection = new FakePeerConnection();
  const sentSignals = [];
  let onSignal = null;
  let signallingClosed = false;
  const signalling = {
    send(message) {
      sentSignals.push(message);
      if (
        message.type === SignallingMessageType.DESCRIPTION &&
        message.descriptionType === 'offer'
      ) {
        queueMicrotask(() => void onSignal?.(new SignallingMessage({
          type: SignallingMessageType.DESCRIPTION,
          sender: 'peer',
          recipient: 'client',
          description: 'fixture-answer',
          descriptionType: 'answer',
        })));
      }
      return okStatus();
    },
    setOnMessage(callback) { onSignal = callback; return okStatus(); },
    close() { signallingClosed = true; return okStatus(); },
    getIdentity: () => 'client',
    isConnected: () => !signallingClosed,
    getStatus: () => okStatus(),
  };

  const stream = WebRtcWireStream.createClient('peer', signalling, {
    peerConnectionFactory: () => connection,
  });
  assert.equal(isOk(stream), true);
  assert.equal(isOk(await stream.start()), true);
  assert.equal(connection.localDescription.sdp, 'fixture-offer');
  assert.equal(connection.remoteDescription.sdp, 'fixture-answer');
  assert.ok(sentSignals.some(
    (message) => message.type === SignallingMessageType.CANDIDATE,
  ));
  assert.ok(sentSignals.some(
    (message) => message.descriptionType === 'offer',
  ));

  assert.equal(isOk(stream.send(messageWithByte('rtc', 66))), true);
  await new Promise((resolve) => setTimeout(resolve, 0));
  assert.ok(connection.channel.sent.length > 0);
  assert.equal(isOk(stream.abort(unavailableError('fixture complete'))), true);
  assert.equal(isOk(await stream.wait()), false);
  assert.equal(signallingClosed, true);
});

test('transport option and signalling getters cannot escape as exceptions', async () => {
  const hostileOptions = Object.defineProperty({}, 'maxMessageSize', {
    get() { throw new TypeError('hostile option getter'); },
  });
  const signallingResult = await WebSocketSignallingClient.connect(
    'ws://127.0.0.1:1/{id}',
    'client',
    undefined,
    hostileOptions,
  );
  assert.equal(isOk(signallingResult), false);
  assert.equal(signallingResult.code, StatusCode.UNKNOWN);

  const hostileSignalling = {
    send: () => okStatus(),
    setOnMessage: () => okStatus(),
    close: () => okStatus(),
    getIdentity: () => 'client',
    isConnected: () => { throw new TypeError('hostile state getter'); },
    getStatus: () => okStatus(),
  };
  const rtcResult = WebRtcWireStream.createClient('peer', hostileSignalling, {
    peerConnectionFactory: () => { throw new Error('must not be reached'); },
  });
  assert.equal(isOk(rtcResult), false);
  assert.equal(rtcResult.code, StatusCode.UNKNOWN);
});

test('client transports convert malformed fulfilled factory values to statuses', async () => {
  const websocket = WebSocketWireStream.createClient(
    'ws://example.test',
    {},
    { webSocketFactory: async () => ({ unexpected: true }) },
  );
  assert.equal(isOk(websocket), true);
  const websocketStarted = await websocket.start();
  assert.equal(isOk(websocketStarted), false);
  assert.equal(websocketStarted.code, StatusCode.INVALID_ARGUMENT);

  const sse = HttpSseClientWireStream.create('https://example.test', {
    fetch: async () => ({ unexpected: true }),
  });
  assert.equal(isOk(sse), true);
  const sseStarted = await sse.start();
  assert.equal(isOk(sseStarted), false);
  assert.equal(sseStarted.code, StatusCode.DATA_LOSS);
});

test('HTTP SSE converts malformed fulfilled body reads to terminal status', async () => {
  let resolveRead;
  const readResult = new Promise((resolve) => { resolveRead = resolve; });
  const sse = HttpSseClientWireStream.create('https://example.test', {
    fetch: async () => ({
      ok: true,
      status: 200,
      headers: new Headers({
        'content-type': 'text/event-stream',
        'x-a11-stream-id': 'malformed-reader',
      }),
      body: {
        getReader: () => ({
          read: () => readResult,
          releaseLock() {},
        }),
      },
    }),
  });
  assert.equal(isOk(sse), true);
  assert.equal(isOk(await sse.start()), true);
  resolveRead({ unexpected: true });
  const terminal = await sse.wait();
  assert.equal(isOk(terminal), false);
  assert.equal(terminal.code, StatusCode.DATA_LOSS);
});
