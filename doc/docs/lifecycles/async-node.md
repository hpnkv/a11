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
  | finalize(...)                       -- or put(..., final=true)
  v
final sequence recorded
  |
  | the same finalize(), unless close=False
  v
closed (OK)

open / writing / final recorded -- abort_with_status --> closed (non-OK)
open / writing ------------------ close() ------------> closed without finality
```

`finalize()` combines marking the final sequence and closing the stream.
Producers generating streams where individual chunks lack a natural terminal boundary
can use `close()` to signal stream completion without writing a final chunk,
though readers expecting a unary value with `consume()` require an explicit final sequence.

## Producer State Transitions

Producer operations follow four explicit milestones:

| Transition | Semantics |
| --- | --- |
| **Write Admitted** | The bounded writer accepts the fragment into its processing queue |
| **Write Confirmed** | The backing store writes the fragment to persistent/in-memory storage |
| **Final Sequence Recorded** | Readers receive the logical end-of-stream boundary |
| **Writes Closed** | The store rejects further writes and waiting readers complete |

`finalize()` records the final sequence and closes writes in a single operation:

- **Finality**: Declares the logical end of data. Readers recognize the final item immediately, even before the store closes.
- **Closure**: Closes the writer, flushes pending queues, and informs attached wire transports.

```python
# Finalize and close immediately
await node.finalize("final_result")

# Or mark finality now and close later across batch operations
await node.finalize("final_result", close=False)
await node.close()
```

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

## 3. Finalise: declare the logical final data

`finalize()` establishes the final sequence. It has three shapes, and which one
a producer wants follows from what it knows:

```python
await answer.finalize()             # the last value already went out with put()
await result.finalize(value)        # this value is the last one
await result.finalize(value, seq=7) # ...and it belongs at this sequence
```

With no value it writes a null terminator: a marker carrying no application
value, which only ends the logical sequence. That is the form for a streaming
port whose last value is not known until it has been written, and for a unary
port a caller has nothing to put on. With a value it uses one chunk instead of
separate value and terminator chunks when the producer knows which value is
last.

!!! important
    A final fragment does **not** close the writer or backing store, and closing
    does **not** create a final fragment. They stay two pieces of state:
    `finalize()` writes one and requests the other, and
    `finalize(..., close=False)` writes the first alone — for a producer that
    marks each of its nodes as it finishes and closes them together later.

Once a store has a final sequence, a conflicting second final sequence or a
fragment beyond it is invalid. This lets out-of-order transports fill earlier
gaps while preserving one unambiguous end. A value written with `final=true`
has already recorded finality; its producer must call `close()`, not another
`finalize()`.

## 4. Close writes

Closure is the second half of `finalize()`, and `close()` on its own when there
is no final fragment to write. Either way it waits for queued writes and closes
the backing store with an OK status. It is the producer's resource and
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

Closing does **not** append a final fragment, which is why `close()` is the
narrow call. Reach for it in two situations:

- the last write already carried `final=true`, so finality is recorded and only
  the store lifecycle is left;
- nothing that arrived can be identified as the last thing — a log, where which
  line is final is not tractable but "no more are coming" is. A reader gets a
  clean end; a reader calling `consume()` gets `FAILED_PRECONDITION`, because a
  clean close is not proof that a whole value was complete.

```python
await log.put(line)
await log.close()          # no final fragment exists, and none can be invented
```

### Asynchronous Completion and Waiting

By default, `finalize()` returns once the final chunk is admitted to the writer queue, allowing the writer's background pump to finish writing and closing while execution proceeds.

To block until the final chunk is confirmed and the store is closed, pass `wait=True`:

```python
await output.finalize(result, wait=True)   # Confirmed by store and closed
```

Use `wait=True` when executing during application shutdown or inside test assertions where immediate store consistency is required. `close()` always awaits store completion.

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
followed by a null-final marker — the two spellings `finalize(value)` and
`put(value)` + `finalize()` produce. A clean store close without either form is
not proof that a whole value was complete.

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
handler calls `finalize()` when the last token or object is complete. Action
cleanup closes writers the handler left open, and failure cleanup aborts them,
but cleanup cannot infer a final application value.
