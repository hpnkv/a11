# A11 TypeScript API

Build browser and Node agents with the same action, stream, session, and wire
contracts as A11's Python and C++ runtimes.

```sh
npm install @curiositystack/a11
```

```ts
import {
  ActionPortSchema,
  ActionRegistry,
  ActionSchema,
  valueOrThrow,
} from "@curiositystack/a11";

const schema = new ActionSchema({
  name: "classify",
  inputs: {
    text: new ActionPortSchema({
      name: "text",
      type: "text/plain",
      unary: true,
      required: true,
    }),
  },
  outputs: {
    label: new ActionPortSchema({
      name: "label",
      type: "text/plain",
      unary: true,
      required: true,
    }),
  },
});

const registry = new ActionRegistry();
valueOrThrow(
  registry.register("classify", schema, async (action) => {
    const text = valueOrThrow(await action.getInput("text"));
    const value = valueOrThrow(await text.next());
    const label = valueOrThrow(await action.getOutput("label"));
    valueOrThrow(
      await label.finalize(
        String(value).includes("?") ? "question" : "statement",
      ),
    );
  }),
);
```

The public surface is organized around:

- `Action`, `ActionRegistry`, and schema classes for local and remote work.
- `AsyncNode`, `Chunk`, and serialization utilities for ordered typed streams.
- `Session` and WebSocket, HTTP SSE, WebRTC, and in-process transports.
- `Interaction`, tool adapters, and presentation reducers for LLM clients.
- `Status` and `StatusOr<T>` for failures that retain their meaning across a
  transport or language boundary.

## Headless developer-tool presentation

`@curiositystack/a11/presentation` contains the React-free, style-neutral
models used to build Studio-grade action, value, stream, Flow, JSON, and chat
interfaces. It exports semantic widget descriptions and Status-aware
controllers; the consumer owns markup, component conventions, CSS, focus, and
product integration.

```ts
import { isOk } from "@curiositystack/a11";
import { presentValue } from "@curiositystack/a11/presentation";

const presented = presentValue(bytes, "image/png");
if (!isOk(presented)) return presented;

// Render `presented.kind` using this product's own elements and styles.
```

DOM-specific overlay observation is opt-in through
`@curiositystack/a11/presentation/browser`. Importing the base presentation
subpath does not require browser globals. Fallible callbacks, transports,
storage, decoding, and controller mutations return `Status`, `StatusOr<T>`, or
promises thereof; handle or propagate failures after `isOk()` narrows them.

The generated pages include lifecycle details, parameter contracts, and small
usage examples on the primary symbols.

- [TypeScript overview and guides](https://docs.a11.to/typescript.html)
- [Run actions and stream values](https://docs.a11.to/guides/typescript-actions.html)
- [Connect sessions and transports](https://docs.a11.to/guides/typescript-sessions.html)
- [Build an LLM client](https://docs.a11.to/guides/typescript-llm.html)
- [Complete browser client](https://docs.a11.to/guides/browser-clients.html)

[Documentation](https://docs.a11.to/) ·
[Repository](https://github.com/hpnkv/a11) ·
[Questions and issues](https://github.com/hpnkv/a11/issues)
