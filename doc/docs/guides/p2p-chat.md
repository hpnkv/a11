# P2P Chat Room

A fully decentralised chat room running entirely in the browser. Peers
connect over WebRTC using A11's signalling infrastructure. The room creator
becomes the host — an A11 service exposing actions that other peers call to
send messages, set names, and receive the replicated event log. When the host
leaves, the peer with the lowest ID takes over seamlessly.

**Privacy guarantee:** a11x is performatively not nosy. It hands out anonymous
identities with time-limited TURN credentials and routes signalling messages
between them. It never receives room identifiers, peer lists, or any data that
could correlate which peers belong to the same room. TURN relays only
DTLS-encrypted frames. Logs do not correlate client identities with IP
addresses.

## Try it

Create a room and share the link, or paste a host ID to join an existing room.
The participant list shows each peer's connection type: green for a direct
STUN path, orange for TURN relay.

<link rel="stylesheet" href="../assets/p2p-chat.css">
<div id="p2p-chat" class="p2p-chat">
  <div id="p2p-errors" class="p2p-errors" role="alert" aria-live="polite"></div>
  <div id="p2p-lobby" class="p2p-lobby">
    <h3>P2P Chat Room</h3>
    <div class="p2p-lobby-actions">
      <button id="p2p-create-btn" type="button">Create Room</button>
      <form id="p2p-join-form" class="p2p-join-form">
        <input id="p2p-join-input" placeholder="Host ID or share link" autocomplete="off">
        <button type="submit">Join</button>
      </form>
    </div>
  </div>
  <div id="p2p-room" class="p2p-room">
    <div class="p2p-room-header">
      <span class="p2p-room-label">Host: <code id="p2p-room-id"></code></span>
      <a id="p2p-room-link" class="p2p-room-link" target="_blank"></a>
      <button id="p2p-copy-link" class="p2p-copy-link" type="button">Copy link</button>
      <span id="p2p-status" class="p2p-status"></span>
    </div>
    <div class="p2p-workspace">
      <div class="p2p-chat-area">
        <div id="p2p-messages" class="p2p-messages"></div>
        <form id="p2p-send-form" class="p2p-compose">
          <input id="p2p-message-input" placeholder="Type a message…" autocomplete="off" maxlength="2000">
          <button type="submit">Send</button>
        </form>
      </div>
      <div class="p2p-sidebar">
        <div class="p2p-sidebar-section">
          <h4>Participants</h4>
        </div>
        <div id="p2p-participants" class="p2p-participants"></div>
        <div class="p2p-sidebar-section">
          <h4>Display Name</h4>
          <form id="p2p-name-form" class="p2p-name-form">
            <input id="p2p-name-input" placeholder="Set name" autocomplete="off" maxlength="100">
            <button type="submit">Set</button>
          </form>
        </div>
      </div>
    </div>
  </div>
</div>
<script type="module" src="../assets/p2p-chat.js"></script>

The source is
[`js/demo/p2p_chat/`](https://github.com/hpnkv/a11/tree/main/js/demo/p2p_chat).

## 1. Anonymous identity and signalling

Each peer independently claims an anonymous identity from a11x. The request
carries no room identifier, no peer list — nothing that could tell a11x
which room the peer intends to join or who else is in it:

```
POST https://a11.to/v1/anonymous/claim
→ { peer_id, signalling_url, claim_token, ice_servers }
```

The exchange generates a random identity, issues a short-lived claim
(10-minute TTL) with TURN credentials, and returns a signalling URL. It
does not track rooms, does not maintain peer lists, and does not log any
correlation between the anonymous identity and the caller's IP address.

Room coordination is entirely client-side: the share URL encodes the host's
peer ID (`?host=<peerId>`), so a joiner knows who to WebRTC-connect to
without asking a11x.

Anonymous TURN access is limited to 10 minutes and, where the TURN server
supports it, to 100 KiB/s bandwidth.

## 2. The host as an A11 service

The room creator becomes the initial host. It runs an `ActionRegistry` with
four actions, each implemented as a handler that reads inputs and writes
outputs through `AsyncNode`:

| Action | Inputs | Outputs | Purpose |
|--------|--------|---------|---------|
| `send_message` | `text` (text/plain, unary) | `status` (JSON, unary) | Append a message to the event log |
| `set_name` | `name` (text/plain, unary) | `status` (JSON, unary) | Set the calling peer's display name |
| `replicate` | — | `events` (JSON, streaming) | Stream the full log then live events |
| `get_peers` | — | `peers` (JSON, unary) | List participants with names |

Each incoming peer gets a `Session` with `StreamMode.ACCEPT`. The host
creates the session, adds the stream, and the registry dispatches action
calls automatically.

The library provides the offer side of WebRTC through
`WebRtcWireStream.createClient`. The answer side is the demo's own
`WebRtcServer`, which wraps the peer's data channel as a `ChannelWireStream`
with `ChannelEndpointRole.SERVER`. That adapter reads a single data channel,
so the peer asks for one with `desiredChannels: 1`; the client default of 8
stripes packets round-robin across every open channel.

## 3. Calling actions from peers

A non-host peer connects to the host's signalling identity, establishes a
WebRTC data channel, and wraps it in a `Session` with `StreamMode.START`.
Sending a message is a standard A11 action call:

```typescript
const action = registry.makeAction('send_message', {
  nodeMap: session.getNodeMap(),
  stream: webrtcStream,
  session,
});
await action.call();
const input = await action.getInput('text');
await input.finalize('Hello, room!');
await action.waitForDispatch(10_000);
```

The peer registers the same schemas client-side (without handlers) so the
session's node map and action framing work identically on both ends.

## 4. Event-log replication

The host maintains an ordered event log:

```typescript
type RoomEvent =
  | { type: 'join'; peerId: string; timestamp: number }
  | { type: 'leave'; peerId: string; timestamp: number }
  | { type: 'message'; peerId: string; text: string; timestamp: number }
  | { type: 'name_change'; peerId: string; name: string; timestamp: number };
```

When a peer calls the `replicate` action, the host sends the full log
through the `events` output, then keeps the stream open and pushes new
events as they occur. Every peer holds a local copy sufficient to derive
the complete room state — participants, names, and messages. A joiner
receives the full history before any live events, so the chat scroll is
populated immediately.

The log is capped at 300 messages. Join, leave, and name-change events for
active participants are always retained.

The `replicate` handler stays suspended for as long as the peer is
connected. An action's output ports close when its handler returns, so a
handler that pushes live values holds itself open — here on the peer's
departure and on `action.signal`.

## 5. Departures, liveness, and failover

A peer leaves in one of three ways, and the room converges on all of them
within ten seconds:

| Departure | Signal | Detected in |
|-----------|--------|-------------|
| Tab or window closed | `Session.abort()` from a `pagehide` handler | One round trip |
| Data channel closed or connection failed | `WireStream.wait()` resolves | One round trip |
| Silent peer: no close, no traffic | `messageTimeoutMs` on the stream | `PEER_TIMEOUT_MS`, 10 s |

`pagehide` fires on close, on navigation, and when the page enters the
bfcache, including on mobile Safari where `beforeunload` does not. The
handler calls `ChatHost.shutdown` or `ChatPeer.disconnect`, both of which
abort their sessions with a status. The abort marker travels on the channel
that is still open, so the other side ends the session, cancels the actions
in flight, and broadcasts the leave event.

Both ends race `Session.done()` against `WireStream.wait()`, so a channel
that dies without a marker removes the peer as soon as the stream reports
it, rather than on a session timeout.

Peers ping their host every three seconds with the `__ping` builtin, which
every `ActionRegistry` answers without a registration. The ping and its
answer are activity in both directions, keeping an idle room inside the
ten-second window; a ping that goes unanswered within that window counts as
the host being gone. A tab hidden long enough for the browser to throttle
its timers past ten seconds is dropped from the room, and reloading the page
rejoins from the share URL.

When the host goes, all remaining peers independently compute the same new
host: the peer with the lexicographically smallest ID. No negotiation round
is needed — every peer arrives at the same answer from its participant list.

After a 1-second convergence delay:

- The elected peer instantiates a new `ChatHost` from its local event log
  and starts accepting connections.
- Other peers reconnect to the new host via signalling and call `replicate`
  to resume receiving events.

## 6. Connection monitoring

Each peer polls `RTCPeerConnection.getStats()` every 3 seconds. The selected
candidate pair reveals the active ICE path:

| Candidate type | Classification | Badge |
|----------------|---------------|-------|
| `host`, `srflx`, `prflx` | Direct (STUN) | ● green |
| `relay` | TURN relayed | ◆ orange + warning |

TURN usage is prominently displayed with an orange badge and a "TURN relayed"
label. The badge updates dynamically as ICE candidates change — if a peer
re-signals and switches from relay to direct, the badge turns green.

## 7. Privacy model

The demo is designed so that a11x is structurally unable to correlate peers
into rooms:

| What a11x sees | What a11x does NOT see |
|---|---|
| Anonymous identity claims | Room identifiers |
| Signalling messages between pairs of identities | Which identities form a room |
| TURN relay traffic (DTLS-encrypted) | Message content, names, or chat history |

The `POST /v1/anonymous/claim` request body is empty — no room ID, no peer
list, no metadata. Peers discover each other through the share URL
(`?host=<peerId>`) which never touches a11x. TURN only relays DTLS-encrypted
frames; the relay server cannot inspect the plaintext.
