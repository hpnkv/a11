# Build browser and Node agents with TypeScript

The TypeScript package provides actions, typed streams, sessions, transports,
serialization, and LLM client utilities for browsers and Node.js. It uses the
same wire records and lifecycle statuses as the Python and C++ runtimes.

```sh
npm install @curiositystack/a11
```

All public symbols are available from one entry point:

```ts
import {
    Action,
    AsyncNode,
    Session,
    StatusException,
    valueOrThrow,
} from '@curiositystack/a11';
```

## Choose what you want to build

- [Run actions and stream values](guides/typescript-actions.md) when work lives
  in the page or a Node process.
- [Connect sessions and transports](guides/typescript-sessions.md) when an
  action runs in another process or language.
- [Build an LLM client](guides/typescript-llm.md) to create interactions,
  expose actions as tools, and render live or stored turns.
- [Call an A11 service from a browser](guides/browser-clients.md) for a complete
  HTTP SSE client and Python service.
- [Let a model use tools in the browser](guides/browser-tools.md) for handlers
  that operate on page state.

## API areas

| Area | Primary symbols |
| --- | --- |
| Actions | [`Action`](typescript/classes/Action.html), [`ActionRegistry`](typescript/classes/ActionRegistry.html), [`ActionSchema`](typescript/classes/ActionSchema.html) |
| Streams | [`AsyncNode`](typescript/classes/AsyncNode.html), [`NodeMap`](typescript/classes/NodeMap.html), [`Chunk`](typescript/classes/Chunk.html) |
| Connections | [`Session`](typescript/classes/Session.html), [`WebSocketWireStream`](typescript/classes/WebSocketWireStream.html), [`HttpSseClientWireStream`](typescript/classes/HttpSseClientWireStream.html), [`WebRtcWireStream`](typescript/classes/WebRtcWireStream.html) |
| Data | [`toChunk`](typescript/functions/toChunk.html), [`fromChunk`](typescript/functions/fromChunk.html), [`SerializationRegistry`](typescript/classes/SerializationRegistry.html) |
| LLM clients | [`Interaction`](typescript/types/Interaction.html), [`ToolAdapter`](typescript/classes/ToolAdapter.html), [`PresentationReducer`](typescript/classes/PresentationReducer.html) |
| Errors | [`Status`](typescript/types/Status.html), [`StatusCode`](typescript/enums/StatusCode.html), [`valueOrThrow`](typescript/functions/valueOrThrow.html) |

[Open the complete TypeScript API reference](typescript/index.html){ .md-button .md-button--primary }

## Handle statuses at boundaries

Runtime methods return `Status` or `StatusOr<T>` so failures can cross workers,
transports, and language boundaries without losing their code or details.
Use `isOk` while composing operations. Use `valueOrThrow` at an application
boundary where exceptions fit the surrounding framework.

```ts
const node = valueOrThrow(await AsyncNode.create('answer'));

try {
    valueOrThrow(await node.finalize({text: 'Ready'}));
} catch (error) {
    if (error instanceof StatusException) {
        console.error(StatusCode[error.status.code], error.status.message);
    }
}
```

`Action.signal` is an `AbortSignal` for cooperative handler cancellation.
Cancelling an action also cancels its active nested actions and sends a remote
cancellation message for calls.
