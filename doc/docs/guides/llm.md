# Stream a model response

`interact_with_llm` presents Claude, Claude Code, Gemini, and Ollama through one
action contract. Feed it messages, generation settings, and optional tool
definitions; read the response while the model is generating it; retain the
completed interaction for the next turn.

Provider SDKs commonly expose one event stream containing text deltas,
reasoning, tool-call arguments, usage, and completion events. A11 separates
those concerns into named output ports. A chat UI can read `text_output`, an
observability view can read `thoughts` or `event_stream`, and conversation
storage can retain `new_interactions`. None has to inspect the others' events.

Alongside `import a11` this page needs a few names from the SDK, which is where
the model helpers live:

```python
import a11
from a11.sdk.interact_with_llm import (
    INTERACT_WITH_LLM_SCHEMA,
    interact_with_llm,
)
from a11.sdk.llm import Interaction, LlmHeaders, Role
```

## Build and start the action

Construct the action from its schema, bind the handler, and set the provider /
model / key as headers. `.run()` starts it in the background and returns the
running action and its ports:

```python
import os

interact = (
    a11.Action(INTERACT_WITH_LLM_SCHEMA)
    .bind_handler(interact_with_llm)
    .set_header(LlmHeaders.PROVIDER.value, "gemini")
    .set_header(LlmHeaders.MODEL.value, "gemini-3.5-flash")
    .set_header(LlmHeaders.API_KEY.value, os.environ["GEMINI_API_KEY"])
    .run()
)
```

Ports are [`AsyncNode`][a11.nodes.async_node.AsyncNode] instances, reached with
`interact["<port>"]`; [Stream data through an AsyncNode](streaming.md) covers
their lifecycle.

### Run on a Claude Code subscription

The `claude_code` provider reaches Claude through the subscription the
[Claude Code](https://code.claude.com/docs/en/agent-sdk) CLI holds, so it takes
no API key:

```python
interact = (
    a11.Action(INTERACT_WITH_LLM_SCHEMA)
    .bind_handler(interact_with_llm)
    .set_header(LlmHeaders.PROVIDER.value, "claude_code")
    .run()
)
```

Install the backend with `pip install 'a11-kit[claude-code]'` and sign in by
running `claude` once. Every port behaves as it does for the other providers,
and registry actions reach the model the same way — Claude Code executes them
through an in-process MCP server the handler assembles.

Claude Code's own tools (file reads and writes, shell commands, web search)
stay off, so a turn offers the model only the actions the allow-list admits.
Turn them on through the config port:

```python
from a11.sdk.anthropic.interact_with_claude_code_schema import (
    CreateSessionConfig,
)

await interact["config"].finalize(
    CreateSessionConfig(
        builtin_tools=["Read", "Grep", "Bash"],
        disallowed_tools=["Bash(rm *)"],
    )
)
```

Enabling a tool also permits it: a session driven through A11 answers no
permission prompt, so there is no approval step between the model asking and
the tool running. Name the tools available to the turn. Use `disallowed_tools`
for a scoped rule such as
`Bash(rm *)`, which is refused in every permission mode.

Set the `x-a11-claude-code-system-preset` header to `claude_code` to keep
Claude Code's own system prompt and append the interaction's system
instructions to it.

### Run with OpenAI or Codex

Set the provider to `gpt` (or its `openai` alias) for the OpenAI API. The
handler reads `OPENAI_API_KEY`, streams text and reasoning, executes admitted
A11 actions as function tools, and accepts structured-output options through
`CreateChatCompletionConfig`.

Set the provider to `codex` to use the locally installed Codex CLI and its
existing login. The handler uses `codex app-server`, records the thread id on
assistant interactions, and resumes that thread on the next turn.
`CreateCodexSessionConfig` controls its workspace, sandbox, reasoning effort,
and final output schema.

Allowed actions are exposed as individual MCP tools with their argument
schemas. A temporary, authenticated loopback server dispatches each tool through
the shared A11 action runner. Codex's MCP parallel-call support lets independent
actions overlap while preserving per-call outputs, errors, images, and logs.
The server and credentials are refreshed for each turn, including resumed
threads. The parent action's deadline and cancellation apply to tool execution.

Install the provider with `pip install 'a11-kit[codex]'`. Automatic resumption
uses threads created with this MCP transport. A history from the dynamic-tool
transport starts a new thread; an explicit `config.resume` should name an
MCP-backed thread.

Install API support with `pip install 'a11-kit[openai]'`. Install the Codex CLI
with `npm install -g @openai/codex`.

## Stream the reply as it arrives

The assistant's visible text lands, already extracted from the raw provider
events, on the `text_output` port. Draining it in a task lets tokens print while
the rest of the interaction is still in flight:

```python
async def stream_text():
    async for chunk in interact["text_output"]:
        print(chunk, end="", flush=True)


stream_task = asyncio.create_task(stream_text())
```

## Feed the conversation in

The input side takes three ports:

- `interactions` — the conversation so far, ending with the new user turn;
- `config` — model settings; close it empty to use backend defaults;
- `tools` — tool definitions; this example closes the port empty.

An `Interaction` is a role plus content chunks:

```python
user_turn = Interaction(
    role=Role.USER,
    content=[a11.to_chunk({"role": "user",
                           "content": [{"type": "text", "text": "Hi!"}]})],
)

await interact["interactions"].finalize(user_turn)  # the input turn, ended
await interact["config"].finalize()   # empty: the backend's own defaults
await interact["tools"].finalize()    # close without tool definitions
```

`finalize()` on each port is what tells the handler that side is complete: it
marks the end of the data and closes the port. A port left open is a port the
handler waits on.

## Collect the result

The turns the model produced — its text, and any tool calls — arrive on
`new_interactions`. Read it to completion, then await the streaming task so the
last tokens have printed:

```python
new_interactions = []
async for interaction in interact["new_interactions"]:
    new_interactions.append(interaction)
await stream_task
await interact.wait()
```

Keep `new_interactions` around and prepend them (plus the user turn) to the next
call's `interactions` to carry the conversation forward.

An `Interaction` records assistant messages, tool calls, tool results, usage,
and provider continuation IDs. It is completed conversation state, not a live
stream or an agent checkpoint. Store it as ordinary application data; use the
output streams for content that must be rendered or processed incrementally.

## Collect nested usage

Construct `UsageAccounting` before starting an action to request dedicated
usage streams for its subtree. Continue draining the action's ordinary outputs:

```python
from a11.sdk.usage import UsageAccounting

accounting = UsageAccounting(action)
collection = asyncio.create_task(accounting.collect())
action.run()
await action.wait()
await collection
print(accounting.total_usage)
```

Every `interact_with_*` action and `interact_with_llm` exposes `usage_sources`.
Each entry identifies a producer, its enclosing producer, a native `NodeRef`,
and source metadata. The referenced node carries normalized `UsageUpdate`
values. The router forwards producer identities unchanged. An intermediate
A11 handler needs no usage-specific code when it creates nested actions through
A11's APIs.

`accounting.sources` retains attribution, `for_source(id)` reads one producer's
consumption, and `direct_usage` and `descendant_usage` select the corresponding
parts of the subtree. Revisions replace earlier reports of the same usage ID.
An aggregate can name covered usage IDs in `UsageUpdate.covers`; covered reports
remain available as evidence and contribute once to the total. Context counts
remain associated with direct model requests.

`coverage == "reported_sources"` describes registered producers. Tools without
instrumentation and subtrees that disable tracking remain unobserved. Missing
reports, interrupted streams, and unverified cumulative counts produce
incomplete accounting. Independent read cursors support multiple consumers and
replay of retained usage.

### Disable usage tracking

Dedicated tracking defaults to off. `UsageAccounting` requests it through the
inherited `x-a11-usage-tracking: on` header. An explicit off suppresses that
request for an action subtree:

```python
from a11.sdk.usage import enable_usage_tracking

enable_usage_tracking(action, False)
```

Configure the policy before `run()` or `call()`. Disabled tracking creates no
producer nodes or source registrations. Existing provider-event and interaction
usage remains available. `enable_usage_tracking(action)` also enables tracking
without constructing a collector; a collector can read that retained usage
later.

### Connect another usage producer

An action wrapping an external tool can publish normalized usage with its own
source metadata, including when it has no `event_stream` output:

```python
from a11.sdk.llm import UsageMetadata
from a11.sdk.usage import UsageReporter

reporter = UsageReporter(
    action,
    "external-provider",
    usage_source={"type": "external_tool", "id": tool_call_id},
)
await reporter.report(UsageMetadata(input_tokens=20, output_tokens=7, total_tokens=27))
```

The adapter owns provider-specific extraction and normalization. A11 preserves
source identity, revisions, and stream lifetime through nested and remote
calls.

### Resolve usage from shared storage

`NodeRef.storage` is optional. Its absence resolves the node in the current
node map. A `StorageIdentity` names a namespace; a host registers its backend
or supplies a resolver that returns the appropriate `NodeMap`:

```python
identity = a11.StorageIdentity("usage-store")
backend = a11.NodeMap(chunk_store_factory=store_factory, storage=identity)
host_nodes.register_storage(identity.id, backend)
```

Producer nodes inherit their node map's storage identity. A collector advertises
its directly accessible namespaces. References cross intermediate actions,
while usage payloads remain in shared storage when the collector has direct
access. Otherwise the runtime mirrors payloads over the action transports.

`host_nodes.set_storage_resolver(resolve, accessible_ids=["usage-store"])`
installs host connection policy. The synchronous callback receives a typed
`StorageIdentity` and returns a backend node map, or `None` when unavailable.
Advertised identities are namespaces the host can access directly for the run.

A storage identity can also carry optional `StorageService` values. Each names
a service role and endpoint, with optional `StorageCredentials` containing a
scheme and flat string fields. For Redis plus S3, services can describe the
index and payload endpoints separately; an S2 backend can describe its stream
endpoint and bearer credential. The host resolver decides which supplied access
fields it accepts. A bare identity uses host configuration. Native diagnostic
representations omit credential values.

`StorageIdentity.credentials` carries namespace-level access fields when a
single credential covers the backend. `NodeRef.with_credentials(credentials)`
returns a qualified copy with those fields;
`with_credentials(credentials, service="payload")` sets one existing service.
Call `source.ref(storage="include")` before adding credentials to a local
reference. A backend map can opt into opening uncached durable targets with
`set_allow_lazy_read_resolution(True)`; local maps resolve existing nodes only.

`NodeRef.offset` identifies the first fragment sequence and `length` counts
fragments. Storage qualification preserves those units. Usage producers expose
whole-node references. Referenced producer nodes remain addressable after action
completion; their node map or backend controls retention independently of normal
output-port cleanup.

## Run the complete model call

```python
import asyncio
import os

import a11
from a11.sdk.interact_with_llm import (
    INTERACT_WITH_LLM_SCHEMA,
    interact_with_llm,
)
from a11.sdk.llm import Interaction, LlmHeaders, Role


async def ask(text: str) -> list[Interaction]:
    interact = (
        a11.Action(INTERACT_WITH_LLM_SCHEMA)
        .bind_handler(interact_with_llm)
        .set_header(LlmHeaders.PROVIDER.value, "gemini")
        .set_header(LlmHeaders.MODEL.value, "gemini-3.5-flash")
        .set_header(LlmHeaders.API_KEY.value, os.environ["GEMINI_API_KEY"])
        .run()
    )

    async def stream_text():
        async for chunk in interact["text_output"]:
            print(chunk, end="", flush=True)

    async def collect_interactions():
        return [item async for item in interact["new_interactions"]]

    text_task = asyncio.create_task(stream_text())
    state_task = asyncio.create_task(collect_interactions())

    user_turn = Interaction(
        role=Role.USER,
        content=[a11.to_chunk({"role": "user",
                               "content": [{"type": "text", "text": text}]})],
    )
    await interact["interactions"].finalize(user_turn)
    await interact["config"].finalize()
    await interact["tools"].finalize()

    await text_task
    new_interactions = await state_task
    await interact.wait()
    return [user_turn, *new_interactions]


history = asyncio.run(ask("Say hello in three languages."))
```

`history` is ready for the next model call or application storage. The visible
text and durable conversation state travel independently, so neither needs to
be reconstructed from the other.

The full multi-turn, multi-provider version is `examples/002-llm-interactions`.

The model call above ran **in the local process**. Move it behind a
[remote server](local-to-remote.md), then give the model a
[tool it can call back into](agent-tool.md).
