# Examples by task

Each guide starts from an application outcome and introduces the A11 concepts
needed to reach it. Begin with a core example, or go directly to the kind of
agent you are building.

## Learn actions and streams

- **[Return values as they are produced](guides/streaming.md).** Write, read,
  finalize, and fail an asynchronous stream.
- **[Serve an action over WebSocket](guides/echo-session.md).** Connect a client
  and server, send a call, and shut both sides down cleanly.
- **[Run the same action locally or remotely](guides/local-to-remote.md).** Move
  a handler to another peer without changing its schema or port I/O.
- **[Compose actions in a Flow document](guides/flow.md).** Build concurrent
  workflows from actions that are already registered.

## Add models and tools

- **[Stream a model response](guides/llm.md).** Send a portable conversation to
  Claude, Gemini, or Ollama through one action interface.
- **[Let a model call an application action](guides/agent-tool.md).** Turn an
  action schema into a tool and run requested calls through its registry.
- **[Use tools published by an MCP server](llm-sdk/mcp-tools.md).** Convert MCP
  declarations into actions that models, flows, and remote peers can call.
- **[Build a parallel research workflow](guides/deep-research.md).** Plan,
  investigate several briefs concurrently, and stream the final report.
- **[Let a model compose its tools](llm-sdk/flow-skill.md).** Keep intermediate
  tool results out of model context by running their composition as one action.

## Build browser experiences

- **[Call an A11 service from a browser](guides/browser-clients.md).** Use the
  same action contract in a Python backend and a TypeScript page.
- **[Continue a chat after a reload](guides/chat-sessions.md).** Store structured
  interactions and reopen a conversation by ID.
- **[Let a model use tools in the page](guides/browser-tools.md).** Serve actions
  over the page's existing connection so the model can operate on browser
  state.
- **[Run a model in the browser](guides/local-models-web.md).** Stream WebGPU
  inference through the same ports as a hosted model.
- **[Stream progress and a finished image separately](guides/generative-media.md).**
  Give each independently consumed result its own output port.

## Distribute and operate

- **[Exchange durable streams through Redis](guides/going-distributed.md).** Let
  two programs communicate without a direct connection.
- **[Represent an HTTP exchange as streams](api/http-actions.md).** Read status,
  headers, body, trailers, and pushed responses independently.
- **[Choose and verify the native allocator](guides/allocator.md).** Enable the
  packaged allocator for native executables or embedding processes.
- **[Integrate Flow diagnostics into an editor or CI](guides/flow-tooling.md).**
  Consume stable diagnostic, formatting, completion, and navigation responses.

Runnable companion programs are available in the repository's
[`examples/`](https://github.com/hpnkv/a11/tree/main/examples) directory.
