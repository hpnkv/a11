# A11

A11 is an open-source library and runtime for building AI agents and the
services around them. It connects model calls, application tools, storage, and
user interfaces through one small set of concepts that work in Python,
TypeScript, and C++.

The same runtime can serve a GPU model to several clients, stream live captions
from speech recognition, feed decoded records into an indexing pipeline, or
report diffusion progress before returning an image. None of these products
needs an agent loop. They need well-scoped operations, typed data exchange, and
streaming across component and process boundaries.

If A11 is new to you, start with an **action**: an asynchronous operation whose
inputs and outputs have names. Each input and output is a **node**, an ordered
stream that may carry one value or many. This lets a caller display text,
process records, or report progress while the action is still running.

An action keeps the same contract when it becomes an LLM tool or moves to
another process. Storage and transport are separate choices, and
[Flow](guides/flow.md) is available when several actions need a reusable or
runtime-defined composition. Applications can begin with local calls and add
these pieces as their deployment and reliability needs grow.

## Choose what you want to build

- **[Return results while work is still running](guides/streaming.md).** Start
  with an ordered stream and learn how producers signal completion or failure.
- **[Host a model or pipeline behind a stable API](guides/local-to-remote.md).**
  Keep its streamed inputs and outputs unchanged while the handler moves to a
  GPU host or service process.
- **[Stream text without handling provider events](guides/llm.md).** Read text,
  reasoning, and completed conversation state from separate outputs.
- **[Give a model tools you already use](guides/agent-tool.md).** Turn an
  application action into a model tool and authorize it for one turn.
- **[Call an A11 service from a browser](guides/browser-clients.md).** Share an
  action contract between a Python service and a TypeScript interface.
- **[Compose the available tools safely at runtime](guides/flow.md).** Check a
  Flow document, enforce its call policy, and pipe data between concurrent
  actions.
- **[Build a focused agent harness or evaluation path](guides/harnesses-evals.md).**
  Reuse actions for tools, durable conversations, trials, and streamed results.

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

## The choices that keep an application flexible

- An **action** is a named operation with described inputs and outputs.
- Each input or output is a **node**, so a single value and a live stream use
  the same interface.
- A **session** carries action calls and node data between peers. Local handlers
  use the same action contract as remote handlers.
- A **store** controls how long node data remains available. A transport carries
  that data between peers.
- A **registry** makes actions discoverable and supplies their handlers. A
  per-turn allow-list controls which registered actions a model may call.
- A **flow** connects actions when a composition should be checked, shared, or
  supplied at runtime. It resolves against the actions available to its host;
  ordinary action calls remain sufficient elsewhere.

The same primitives cover common agent application needs:

- [show progress separately from a finished image](guides/generative-media.md);
- [continue a recorded conversation after a reload](guides/chat-sessions.md);
- [let a model operate on state inside a web page](guides/browser-tools.md);
- [investigate several research questions concurrently](guides/deep-research.md);
- [exchange durable streams without a direct connection](guides/going-distributed.md).

They also cover APIs and pipelines that contain no agent:

- [return protocol fields while an HTTP body is arriving](api/http-actions.md);
- [serve image generation with separate progress and result
  ports](guides/generative-media.md);
- [run a model in a browser through the same interaction
  ports](guides/local-models-web.md);
- [compose speech capture, transcription, and generation at runtime](guides/flow.md).

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
