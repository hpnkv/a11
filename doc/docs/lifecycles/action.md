# Action lifecycle

An `Action` is one schema-described unit of agent work. Its input and output
ports are AsyncNodes, so the same lifecycle covers a local coroutine, a remote
RPC-like call, and a streaming tool invocation whose outputs begin before its
inputs are fully consumed.

An Action is **one-shot**. Configure it first, then choose either local `run` or
remote `call`. Reusing the object for another invocation would mix ids, port
logs, cancellation, and completion status; create a new Action instead.

See the [Python Action API](../api/actions.md),
[AsyncNode lifecycle](async-node.md), [Session lifecycle](session.md), and
[TypeScript reference](../typescript.md) for the concrete APIs.

## State overview

```text
configured
   |
   +-- run (local) --> queued --> slot acquired --> handler running
   |                                                |
   |                                                v
   |                                      outputs/status finishing
   |                                                |
   |                                                v
   |                                               done
   |
   +-- call (remote) --> call queued --> dispatch acknowledged
                                             |
                                             | remote handler + output streams
                                             v
                                      completion status received
                                             |
                                             v
                                            done

configured / queued / running / called -- cancel or failure --> cleanup --> done
```

Remote calls expose two lifecycle milestones:

1. **dispatch status** — did the peer accept and start handling this action?
2. **completion status** — did the action and its output-writer cleanup
   ultimately succeed?

Dispatch gives fast admission feedback; only completion reports the outcome.

## 1. Configure identity and collaborators

An Action starts with an `ActionSchema`. The schema defines:

- the stable action name used by registries and remote dispatch;
- typed input and output port names;
- required/unary port expectations and optional input autofills;
- declared headers and defaults;
- optional output-to-JSON field mappings.

The instance also has a unique call id. A port's node id is derived from the
call id and port name, which keeps two concurrent calls to the same schema from
sharing stream data accidentally.

Before starting, bind the collaborators appropriate to the execution mode:

| Binding | Why the Action needs it |
| --- | --- |
| Handler | Local implementation invoked by `run` |
| NodeMap | Shared namespace that owns port nodes |
| Session | Routing, active-action tracking, limits, and nested context |
| WireStream | Direct remote transport when no Session routes the call |
| ActionRegistry | Resolution of nested action names and local handlers |
| Settings | Stream binding and input/output retention policy |

Identity, schema, and port mapping freeze after `run` or `call` begins. Headers
and collaborators should also be treated as pre-start configuration unless a
method explicitly documents otherwise.

## 2. Materialize ports

Each input/output is an AsyncNode in the bound NodeMap. The Action maps schema
port names to their node ids and can attach selected nodes to a WireStream.

For a remote call, the `ActionMessage` carries those mappings rather than the
port payloads themselves. Payloads travel as sequenced `NodeFragment` values,
which lets the caller stream a large input while the remote handler is already
running and lets outputs return incrementally.

Required ports must map to valid nodes. Unary is a contract about how the
handler should consume a port; it does not turn the underlying node into a
different data structure.

## Local run

### 3. Begin and acquire a concurrency slot

`run()` validates that a handler is bound, atomically chooses local mode, and
tracks the Action in its Session when present. It schedules the handler and
returns; await `wait()` or the Python `done` event for terminal cleanup.

Before invoking application code, a session-bound Action acquires a concurrency
slot. Root actions use the session's root limit. Children made with
`make_nested` / `makeNested` use the nested limit. Waiting for a slot is
cancellable, so shutdown does not leave a queued handler stranded.

### 4. Apply input autofills

An input schema may define fragments to materialize before a local handler
starts. The runtime first verifies that the input node is writable and empty,
writes the fragments exactly as configured, then drains and closes that input.
If data is already present, the action fails instead of choosing between two
writers. Configure a final fragment (or a null-final fragment) when the handler
needs a complete logical value; local autofill does not invent finality.

Remote calls have two schema boundaries. Caller-side autofills travel with the
call, and the last transmitted autofill fragment for each port is made final.
The receiver resolves its own registered schema and applies any receiver-side
autofills before fragments in the same wire message. A receiver-autofilled
input must remain empty, so incoming data for that port is rejected: a caller
cannot inject a value into an input the receiving agent controls.

Autofills make optional/default agent inputs part of the schema. Keep large or
dynamic defaults in application code rather than embedding them in every
dispatch message.

### 5. Run the handler

The handler reads input nodes and writes output nodes. It may interleave those
operations—for example, read a user turn, start a model request, and emit tokens
as they arrive.

The handler owns the semantic data contract of every output. In particular, it
must mark the logical end of one when a consumer needs a complete logical
result, which is what `finalize()` does:

```python
async def summarize(action: a11.Action) -> None:
    request = await action["request"].consume()
    result = await build_summary(request)
    await action["summary"].finalize(result)
```

The handler need not wait for that write: the writer's pump completes it and the
closure after the handler has returned. Action cleanup closes writable outputs
the handler left open, but it does **not** invent final data. Only the handler knows whether its last token, object, or event is a
complete value. On failure, cleanup aborts output nodes so readers receive the
structured error instead of mistaking partial data for success.

### 6. Finish local status and resources

When the handler returns or throws, the runtime:

1. converts the result/exception to a Status;
2. releases its root or nested concurrency slot;
3. drains successful output writers or aborts them on failure;
4. writes the reserved action completion-status output;
5. detaches stream bindings and applies configured node-retention policy;
6. records completion, untracks the Action, and wakes waiters.

A failure during cleanup can become the action failure when the handler itself
succeeded. This ensures a caller does not receive OK if output could not be
persisted or communicated.

## Remote call

### 3. Queue the call message

`call()` chooses remote mode, tracks the Action, gathers input autofills, and
sends a WireMessage containing the `ActionMessage` plus any autofill fragments.
The last autofill fragment for each port is forced final for transport. It
requires either a direct WireStream or a Session. A successful return means the
call was queued locally, not that the peer accepted it.

The schema and port nodes must be configured before this point. Inputs may
continue streaming over their bound nodes after the control message is sent.

### 4. Await dispatch acknowledgement

The peer reports dispatch through the reserved dispatch-status output node.
`wait_for_dispatch()` / `waitForDispatch()` resolves when that status arrives:

- OK means the peer resolved the action and admitted its handler lifecycle;
- non-OK means it rejected the call, for example because the action was absent,
  invalid, unauthorized, or over a runtime boundary.

Dispatch success does not imply that the handler will finish successfully; it
reports only that the work entered the queue.

### 5. Consume outputs while the peer runs

Output nodes are ordinary AsyncNodes, so a caller can begin reading before the
completion status arrives.

Sequence numbers and final markers belong to each output node. The action-level
completion status describes the operation as a whole; it does not replace
per-output finality.

### 6. Await completion

The peer writes the reserved completion-status output after its handler and
output-writer cleanup. On receipt, the local call records completion, detaches
its stream bindings, and is removed from session tracking. Inspecting an output
before `wait()` is valid for streaming; treating the whole call as successful
before `wait()` is not.

If completion arrives before a separate dispatch acknowledgement, the runtime
implicitly settles dispatch so neither waiter remains stuck.

## Nested actions

`make_nested` / `makeNested` creates a child with its own action id and its own
port ids. With I/O propagation enabled (the default), it shares the parent's
NodeMap, stream, and Session; it also shares the registry used to resolve a
child by name. Sharing these collaborators connects storage, routing, and
nested concurrency limits, but it does not copy the parent's port mappings.

By default, headers under the reserved `x-a11-*` prefix flow into nested work.
That keeps deadlines, tracing context, tool policy, and user-log routing
connected across an agent plan. Forward only intended application headers;
credentials and tenant metadata should follow explicit security policy.

A child call is still an independent one-shot Action with its own id, status,
ports, and waiters. “Nested” describes ownership and limits, not inline
execution. Do not assume parent-only cancellation is recursive across every
language: TypeScript children have independent `AbortSignal`s. Explicitly
cancel and await children owned by a handler, or abort the Session when the
whole operation must stop.

## Cancellation

Cancellation is cooperative and idempotent:

- a local handler sees its cancellation signal and registered cancellation
  callbacks;
- a remote call sends A11's reserved cancellation action naming the target id;
- output nodes are eventually aborted with CANCELLED;
- the Action remains in cleanup until `wait()` reaches its terminal barrier.

Calling `cancel()` requests the transition; it does not prove that provider SDK
work or a remote peer has stopped at that exact instant. Handlers should pass
the signal/deadline into model, HTTP, and tool clients wherever those clients
support cancellation.

Session abort cancels every tracked action. Cancelling one Action does not by
itself abort the entire Session, allowing sibling work to continue when that is
safe; in TypeScript it also does not recursively cancel that Action's children.

## Failure propagation

Thrown handler exceptions are converted to structured statuses at the Action
boundary. Register exception casters for provider-specific failures so quota,
authentication, and transient network errors remain actionable to a remote
caller.

When local execution fails, output nodes are aborted and the completion status
is still communicated when possible. When a remote status is non-OK, readers
and `wait()` observe the same failure rather than a generic transport
exception.

## What should I await?

| Operation | What completion means |
| --- | --- |
| `run()` | Local handler scheduling was accepted |
| `call()` | Remote control message was queued locally |
| `wait_for_dispatch()` | The peer accepted or rejected remote dispatch |
| output node read | One streamed output value/final/error became observable |
| `wait()` | Handler/call status and lifecycle cleanup are terminal |
| Python `done.wait()` | Lifecycle is terminal, regardless of success status |

Use `wait()` when success matters; the Python event-shaped `done` is useful for
coordination code that needs only to know that teardown finished.
