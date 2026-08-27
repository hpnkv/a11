# Examples

Use these examples to build local streams, distributed services, and
tool-using agents. Each example focuses on one application task and the A11 APIs
required to implement it.

| Example | Outcome |
| --- | --- |
| [Compose actions without deploying code](guides/flow.md) | Describe a composition of existing actions in the Flow language and run it as one action. |
| [Stream data through an AsyncNode](guides/streaming.md) | Produce and consume an ordered asynchronous stream, including clean finalisation. |
| [A WebSocket echo session](guides/echo-session.md) | Connect a client and server session over WebSocket and stream an action result back. |
| [Talk to a model](guides/llm.md) | Call `interact_with_llm`, feed a conversation, and print model output as it arrives. |
| [From a local run to a remote call](guides/local-to-remote.md) | Move an action across a session without changing its schema or handler-facing API. |
| [A toy agent with a tool](guides/agent-tool.md) | Register an A11 action as an LLM tool and let the model use its result. |
| [Going distributed](guides/going-distributed.md) | Choose storage and transport boundaries for an agent that spans processes or machines. |
| [Browser clients](guides/browser-clients.md) | Connect a TypeScript browser application to an A11 backend. |
| [A chat that survives a reload](guides/chat-sessions.md) | Answer with any provider through one action, and continue a conversation the SQLite store recorded. |
| [Deep research, as a composition](guides/deep-research.md) | Plan a topic, investigate its parts at once and synthesise a report — in Flow, with no orchestration code. |
| [The model calls back into the page](guides/browser-tools.md) | Serve actions from a web page and let a model drive them over the same socket. |
| [A port per thing the caller cares about](guides/generative-media.md) | Stream a step counter and a finished image off one action, on ports of their own. |
| [Local models on the web](guides/local-models-web.md) | Run a local model in a browser-facing application while retaining A11 action semantics. |
| [HTTP as separate streams](api/http-actions.md) | Make HTTP requests with a port per protocol concern — act on the status before the body arrives, read trailers, and receive pushed responses. |

Runnable companion programs live in the repository’s
[`examples/`](https://github.com/hpnkv/a11/tree/main/examples) directory.
