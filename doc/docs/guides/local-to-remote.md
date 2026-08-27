# From a local run to a remote call

`.run()` starts an action handler in the current process. `.call()` dispatches
the same action to another peer over a session. This supports servers that hold
API keys, provide network egress, or own a GPU without changing the action's
schema, handler, or ports.

For example, begin with speech recognition beside a desktop client, then move
the handler to a GPU service when several clients need it. The client continues
to stream audio into the same input and read transcript fragments from the same
output. The same transition applies to model inference, image generation,
embedding, document conversion, and other resource-intensive handlers.

This guide demonstrates the transition with a small action. The same steps move
`interact_with_llm`, which has additional ports.

```python
import a11
```

## An action, run locally

Here is a one-line action and a local `.run()` — feed its input port, read its
output port, exactly like the LLM interaction:

```python
async def shout(action):
    text = await action["text"].consume()
    await action["result"].finalize(text.upper())


SHOUT = a11.ActionSchema(
    name="shout",
    inputs={
        "text": a11.ActionPortSchema(
            name="text", type="text/plain", typeinfo=str, required=True
        )
    },
    outputs={
        "result": a11.ActionPortSchema(
            name="result", type="text/plain", typeinfo=str, required=True
        )
    },
)


async def run_locally() -> str:
    action = a11.Action(SHOUT).bind_handler(shout).run()
    await action["text"].finalize("hello")
    result = await action["result"].consume()
    await action.wait()
    return result
```

## Register it so a server can host it

To run the handler on a peer, put it in an
[`ActionRegistry`][a11.actions.registry.ActionRegistry] under its name. A server
[`Session`][a11.service.session.Session] built with that registry dispatches
incoming calls to it:

```python
registry = a11.ActionRegistry()
registry.register("shout", SHOUT, shout)
```

## Stand up the server

This uses the server structure from the [echo session](echo-session.md), with an
`action_registry` in place of the echo callback so the session can dispatch
action calls:

```python
async def accept(stream):
    session = a11.Session(action_registry=registry)
    await session.add_stream(stream, mode="accept")
    await session.done.wait()


options = a11.WebSocketServerOptions()
options.path = "/ws"
server = a11.WebSocketWireServer.create(accept, options)
```

## Call it from a client

The client opens a session over a wire stream, then builds the action with
`registry.make_action`, binding it to that stream and session. `make_action`
needs the **schema** (to shape the ports) — the handler stays on the server.
`await action.call()` dispatches it across the wire:

```python
client = a11.Session()
stream = a11.WebSocketWireStream.connect(f"ws://127.0.0.1:{server.port}/ws")
await client.add_stream(stream, mode="start")

action = registry.make_action(
    "shout", node_map=client.node_map, stream=stream, session=client
)
await action.call()
```

## Feed and read, unchanged

Once dispatched, the ports behave exactly as in the local case — write the
input, read the output — but the bytes now travel over the network to the
server's handler and back:

```python
await action["text"].finalize("hello")
print(await action["result"].consume())
await action.wait()
```

Sealing a port carries across the connection as well as locally: closing a
writer tees a closure marker to the stream, so the peer's copy of that node
closes too. A handler that streams with plain `put()` and then closes therefore
ends the caller's read even though it never marked a fragment final — see
[the node lifecycle](../lifecycles/async-node.md#4-close-writes).

## Putting it together

Do the setup — registering the handler, creating the sessions — **inside your
async entrypoint** (under a running event loop), then run the action both ways:

```python
import asyncio
import a11


async def shout(action):
    text = await action["text"].consume()
    await action["result"].finalize(text.upper())


SHOUT = a11.ActionSchema(
    name="shout",
    inputs={
        "text": a11.ActionPortSchema(
            name="text", type="text/plain", typeinfo=str, required=True
        )
    },
    outputs={
        "result": a11.ActionPortSchema(
            name="result", type="text/plain", typeinfo=str, required=True
        )
    },
)


async def main() -> None:
    # Local: the handler runs in this process.
    local = a11.Action(SHOUT).bind_handler(shout).run()
    await local["text"].finalize("hello")
    print("local:", await local["result"].consume())

    # Remote: host the same action on a server and call it over a socket.
    registry = a11.ActionRegistry()
    registry.register("shout", SHOUT, shout)

    async def accept(stream):
        session = a11.Session(action_registry=registry)
        await session.add_stream(stream, mode="accept")
        await session.done.wait()

    options = a11.WebSocketServerOptions()
    options.path = "/ws"
    server = a11.WebSocketWireServer.create(accept, options)
    try:
        client = a11.Session()
        stream = a11.WebSocketWireStream.connect(
            f"ws://127.0.0.1:{server.port}/ws"
        )
        await client.add_stream(stream, mode="start")

        action = registry.make_action(
            "shout", node_map=client.node_map, stream=stream, session=client
        )
        await action.call()
        await action["text"].finalize("hello")
        print("remote:", await action["result"].consume())
        await action.wait()
    finally:
        server.stop()


asyncio.run(main())
```

## Local and remote calls

The two paths differ only in the last mile:

| Local | Remote |
| --- | --- |
| `a11.Action(SHOUT).bind_handler(shout)` | `registry.make_action("shout", stream=..., session=...)` |
| `.run()` — handler runs here | `.call()` — handler runs on the peer |

The handler, schema, and port I/O are identical. That is why
`interact_with_llm` moves server-side with no change to how you feed
`interactions`/`config`/`tools` or read `text_output`/`new_interactions` — set
its provider/model/key headers on the action you `make_action`, and `.call()`
it. Next, let the model [call an action of yours back](agent-tool.md).
