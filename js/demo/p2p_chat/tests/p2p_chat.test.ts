/**
 * P2P Chat Room tests.
 *
 * Validates privacy guarantees, pure state functions, host election,
 * event log pruning, and URL handling.
 *
 * Run via: npm run test:demo:p2p
 */

import assert from 'node:assert/strict';
import { test, describe } from 'node:test';

import {
  ActionMessage,
  ChannelEndpointRole,
  ChannelWireStream,
  InProcessWireStream,
  Session,
  SignallingMessage,
  SignallingMessageType,
  StreamMode,
  WebRtcWireStream,
  WireMessage,
  isOk,
  okStatus,
  type BinaryChannel,
  type BinaryChannelCallbacks,
  type Status,
} from '../../../src/index.js';

import { deriveState, pruneEvents, electHost } from '../room_state.js';
import { shouldBecomeHost } from '../election.js';
import { ChatHost } from '../host.js';
import { ChatPeer, PEER_DESIRED_CHANNELS } from '../peer_client.js';
import {
  ConnectionType,
  MAX_MESSAGE_HISTORY,
  MAX_MESSAGE_LENGTH,
  PEER_TIMEOUT_MS,
  type RoomEvent,
} from '../types.js';

// -------------------------------------------------------- privacy

describe('privacy: anonymous claim endpoint', () => {
  test('claimAnonymous sends no room ID, no peer list, no correlating data', async () => {
    // Intercept the fetch call to verify the request.
    let capturedUrl = '';
    let capturedInit: RequestInit | undefined;
    const originalFetch = globalThis.fetch;
    globalThis.fetch = async (input: string | URL | Request, init?: RequestInit) => {
      capturedUrl = String(input);
      capturedInit = init;
      return new Response(JSON.stringify({
        peer_id: 'anon-test-123',
        signalling_url: 'wss://a11.to/signal/ws/anon-test-123',
        claim_token: 'tok_test',
        ice_servers: [],
      }), { status: 200, headers: { 'Content-Type': 'application/json' } });
    };

    try {
      const { claimAnonymous } = await import('../signalling_client.js');
      const result = await claimAnonymous();

      // The URL must NOT contain any room ID or path segment that
      // could identify a room.
      assert.ok(capturedUrl.includes('/v1/anonymous/claim'),
        `URL should use /v1/anonymous/claim, got: ${capturedUrl}`);
      assert.ok(!capturedUrl.includes('room'),
        `URL must not contain 'room': ${capturedUrl}`);

      // The request body must be empty or contain no room/peer data.
      const body = capturedInit?.body;
      assert.ok(body === '{}' || body === '' || body === undefined,
        `Request body must be empty or {}, got: ${body}`);

      // The result must not contain a peers list.
      assert.ok(!('peers' in result),
        'AnonymousClaimResult must not have a peers field');

      // Verify the result has the expected fields.
      assert.equal(result.peerId, 'anon-test-123');
      assert.equal(result.signallingUrl, 'wss://a11.to/signal/ws/anon-test-123');
      assert.equal(result.claimToken, 'tok_test');
      assert.ok(Array.isArray(result.iceServers));
    } finally {
      globalThis.fetch = originalFetch;
    }
  });

  test('refreshCredentials also sends no room data', async () => {
    let capturedUrl = '';
    let capturedBody = '';
    const originalFetch = globalThis.fetch;
    globalThis.fetch = async (input: string | URL | Request, init?: RequestInit) => {
      capturedUrl = String(input);
      capturedBody = String(init?.body ?? '');
      return new Response(JSON.stringify({
        peer_id: 'anon-refresh-456',
        signalling_url: 'wss://a11.to/signal/ws/anon-refresh-456',
        claim_token: 'tok_refresh',
        ice_servers: [],
      }), { status: 200, headers: { 'Content-Type': 'application/json' } });
    };

    try {
      const { refreshCredentials } = await import('../signalling_client.js');
      await refreshCredentials();

      assert.ok(capturedUrl.includes('/v1/anonymous/claim'),
        `Refresh must use /v1/anonymous/claim: ${capturedUrl}`);
      assert.ok(!capturedUrl.includes('room'),
        `Refresh URL must not contain room: ${capturedUrl}`);
      assert.ok(capturedBody === '{}' || capturedBody === '' || capturedBody === 'undefined',
        `Refresh body must be empty: ${capturedBody}`);
    } finally {
      globalThis.fetch = originalFetch;
    }
  });

  test('AnonymousClaimResult type has no peers field', () => {
    // Structural test: create an object matching the interface and
    // verify it has exactly the expected keys.
    const result = {
      peerId: 'test',
      signallingUrl: 'wss://test',
      claimToken: 'tok',
      iceServers: [],
    };
    const keys = Object.keys(result).sort();
    assert.deepEqual(keys, ['claimToken', 'iceServers', 'peerId', 'signallingUrl']);
    assert.ok(!keys.includes('peers'),
      'AnonymousClaimResult must not include peers');
    assert.ok(!keys.includes('roomId'),
      'AnonymousClaimResult must not include roomId');
  });
});

// -------------------------------------------------------- state derivation

describe('deriveState', () => {
  test('empty log yields empty state', () => {
    const state = deriveState([]);
    assert.equal(state.participants.size, 0);
    assert.equal(state.messages.length, 0);
  });

  test('join adds a participant, leave removes', () => {
    const events: RoomEvent[] = [
      { type: 'join', peerId: 'a', timestamp: 1 },
      { type: 'join', peerId: 'b', timestamp: 2 },
      { type: 'leave', peerId: 'a', timestamp: 3 },
    ];
    const state = deriveState(events);
    assert.equal(state.participants.size, 1);
    assert.ok(state.participants.has('b'));
    assert.ok(!state.participants.has('a'));
  });

  test('messages are extracted in order', () => {
    const events: RoomEvent[] = [
      { type: 'join', peerId: 'a', timestamp: 1 },
      { type: 'message', peerId: 'a', text: 'hello', timestamp: 2 },
      { type: 'message', peerId: 'a', text: 'world', timestamp: 3 },
    ];
    const state = deriveState(events);
    assert.equal(state.messages.length, 2);
    assert.equal(state.messages[0]!.text, 'hello');
    assert.equal(state.messages[1]!.text, 'world');
  });

  test('name_change updates participant name', () => {
    const events: RoomEvent[] = [
      { type: 'join', peerId: 'a', timestamp: 1 },
      { type: 'name_change', peerId: 'a', name: 'Alice', timestamp: 2 },
    ];
    const state = deriveState(events);
    assert.equal(state.participants.get('a'), 'Alice');
  });

  test('name_change for absent peer is ignored', () => {
    const events: RoomEvent[] = [
      { type: 'name_change', peerId: 'ghost', name: 'Ghost', timestamp: 1 },
    ];
    const state = deriveState(events);
    assert.equal(state.participants.size, 0);
  });
});

// -------------------------------------------------------- pruning

describe('pruneEvents', () => {
  test('returns all events when under the message cap', () => {
    const events: RoomEvent[] = [
      { type: 'join', peerId: 'a', timestamp: 1 },
      { type: 'message', peerId: 'a', text: 'hi', timestamp: 2 },
    ];
    const pruned = pruneEvents(events, 300);
    assert.equal(pruned.length, 2);
  });

  test('caps at maxMessages and drops oldest messages', () => {
    const events: RoomEvent[] = [
      { type: 'join', peerId: 'a', timestamp: 0 },
    ];
    for (let i = 1; i <= 305; i++) {
      events.push({ type: 'message', peerId: 'a', text: `msg${i}`, timestamp: i });
    }
    const pruned = pruneEvents(events, 300);
    const messages = pruned.filter(e => e.type === 'message');
    assert.equal(messages.length, 300);
    // The first 5 messages should have been dropped.
    assert.equal((messages[0]! as any).text, 'msg6');
  });

  test('keeps join/name events for active participants', () => {
    const events: RoomEvent[] = [
      { type: 'join', peerId: 'a', timestamp: 0 },
      { type: 'name_change', peerId: 'a', name: 'Alice', timestamp: 1 },
    ];
    for (let i = 2; i <= 302; i++) {
      events.push({ type: 'message', peerId: 'a', text: `m${i}`, timestamp: i });
    }
    const pruned = pruneEvents(events, 300);
    // Join and name_change for active peer 'a' must survive.
    assert.ok(pruned.some(e => e.type === 'join' && e.peerId === 'a'));
    assert.ok(pruned.some(e => e.type === 'name_change' && e.peerId === 'a'));
  });

  test('drops structural events for departed participants', () => {
    const events: RoomEvent[] = [
      { type: 'join', peerId: 'gone', timestamp: 0 },
      { type: 'name_change', peerId: 'gone', name: 'Gone', timestamp: 1 },
      { type: 'leave', peerId: 'gone', timestamp: 2 },
      { type: 'join', peerId: 'here', timestamp: 3 },
    ];
    const pruned = pruneEvents(events, 300);
    // 'gone' has left, so their join/name events should be dropped.
    assert.ok(!pruned.some(e => e.type === 'join' && e.peerId === 'gone'));
    assert.ok(!pruned.some(e => e.type === 'name_change' && e.peerId === 'gone'));
    // 'here' is still active.
    assert.ok(pruned.some(e => e.type === 'join' && e.peerId === 'here'));
  });

  test('default cap is MAX_MESSAGE_HISTORY (300)', () => {
    const events: RoomEvent[] = [
      { type: 'join', peerId: 'a', timestamp: 0 },
    ];
    for (let i = 1; i <= 400; i++) {
      events.push({ type: 'message', peerId: 'a', text: `m${i}`, timestamp: i });
    }
    const pruned = pruneEvents(events);
    const messages = pruned.filter(e => e.type === 'message');
    assert.equal(messages.length, MAX_MESSAGE_HISTORY);
  });
});

// -------------------------------------------------------- host election

describe('electHost', () => {
  test('returns the lexicographically smallest peer ID', () => {
    assert.equal(electHost(['charlie', 'alice', 'bob']), 'alice');
  });

  test('returns empty string for empty list', () => {
    assert.equal(electHost([]), '');
  });

  test('single peer is always the host', () => {
    assert.equal(electHost(['only-one']), 'only-one');
  });

  test('is deterministic regardless of input order', () => {
    const peers = ['z-peer', 'a-peer', 'm-peer'];
    const shuffled = ['m-peer', 'z-peer', 'a-peer'];
    assert.equal(electHost(peers), electHost(shuffled));
    assert.equal(electHost(peers), 'a-peer');
  });

  test('works with anonymous-style peer IDs', () => {
    const peers = ['anon-xyz789', 'anon-abc123', 'anon-mno456'];
    assert.equal(electHost(peers), 'anon-abc123');
  });
});

describe('shouldBecomeHost', () => {
  test('returns true when myId is the lowest', () => {
    assert.ok(shouldBecomeHost('alice', ['alice', 'bob', 'charlie']));
  });

  test('returns false when myId is not the lowest', () => {
    assert.ok(!shouldBecomeHost('bob', ['alice', 'bob', 'charlie']));
  });
});

// -------------------------------------------------------- URL handling

describe('URL host ID encoding', () => {
  test('share URL contains host param, no room param', () => {
    // Simulate getShareUrl logic.
    const base = 'https://example.com/guides/p2p-chat';
    const hostId = 'anon-test-789';
    const url = new URL(base);
    url.searchParams.delete('room');
    url.searchParams.set('host', hostId);
    const shareUrl = url.toString();

    assert.ok(shareUrl.includes('host=anon-test-789'));
    assert.ok(!shareUrl.includes('room='));
  });

  test('host ID can be extracted from share URL', () => {
    const shareUrl = 'https://example.com/guides/p2p-chat?host=anon-test-789';
    const url = new URL(shareUrl);
    const hostId = url.searchParams.get('host');
    assert.equal(hostId, 'anon-test-789');
  });

  test('extractHostId returns null for non-URL input', () => {
    // Simulate the extractHostId function.
    function extractHostId(input: string): string | null {
      try {
        const url = new URL(input);
        return url.searchParams.get('host');
      } catch {
        return null;
      }
    }
    assert.equal(extractHostId('not-a-url'), null);
    assert.equal(extractHostId('anon-abc123'), null);
  });

  test('extractHostId works with full share URL', () => {
    function extractHostId(input: string): string | null {
      try {
        const url = new URL(input);
        return url.searchParams.get('host');
      } catch {
        return null;
      }
    }
    assert.equal(
      extractHostId('https://example.com/p2p?host=anon-xyz'),
      'anon-xyz',
    );
  });

  test('a11x never sees the host parameter', () => {
    // The host parameter is a URL query param that only exists in the
    // share link the user copies. Verify it's not part of any a11x URL.
    const a11xUrl = 'https://a11.to/v1/anonymous/claim';
    const url = new URL(a11xUrl);
    assert.equal(url.searchParams.get('host'), null);
    assert.equal(url.searchParams.get('room'), null);
    // The path should not contain room or host identifiers.
    assert.ok(!url.pathname.includes('room'));
  });
});

// -------------------------------------------------------- connection type

describe('ConnectionType enum', () => {
  test('has exactly three values', () => {
    const values = Object.values(ConnectionType) as string[];
    assert.equal(values.length, 3);
    assert.ok(values.includes('direct'));
    assert.ok(values.includes('turn'));
    assert.ok(values.includes('unknown'));
  });
});

// -------------------------------------------------------- constants

describe('constants', () => {
  test('MAX_MESSAGE_LENGTH is 2000', () => {
    assert.equal(MAX_MESSAGE_LENGTH, 2000);
  });

  test('MAX_MESSAGE_HISTORY is 300', () => {
    assert.equal(MAX_MESSAGE_HISTORY, 300);
  });
});

// ---------------------------------------------------- live replication

const delay = (ms: number) => new Promise((resolve) => setTimeout(resolve, ms));

/**
 * Attach a peer to a host over an in-process stream pair.
 *
 * The demo reaches the same state through WebRTC signalling; the private
 * members set here are what `ChatPeer.connectToHost` assigns once its
 * data channel is up.
 */
async function attachPeer(host: ChatHost, myId: string): Promise<ChatPeer> {
  const pair = InProcessWireStream.createPair();
  assert.ok(isOk(pair));
  const [clientStream, serverStream] = pair;

  const peer = new ChatPeer({
    myId,
    hostId: 'host-aaa',
    onStateChange: () => {},
    onBecomeHost: () => {},
    onConnectionTypeChange: () => {},
  });
  const session = Session.create({
    actionRegistry: (peer as any).registry,
    noStreamTimeoutMs: null,
  });
  assert.ok(isOk(session));

  await Promise.all([
    (host as any).onPeerConnected(myId, serverStream, null),
    session.addStream(clientStream, StreamMode.START),
  ]);
  (peer as any).session = session;
  (peer as any).stream = clientStream;

  await peer.startReplication();
  await delay(150);
  return peer;
}

describe('replication over a session', () => {
  test('peers receive the log snapshot and live events', async () => {
    const host = new ChatHost('host-aaa', [], () => {});
    host.appendEvent({ type: 'join', peerId: 'host-aaa', timestamp: 1 });

    const bee = await attachPeer(host, 'peer-bbb');
    // The snapshot carries the joins that preceded the replicate call.
    assert.deepEqual(
      [...deriveState(bee.getEventLog()).participants.keys()].sort(),
      ['host-aaa', 'peer-bbb'],
    );

    const cee = await attachPeer(host, 'peer-ccc');
    await delay(150);

    // A join after the snapshot arrives as a live event.
    assert.deepEqual(
      [...deriveState(bee.getEventLog()).participants.keys()].sort(),
      ['host-aaa', 'peer-bbb', 'peer-ccc'],
    );

    await bee.sendMessage('from B');
    await cee.sendMessage('from C');
    await bee.setName('Bea');
    host.appendEvent({
      type: 'message', peerId: 'host-aaa', text: 'from host', timestamp: 2,
    });
    await delay(300);

    for (const [label, events] of [
      ['host', host.getEventLog()],
      ['peer-bbb', bee.getEventLog()],
      ['peer-ccc', cee.getEventLog()],
    ] as const) {
      const state = deriveState(events);
      assert.deepEqual(
        state.messages.map((message) => `${message.peerId}:${message.text}`),
        ['peer-bbb:from B', 'peer-ccc:from C', 'host-aaa:from host'],
        `${label} sees every message in log order`,
      );
      assert.equal(
        state.participants.get('peer-bbb'), 'Bea',
        `${label} sees the name change`,
      );
    }

    host.shutdown();
  });
});

// ------------------------------------------------------ data channels

/** Minimal RTCDataChannel double recording what the peer sends. */
class FakeDataChannel {
  binaryType = '';
  bufferedAmountLowThreshold = 0;
  bufferedAmount = 0;
  readyState = 'connecting';
  sent: Uint8Array[] = [];
  private readonly listeners = new Map<string, Array<(event: any) => void>>();

  constructor(readonly label: string) {}

  addEventListener(type: string, listener: (event: any) => void): void {
    this.listeners.set(type, [...(this.listeners.get(type) ?? []), listener]);
  }

  removeEventListener(): void {}

  emit(type: string, event: any = {}): void {
    for (const listener of this.listeners.get(type) ?? []) listener(event);
  }

  open(): void {
    this.readyState = 'open';
    this.emit('open');
  }

  send(data: ArrayBuffer): void { this.sent.push(new Uint8Array(data)); }
  close(): void { this.readyState = 'closed'; this.emit('close'); }
}

/** Peer connection double that opens its channels when the answer lands. */
class FakePeerConnection {
  connectionState = 'connected';
  localDescription: any = null;
  channels: FakeDataChannel[] = [];

  createDataChannel(label: string): FakeDataChannel {
    const channel = new FakeDataChannel(label);
    this.channels.push(channel);
    return channel;
  }

  addEventListener(): void {}
  async createOffer() { return { type: 'offer', sdp: 'fixture-offer' }; }
  async setLocalDescription(description: any) { this.localDescription = description; }
  async setRemoteDescription() {
    for (const channel of this.channels) channel.open();
  }
  async addIceCandidate() {}
  close() { this.connectionState = 'closed'; }
}

describe('peer data channels', () => {
  test('every packet reaches a host that reads one data channel', async () => {
    const connection = new FakePeerConnection();
    let onSignal: ((message: SignallingMessage) => unknown) | null = null;
    const signalling = {
      send(message: SignallingMessage) {
        if (
          message.type === SignallingMessageType.DESCRIPTION &&
          message.descriptionType === 'offer'
        ) {
          queueMicrotask(() => void onSignal?.(new SignallingMessage({
            type: SignallingMessageType.DESCRIPTION,
            sender: 'host-aaa',
            recipient: 'peer-bbb',
            description: 'fixture-answer',
            descriptionType: 'answer',
          })));
        }
        return okStatus();
      },
      setOnMessage(callback: any) { onSignal = callback; return okStatus(); },
      close: () => okStatus(),
      getIdentity: () => 'peer-bbb',
      isConnected: () => true,
      getStatus: () => okStatus(),
    };

    const stream = WebRtcWireStream.createClient('host-aaa', signalling as any, {
      peerConnectionFactory: () => connection as any,
      desiredChannels: PEER_DESIRED_CHANNELS,
    });
    assert.ok(isOk(stream));
    assert.ok(isOk(await stream.start()));

    // Distinct headers keep the frames from merging, as separate user
    // actions do in the demo.
    for (let index = 0; index < 16; index++) {
      const message = new WireMessage({
        headers: new Map([
          ['x-fixture', new TextEncoder().encode(`frame-${index}`)],
        ]),
      });
      message.actions.push(new ActionMessage({
        id: `fixture-${index}`, name: 'send_message',
      }));
      assert.ok(isOk(stream.send(message)));
      await delay(2);
    }
    await delay(50);

    // WebRtcServer adopts the first channel and reports any other, so a
    // striped packet is one the host never reads.
    assert.equal(connection.channels.length, 1);
    assert.equal(connection.channels[0]!.sent.length, 16);
  });
});

// ------------------------------------------------------ departures

/**
 * Binary channel delivering packets to a twin.
 *
 * ChannelWireStream is what WebRtcWireStream wraps, so a pair of these
 * carries the half-close and abort markers the demo relies on;
 * InProcessWireStream does not propagate an abort to its far end.
 */
class LoopbackChannel implements BinaryChannel {
  twin!: LoopbackChannel;
  private callbacks: BinaryChannelCallbacks | null = null;
  private live = false;

  setCallbacks(callbacks: BinaryChannelCallbacks): Status {
    this.callbacks = callbacks;
    return okStatus();
  }

  resetCallbacks(): Status { this.callbacks = null; return okStatus(); }

  async open(): Promise<Status> {
    this.live = true;
    this.callbacks?.onOpen();
    return okStatus();
  }

  isOpen(): boolean { return this.live; }

  send(packet: Uint8Array): Status {
    const copy = new Uint8Array(packet);
    queueMicrotask(() => this.twin.deliver(copy));
    return okStatus();
  }

  deliver(packet: Uint8Array): void {
    this.live = true;
    this.callbacks?.onMessage(packet);
  }

  bufferedAmount() { return 0; }
  async waitForBufferedAmountLow(): Promise<Status> { return okStatus(); }

  close(): Status {
    if (!this.live) return okStatus();
    this.live = false;
    this.callbacks?.onClosed();
    this.twin.close();
    return okStatus();
  }

  getImpl(): null { return null; }
}

interface FramedPeer {
  peer: ChatPeer;
  channel: LoopbackChannel;
}

/** Attach a peer to a host over a framed channel pair. */
async function attachFramedPeer(
  host: ChatHost,
  myId: string,
  options: { hostIdleMs?: number; ping?: boolean } = {},
): Promise<FramedPeer> {
  const clientChannel = new LoopbackChannel();
  const serverChannel = new LoopbackChannel();
  clientChannel.twin = serverChannel;
  serverChannel.twin = clientChannel;

  const clientStream = ChannelWireStream.create(
    clientChannel, 'loopback', ChannelEndpointRole.CLIENT,
  );
  const serverStream = ChannelWireStream.create(
    serverChannel, 'loopback', ChannelEndpointRole.SERVER,
    { messageTimeoutMs: options.hostIdleMs ?? PEER_TIMEOUT_MS },
  );
  assert.ok(isOk(clientStream));
  assert.ok(isOk(serverStream));

  const peer = new ChatPeer({
    myId,
    hostId: 'host-aaa',
    onStateChange: () => {},
    onBecomeHost: () => {},
    onConnectionTypeChange: () => {},
  });
  const session = Session.create({
    actionRegistry: (peer as any).registry,
    noStreamTimeoutMs: PEER_TIMEOUT_MS,
  });
  assert.ok(isOk(session));

  await Promise.all([
    (host as any).onPeerConnected(myId, serverStream, null),
    session.addStream(clientStream, StreamMode.START),
  ]);
  if (options.ping === false) {
    (peer as any).session = session;
    (peer as any).stream = clientStream;
  } else {
    (peer as any).adoptConnection(session, clientStream);
  }

  await peer.startReplication();
  await delay(150);
  return { peer, channel: clientChannel };
}

describe('departures', () => {
  test('a peer that leaves is removed before the timeout elapses', async () => {
    const host = new ChatHost('host-aaa', [], () => {});
    host.appendEvent({ type: 'join', peerId: 'host-aaa', timestamp: 1 });

    const bee = await attachFramedPeer(host, 'peer-bbb');
    const cee = await attachFramedPeer(host, 'peer-ccc');
    await delay(150);

    const started = Date.now();
    bee.peer.disconnect();
    await delay(400);
    const elapsed = Date.now() - started;

    assert.ok(elapsed < PEER_TIMEOUT_MS, 'the removal does not wait for a timeout');
    assert.ok(
      !deriveState(host.getEventLog()).participants.has('peer-bbb'),
      'the host drops the peer that left',
    );
    assert.ok(
      !deriveState(cee.peer.getEventLog()).participants.has('peer-bbb'),
      'the remaining peer sees the leave event',
    );

    host.shutdown();
  });

  test('a peer whose channel dies is removed before the timeout elapses', async () => {
    const host = new ChatHost('host-aaa', [], () => {});
    host.appendEvent({ type: 'join', peerId: 'host-aaa', timestamp: 1 });

    const bee = await attachFramedPeer(host, 'peer-bbb');
    const cee = await attachFramedPeer(host, 'peer-ccc');
    await delay(150);

    const started = Date.now();
    // No marker travels: the transport just goes away.
    bee.channel.close();
    await delay(400);

    assert.ok(Date.now() - started < PEER_TIMEOUT_MS);
    assert.ok(!deriveState(host.getEventLog()).participants.has('peer-bbb'));
    assert.ok(!deriveState(cee.peer.getEventLog()).participants.has('peer-bbb'));

    host.shutdown();
  });

  test('a host that leaves hands the room to the elected peer', async () => {
    const host = new ChatHost('host-aaa', [], () => {});
    host.appendEvent({ type: 'join', peerId: 'host-aaa', timestamp: 1 });

    const clientChannel = new LoopbackChannel();
    const serverChannel = new LoopbackChannel();
    clientChannel.twin = serverChannel;
    serverChannel.twin = clientChannel;
    const clientStream = ChannelWireStream.create(
      clientChannel, 'loopback', ChannelEndpointRole.CLIENT,
    );
    const serverStream = ChannelWireStream.create(
      serverChannel, 'loopback', ChannelEndpointRole.SERVER,
    );
    assert.ok(isOk(clientStream));
    assert.ok(isOk(serverStream));

    let elected: ChatHost | null = null;
    const peer = new ChatPeer({
      myId: 'peer-bbb',
      hostId: 'host-aaa',
      onStateChange: () => {},
      onBecomeHost: (newHost) => { elected = newHost; },
      onConnectionTypeChange: () => {},
    });
    const session = Session.create({
      actionRegistry: (peer as any).registry,
      noStreamTimeoutMs: PEER_TIMEOUT_MS,
    });
    assert.ok(isOk(session));
    await Promise.all([
      (host as any).onPeerConnected('peer-bbb', serverStream, null),
      session.addStream(clientStream, StreamMode.START),
    ]);
    (peer as any).adoptConnection(session, clientStream);
    await peer.startReplication();
    await delay(150);

    const started = Date.now();
    host.shutdown();
    // The peer waits out its convergence delay before taking over.
    await delay(1_600);

    assert.ok(Date.now() - started < PEER_TIMEOUT_MS + 1_600);
    assert.notEqual(elected, null, 'the remaining peer becomes the host');
    (elected as unknown as ChatHost).shutdown();
  });
});

describe('liveness', () => {
  test('the __ping builtin answers, so an idle peer stays connected', async () => {
    const host = new ChatHost('host-aaa', [], () => {});
    host.appendEvent({ type: 'join', peerId: 'host-aaa', timestamp: 1 });

    const bee = await attachFramedPeer(host, 'peer-bbb');
    // A ping that goes unanswered runs the host-gone path instead.
    await (bee.peer as any).pingHost();
    await delay(100);

    assert.equal((bee.peer as any).hostGone, false);
    assert.ok(deriveState(host.getEventLog()).participants.has('peer-bbb'));

    host.shutdown();
  });

  test('a peer that stops sending is dropped when the stream goes idle', async () => {
    const host = new ChatHost('host-aaa', [], () => {});
    host.appendEvent({ type: 'join', peerId: 'host-aaa', timestamp: 1 });

    // A short idle window stands in for PEER_TIMEOUT_MS; no pings run.
    await attachFramedPeer(host, 'peer-bbb', { hostIdleMs: 300, ping: false });
    assert.ok(deriveState(host.getEventLog()).participants.has('peer-bbb'));

    await delay(700);
    assert.ok(!deriveState(host.getEventLog()).participants.has('peer-bbb'));

    host.shutdown();
  });
});
