# AsyncNode lifecycle

An `AsyncNode` is the ordered stream behind every action port. A producer writes
serialized chunks into its `ChunkStore`; a reader follows that log at its own
pace; optional WireStreams tee stored fragments to another runtime.

The node combines two related state machines:

- a **writer lifecycle** that admits, confirms, marks finality, and closes;
- a **reader cursor** that waits, yields, reaches an end, or is reset to replay.

Those halves share data but not progress. A writer can run ahead within bounded
buffers, and one reader can be reset without rewinding or reopening the writer.
See the [Python node API](../api/nodes.md), [store API](../api/stores.md), and
[TypeScript reference](../typescript.md) for concrete signatures.

## Writer state overview

```text
open
  |
  | put / put_chunk / put_fragment
  v
writing -- more writes --> writing
  |
  | put_final, put_null_final, or final=true
  v
final sequence recorded
  |
  | drain_and_close
  v
closed (OK)

open / writing / final recorded -- abort_with_status --> closed (non-OK)
open / writing ------------------ drain_and_close --> closed without finality
```

The last arrow is legal and sometimes useful for an iterator, but it does not
invent a complete logical value. Code using `consume()` normally needs an
explicit final fragment before closure.

## The three different promises a producer makes

It is easy to treat “write,” “final,” and “close” as synonyms. They answer
different questions:

| Transition | Promise to the rest of the application |
| --- | --- |
| Write admitted | The bounded writer accepted this fragment for processing |
| Write confirmed | The backing store accepted the fragment; the writer attempted or queued attached sends |
| Final sequence recorded | Readers know which fragment is the logical end of data |
| Writes closed | No more fragments will arrive; blocked readers can settle with the store status |

A reliable producer usually establishes all four in that order.

## 1. Create or obtain the node

Most application code receives nodes as an Action's input or output ports. To
create one directly, give it a stable id and optionally choose:

- a store factory (`LocalChunkStore` for process-local work,
  `RedisChunkStore` for shared persistence);
- reader ordering, offset, buffering, and retention options;
- writer queue and batch sizes;
- a serialization registry for application objects.

The id is part of the distributed data model. Sessions use it to route incoming
`NodeFragment` values into the same logical stream on the peer.

## 2. Admit and confirm writes

Writing converts an application value into a `Chunk`, assigns or preserves a
sequence number, and queues a `NodeFragment` for the store. Bounded admission
prevents a fast model/token producer from growing memory without limit.

Confirmation is the stronger barrier. It resolves after the store accepts the
fragment. The writer attempts or queues attached stream sends while processing
the batch, but confirmation is not an end-to-end delivery acknowledgement and
a later tee failure cannot retract it. Await confirmations to propagate store
backpressure through an agent pipeline.

Python exposes the two stages explicitly:

```python
confirmation = await node.put(token)  # admission
sequence = await confirmation         # backing-store confirmation
```

C++ returns the confirmation `Future` directly. TypeScript's `put` promise
resolves with either the confirmed sequence or a Status.

Attached WireStreams receive sequenced fragments, so serialization and sequence
assignment happen once at the node boundary. The same fragment identity is
offered to the transport, while the WireStream lifecycle governs eventual
delivery.

## 3. Declare the logical final data

There are three equivalent ways to establish a final sequence:

- write the last value with `final=true`;
- call `put_final(value)` / `putFinal(value)`;
- write a normal value and follow it with `put_null_final()` /
  `putNullFinal()`.

The null-final form is useful for a unary port whose one visible value was
already emitted. The marker contains no application value; it only terminates
the logical sequence.

!!! important
    A final fragment does **not** close the writer or backing store. Conversely,
    closing writes does **not** create a final fragment. Final sequence and
    terminal store status are independent pieces of state.

Once a store has a final sequence, a conflicting second final sequence or a
fragment beyond it is invalid. This lets out-of-order transports fill earlier
gaps while preserving one unambiguous end.

## 4. Drain and close writes

`drain_and_close()` / `drainAndClose()` waits for queued writes and closes the
backing store with an OK status. It is the producer's resource and
synchronization barrier:

- future writes are rejected;
- readers waiting for data that can no longer arrive are released;
- writer buffers and attachment bookkeeping can be reclaimed;
- every attached stream is told, so a peer's mirror of the node closes too.

That last point makes closure a shared fact. After the last teed batch the
writer sends one **closure marker**: a status chunk
(`application/x-a11-status`) carrying the metadata attribute `a11-close` and
the OK close status. The receiving runtime does not store it; it applies it to
its own copy of the node, which closes that mirror's write half and releases
its readers. Draining and teeing are synchronised — the close only begins once
every batch has gone out — so the marker is the last thing a peer sees.

If the marker cannot be sent the store still closes, and the send error becomes
the writer's terminal status; like a failed data tee, it cannot revoke
confirmations already returned. An aborting action fans its status out over its
own stream instead.

Closing does **not** append a final fragment. For a complete unary output, use
this sequence:

```python
await output.put_final(result)
await output.drain_and_close()
```

For a visible value followed by an invisible terminator:

```python
await output.put(result)
await output.put_null_final()
await output.drain_and_close()
```

The Python AsyncNode context manager calls `drain_and_close()` on a clean exit,
but it deliberately does not choose finality for you. Put the final marker
inside the block when a whole-value consumer depends on it.

## Reader lifecycle

```text
idle
  |
  | next / async iteration / consume
  v
waiting or prefetching
  |
  | fragment available
  v
yielding -------- next --------> waiting
  |
  | final sequence exhausted or clean closed end reached
  v
end

waiting / yielding -- store closes non-OK --> failed
idle / yielding / end -- reset_reader --> idle at a configured offset
```

A `ChunkStoreReader` owns its cursor and buffering policy. Ordered mode waits for
sequence gaps to fill; arrival-order mode exposes ingestion order. Options can
start at an offset, cap the number of chunks, retain a sticky mimetype, or clear
payloads after consumption.

Choose a read shape that matches the port contract:

- `next()` or `async for` is for a stream of independent values such as tokens,
  progress events, or audio frames;
- `next_chunk()` / `next_fragment()` keeps serialization and routing metadata
  visible;
- `consume()` is for exactly one whole unary value and validates its final
  shape.

`consume()` accepts either a value that is itself final, or one continued value
followed by a null-final marker. A clean store close without either form is not
proof that a whole value was complete.

## Reset and replay

`reset_reader()` replaces the cursor without changing stored fragments or
writer state. Use it to replay an output for another stage, recover from an
application-level parse attempt, or begin at a different offset. On a persistent
store this is a new view over the same log, not a request for the producer to
send data again.

Be cautious with destructive reader options such as popping/clearing payloads:
another reader may retain sequence metadata but no longer be able to recover
the original bytes.

## Failure and cancellation

`abort_with_status(non_ok_status)` closes writes with a structured failure.
Pending confirmations and readers settle with that status, and attached streams
can propagate it to remote peers. Use this path when partial output must not be
mistaken for a valid result.

Cancelling one local wait is different from aborting the node. A timed-out or
cancelled reader may stop waiting while the producer and other readers continue.
Abort only when the shared stream itself has failed.

## How this fits an Action

An [Action lifecycle](action.md) maps each schema port to an AsyncNode. The
handler decides semantic output finality because only application code knows
whether the last token or object is complete. Action cleanup can drain and close
writers, and failure cleanup can abort them, but cleanup cannot safely invent a
final application value.
