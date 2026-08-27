# A11

A11 helps an agent start work immediately, stream partial results, and call the
same operations in another process or on another machine. Build each capability
as an action with named inputs and outputs, then decide where it runs when you
deploy it.

## Choose what you want to build

- **[Return results while work is still running](guides/streaming.md).** Start
  with an ordered stream and learn how producers signal completion or failure.
- **[Move an action from local code to a service](guides/local-to-remote.md).**
  Keep its inputs and outputs unchanged while the handler moves behind a
  network connection.
- **[Stream a response from a language model](guides/llm.md).** Feed a
  conversation into one action and display its answer as it arrives.
- **[Let a model use an application action](guides/agent-tool.md).** Publish an
  action as a tool, control which tools are available, and return the result to
  the model.
- **[Call an A11 service from a browser](guides/browser-clients.md).** Share an
  action contract between a Python service and a TypeScript interface.
- **[Compose existing actions into a workflow](guides/flow.md).** Connect
  actions, branches, and concurrent steps in a Flow document.

The [examples page](examples.md) groups the remaining guides by task, including
persistent chat, browser-hosted tools, parallel research, local models, and
distributed streams.

## A stream in one minute

An `AsyncNode` is an ordered stream. A producer writes values and finalizes the
stream; a consumer can process each value as it arrives.

```python
import asyncio
import a11


async def main() -> None:
    node = a11.AsyncNode.create("greeting")

    await node.put("hello")
    await node.finalize("world")

    async for value in node:
        print(value)


asyncio.run(main())
```

Action inputs and outputs use these same streams. A handler can therefore emit
progress or partial output before it has finished, and its caller can begin
processing that output immediately.

## How an application fits together

- An **action** is a named operation with described inputs and outputs.
- Each input or output is a **node**, so a single value and a live stream use
  the same interface.
- A **session** carries action calls and node data between peers. Local handlers
  use the same action contract as remote handlers.
- A **store** controls how long node data remains available. A transport carries
  that data between peers.

This separation supports a few useful application patterns:

- [show progress separately from a finished image](guides/generative-media.md);
- [continue a recorded conversation after a reload](guides/chat-sessions.md);
- [let a model operate on state inside a web page](guides/browser-tools.md);
- [investigate several research questions concurrently](guides/deep-research.md);
- [exchange durable streams without a direct connection](guides/going-distributed.md).

## Understand the design

[Why A11](principles.md) explains the streaming model, local and remote
execution, storage choices, and lifecycle boundaries. The lifecycle articles
then show exactly when a [node](lifecycles/async-node.md),
[action](lifecycles/action.md), [session](lifecycles/session.md), or
[connection](lifecycles/wire-stream.md) is complete.

## API references

- [Python API](api/nodes.md)
- [TypeScript API](typescript.md)
- [C++ API](cpp.md)
- [Flow language](api/flow.md)
