# Session lifecycle

A `Session` is the connection-scoped runtime around one or more WireStreams. It
owns the node namespace, action registry, active action accounting, transport
buffers, and connection deadlines that make a remote agent exchange coherent.

Applications normally create a Session, attach at least one stream, and let the
session route messages. Calling handlers directly bypasses the limits,
cancellation, and status propagation described here.

See the [Python Session API](../api/service.md),
[transport API](../api/net.md), [TypeScript reference](../typescript.md), and
[C++ runtime reference](../cpp.md) for language-specific entry points.

## State overview

```text
open
  |  add streams, send messages, dispatch actions
  |
  +-- half_close / peer clean close --> closing
  |                                      |
  |                                      | all streams fully terminate
  |                                      v
  |                                    done
  |
  +-- abort / peer abort / deadline --> aborted
                                         |
                                         | all stream state removed
                                         v
                                       done
```

“Closed” and “done” are intentionally different:

- `is_closed` / `isClosed` means no new session work should begin because
  either endpoint has started shutdown;
- `is_done` / `isDone` means every attached stream has completed and the
  session has released its connection state.

Await `done` / `wait_done`, not merely `is_closed`, before destroying resources
that callbacks or stream pumps may still use.

## What a Session owns

| Resource | Role in an agent application |
| --- | --- |
| NodeMap | Resolves node ids in incoming fragments and action port mappings |
| ActionRegistry | Resolves inbound action names to schemas and local handlers |
| WireStreams | Carry multiplexed action messages, fragments, and session trailers |
| Active actions | Support cancellation, shutdown, and duplicate-id checks |
| Root/nested limiters | Bound top-level calls separately from child tool/action calls |
| Buffer limits | Bound messages and bytes per stream and across the connection |
| Timers | Enforce the absolute deadline and the no-stream grace period |

You may inject a NodeMap or ActionRegistry before work starts. The setters also
rebind active actions, but they do not migrate fragments already stored in the
old NodeMap, and a registry change can make different phases resolve different
registrations. Configure both before attaching streams or starting actions
when possible; changing either mid-flight can split the connection's state.

## 1. Create the open session

Session creation validates ids, headers, byte/message limits, root and nested
action concurrency, no-stream timeout, and deadline. It does not by itself open
a network connection.

Session headers describe the connection as a whole. They are copied,
case-normalized, and carried in terminal half-close or abort metadata; they are
not copied onto actions. The runtime reserves `x-a11-*` names for protocol
metadata. Per-call values belong on Action headers, which can be forwarded into
nested work.

An open session with no streams starts its no-stream timer. This catches an
agent connection that was constructed but never attached, without keeping it
alive indefinitely.

## 2. Attach and drive streams

`add_stream` / `addStream` records the transport id, installs the session's
message/done callbacks, and calls exactly one endpoint method:

- **start mode** for the initiating/client side;
- **accept mode** for the responding/server side.

The startup result covers attaching and opening that stream. The session keeps
pumping it afterward. Several streams may belong to one session; an explicit
stream id routes a send to one of them, while an omitted id lets the runtime
choose an active stream.

Each stream has independent pending-message counters, but total session limits
apply across all of them. When a callback or dispatcher is slow, the stream
pump waits for capacity.

## 3. Dispatch each WireMessage

One WireMessage may contain action control messages and node fragments. With
the default stream callback, the session validates the encoded size and
dispatches its elements into shared runtime state:

1. action messages establish, cancel, or update action lifecycles;
2. node fragments are applied to the NodeMap by id and sequence;

Supplying a custom `on_stream_message` / `onStreamMessage` callback replaces
that default dispatcher. This is useful for gateways and protocol inspection,
but the callback must explicitly call `dispatch_wire_message` /
`dispatchWireMessage` if it still wants ordinary action/node routing. Messages
for one stream enter the callback through a serialized pump.

Action dispatch and fragment writes retain structured failures: if one element
is invalid, diagnostics identify its element kind and index.

Incoming action calls resolve their schema and handler through the registry.
Before a handler runs, it acquires either a root or nested concurrency slot.
Nested calls therefore cannot starve the connection's externally initiated
work, and a burst of tool calls cannot create unlimited tasks.

## 4. Track action work

The session indexes active actions by call id. Tracking supports:

- rejecting duplicate active ids;
- finding or cancelling one call;
- cancelling every call during abort;
- awaiting active work during application-controlled shutdown;
- applying shared root/nested concurrency limits.

An Action removes itself after its terminal cleanup. Session half-close prevents
new outbound messages and dispatches, but work already unwinding may still own
nodes, callbacks, or stream state. This is another reason full session
completion is a separate barrier.

## 5. Clean half-close

`half_close` / `halfClose` moves an open session into closing. The runtime:

1. records an OK session status;
2. packs that status under `SESSION_STATUS_HEADER` with the session trailers;
3. half-closes each active WireStream;
4. rejects new session messages, streams, and actions;
5. continues receiving and cleaning up work already admitted.

This transition is idempotent. It does not mean every stream has drained yet.
Each WireStream still has its own local half-close and remote half-close, and
the session remains not-done until those stream state entries are removed.

A peer cleanly ending its session reports the same OK trailer. The local
session becomes closed, delivers the relevant remote half-close callback, and
waits for its remaining stream direction to settle.

## 6. Abort with a structured failure

`abort(non_ok_status)` enters the failure path. It is appropriate when the
connection cannot safely continue: an action/session protocol violation,
deadline expiry, resource exhaustion, or an unrecoverable application failure.

Abort performs broader cleanup than half-close:

- root and nested action waiters are cancelled;
- active actions receive cooperative cancellation;
- pending inbound message buffers stop accepting work;
- the session status is serialized into trailers;
- streams receive the reserved session-abort signal or are directly aborted if
  that signal cannot be sent.

The stream-level abort status identifies the transport shutdown, while the
session trailer retains the original application/session failure. This lets the
peer distinguish “the session failed with X” from the mechanical fact that its
streams were terminated.

## 7. Reach full done

The done transition occurs only after shutdown has begun and every attached
stream has finished its callback/drain/cleanup path. At that point:

- `is_done` / `isDone` becomes true;
- `done` / `wait_done` resolves;
- `get_status` / `getStatus` is the authoritative terminal session status;
- no stream callback can re-enter the session.

The Python `done` property is event-shaped, so `await session.done.wait()` is
the usual connection-lifetime barrier. TypeScript returns the terminal Status
from `await session.done()`.

## Automatic terminal paths

### No-stream timeout

If an open session has no attached streams for its configured grace period, it
cleanly half-closes. Attaching another stream before the timer fires cancels the
pending transition. This handles transient gaps while ensuring abandoned
session objects do not remain open forever.

### Absolute deadline

When the session deadline expires, the runtime aborts with
`DEADLINE_EXCEEDED`. Updating the deadline reschedules enforcement; clearing it
uses an infinite/no-deadline value. A per-call receive deadline is different:
it only bounds that wait and does not alter the connection lifetime.

### Stream failure or peer abort

A normal stream failure affects that stream and its callbacks. The reserved
session-abort signal, however, moves the whole session into its aborted phase,
stops other stream pumps, and cancels actions so no sibling transport continues
an exchange the peer considers failed.

## Pull-style reception

`SessionWithRecv` replaces application stream callbacks with a bounded pull
queue. Use `receive()` for a single multiplexed message loop or
`receive_with_stream_id()` / `receiveWithStreamId()` when routing depends on the
source transport. A missing result means the session is fully finished; a
non-OK status reports abort/deadline failure.

Pull reception replaces automatic dispatch. The receive loop can
inspect, proxy, or explicitly pass each result to `dispatch_wire_message` /
`dispatchWireMessage`. This makes ownership visible and avoids applying the
same action or node fragment twice.

## What should I await?

| Operation | What completion means |
| --- | --- |
| `add_stream` | That stream's startup handshake completed |
| action dispatch | The message was validated and admitted to its action lifecycle |
| `await_all_actions` | Currently tracked handlers reached terminal cleanup |
| `receive` | One inbound message arrived, the session ended, or the wait failed |
| `done` / `wait_done` | Shutdown began and all stream state is gone |

The action-specific transitions within this connection are described in the
[Action lifecycle](action.md).
