# Examples

These examples grow from one local stream into distributed and tool-using
agents. Each one focuses on an applied task and points out the part of A11 that
makes it work.

| Example | What you will build |
| --- | --- |
| [Compose actions without deploying code](guides/flow.md) | Describe a composition of existing actions in the Flow language and run it as one action. |
| [Stream data through an AsyncNode](guides/streaming.md) | Produce and consume an ordered asynchronous stream, including clean finalisation. |
| [A WebSocket echo session](guides/echo-session.md) | Connect a client and server session over WebSocket and stream an action result back. |
| [Talk to a model](guides/llm.md) | Call `interact_with_llm`, feed a conversation, and print model output as it arrives. |
| [From a local run to a remote call](guides/local-to-remote.md) | Move an action across a session without changing its schema or handler-facing API. |
| [A toy agent with a tool](guides/agent-tool.md) | Register an A11 action as an LLM tool and let the model use its result. |
| [Going distributed](guides/going-distributed.md) | Choose storage and transport boundaries for an agent that spans processes or machines. |
| [Browser clients](guides/browser-clients.md) | Connect a TypeScript browser application to an A11 backend. |
| [Local models on the web](guides/local-models-web.md) | Run a local model in a browser-facing application while retaining A11 action semantics. |
| [HTTP as separate streams](api/http-actions.md) | Make HTTP requests with a port per protocol concern — act on the status before the body arrives, read trailers, and receive pushed responses. |

Runnable companion programs live in the repository’s
[`examples/`](https://github.com/hpnkv/a11/tree/main/examples) directory.
