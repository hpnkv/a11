# WireStream lifecycle

A `WireStream` is the bidirectional message channel beneath sessions, remote
actions, and node mirroring. The transport may be in-process, WebSocket, HTTP
SSE, WebRTC, or an application implementation; the lifecycle above it is the
same.

The important mental model is **two independently closing directions**. Your
endpoint can stop sending and continue receiving, and the peer can do the same.
The stream is fully done only when both directions have ended, or when an error
aborts the exchange.

See the [Python transport API](../api/net.md), the
[TypeScript reference](../typescript.md), or the
[C++ runtime reference](../cpp.md) for language-specific signatures.

## State overview

```text
created
   |
   | start (initiator) or accept (responder), exactly once
   v
active <------------------------------------+
   |                                        |
   | half-close local output                | peer output remains active
   v                                        |
local half-close queued                     |
   |                                        |
   | drain outgoing messages                |
   v                                        |
local output delivered ---------------------+
   |
   | peer half-closes (before or after the local half-close)
   v
done

created / active / half-closed -- abort, deadline, transport error --> failed
```

The drawing shows one common order. The peer may half-close first, in which case
your message callback observes the end of inbound data while your outbound side
remains writable.

| State | What the endpoint may do | What moves it forward |
| --- | --- | --- |
| Created | Configure limits, headers, callbacks, and deadline | `start` or `accept` |
| Active | Send messages and receive callbacks | Either side half-closes, or an error occurs |
| Local half-close queued | Continue receiving; do not send more data | The transport drains the queued terminal message |
| Local output delivered | Continue receiving | Peer half-close |
| Done | Inspect status and peer trailers; release resources | Terminal |
| Failed | Inspect the structured non-OK status | Terminal |

## 1. Choose one endpoint role

Call `start` on the endpoint that initiates the connection and `accept` on the
endpoint responding to it. Each endpoint is driven exactly once. Calling both,
or calling either method twice, is an invalid lifecycle transition.

Starting installs two callbacks:

- the message callback receives each `WireMessage`; a missing value
  (`None`, `null`, or `std::nullopt`) means **the peer half-closed**;
- the done callback runs once after full clean completion or failure.

The runtime awaits the message callback before delivering another message on
that endpoint. A slow consumer therefore applies backpressure instead of
allowing an unbounded callback backlog.

`start` and `accept` are startup barriers, not universally completion barriers.
In the C++ runtime they resolve after the channel handshake; TypeScript follows
the same model and provides `wait()` for terminal completion. In callback-based
Python/C++ code, signal your own event from `on_done` when a caller must await
the complete stream.

## 2. Sending is admission, not delivery

`send(message)` validates and queues a message. A successful return means the
endpoint accepted it into its outbound state machine; it does not mean the peer
has observed it.

That distinction lets a session batch independent node fragments and action
messages without blocking on every network write. It also means teardown needs
an explicit synchronization step: half-close and then drain.

WireStream itself does not promise global message order. A transport may deliver
messages in a different order from admission, and several streams in one
session advance independently. Ordered agent data travels as sequenced
`NodeFragment` values; the receiving `ChunkStore` reconstructs their logical
order.

## 3. Half-close one direction

Call `half_close` (`halfClose` in TypeScript) when this endpoint has queued its
last outbound message. Optional trailers carry end-of-exchange metadata. After
this transition:

- new sends are rejected;
- already queued messages and the half-close marker remain eligible for
  delivery;
- inbound messages continue normally;
- peer trailers become available after its half-close arrives.

Half-close is deliberately not a full close. A common request/response exchange
has the client half-close after its request, then continue receiving streamed
output until the service half-closes its response direction.

## 4. Drain the local direction

After half-close, await `drain_outgoing_messages`
(`drainOutgoingMessages`). It resolves when the messages admitted before the
half-close, including the terminal marker, have been handed through the
transport's buffered output path.

Draining has a precondition: the endpoint must already be half-closed. It does
not decide that sending is finished for you. In Python, the WireStream async
context manager calls the drain method on exit, so call `half_close()` inside
the block first.

This is a local delivery barrier, not full exchange completion. The peer may
still be producing data, and its callbacks may continue after your drain
finishes.

## 5. Observe peer closure and full completion

The peer half-close is delivered to the message callback as a missing message.
All messages accepted ahead of that terminal event are delivered before it. At
that point the endpoint can inspect the peer's trailers, but full completion
still waits for its own outbound direction to finish too.

Once both directions have ended, the done callback runs. TypeScript callers may
also await `wait()`. That terminal barrier is the right time to:

- inspect `get_status` / `getStatus`;
- read peer trailers;
- discard transport-specific handles;
- remove the stream from a session or connection registry.

## Abort and deadline paths

`abort(non_ok_status)` ends the exchange with a structured reason. Use it when
continuing could expose partial or misleading agent output: malformed wire
data, an application callback failure, a cancelled action tree, or an exhausted
resource bound. An abort discards normal pending work; do not expect a normal
drain afterward.

Transport errors, message-delivery timeouts, and absolute deadlines enter the
same terminal failure path. The first authoritative failure becomes the stream
status and is propagated to the peer when the transport can still communicate.
The done callback still runs, allowing session bookkeeping to release the
stream.

## What should I await?

| Operation | What completion means |
| --- | --- |
| `start` / `accept` | The endpoint is configured and its transport handshake completed |
| message callback | The application has finished consuming one inbound event |
| `drain_outgoing_messages` | Local messages queued before half-close reached the transport delivery barrier |
| TypeScript `wait()` / done callback | Both directions ended, or the stream failed |

For most applications, a [Session lifecycle](session.md) owns these transitions.
Drive a WireStream directly when implementing a transport, writing a focused
integration test, or building a protocol below Session.
