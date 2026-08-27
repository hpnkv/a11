# Let a model compose its own tools

A model that calls tools one at a time must read each intermediate result and
include it in the next call. Large pages, transcripts, and file listings
therefore consume model context and may lose detail during copying.

[`a11.sdk.flow_tools`][a11.sdk.flow_tools] offers the alternative as three
tools. The model writes a [flow](../guides/flow.md) — a composition of the
actions it can already call — and runs it as one step. The values that move
between the steps never pass through the model at all.

```python
from a11.actions import ActionRegistry
from a11.sdk import bash, flow_tools

registry = ActionRegistry()
bash.register(registry)  # the tools to compose
flow_tools.register(registry)  # and the ability to compose them

system_prompt = "\n\n".join(
    [bash.get_system_prompt(), flow_tools.get_system_prompt()]
)
```

Register them on the registry that holds the actions they are meant to compose:
a flow resolves its calls by name through the registry it runs under. In
`a11 chat`, for instance, the shell tools are the *client's* — so the flow tools
belong there too, not on the gateway.

## On the gateway

The gateway serves them by default, after everything else it registers, so a
composition can reach its shell, audio and conversation actions:

```sh
a11 gateway run                    # flow_actions, flow_check, flow_run served
a11 gateway run --no-flow-tools    # not served
```

Clients can send a flow that connects `capture_transcription` directly to
`interact_with_llm`, avoiding transcript round trips through the client and
model. `scripts/flow_playground.py` demonstrates this workflow: it discovers
gateway actions, checks a flow, captures a spoken sentence, and sends the model
reply history into the next turn with `then`.

A flow can also run on the *client* and call the gateway's actions, which is
what that script does with its own microphone-to-model composition. For that,
register the actions' **schemas** locally with no handler: a flow resolves
every call against its registry to learn the port names, and dispatches it to
the session precisely when it finds no handler to run it with. The SDK ships
the schemas (`audio.actions.CAPTURE_TRANSCRIPTION_SCHEMA`,
`INTERACT_WITH_LLM_SCHEMA`), so this costs one `registry.register(schema.name,
schema)` per action.

## The three tools

| Tool           | What the model does with it                                                                                                                                                         |
|----------------|-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `flow_actions` | Asks what it may compose. Returns each action with its **input and output ports** — the part a tool definition cannot carry, and exactly what a pipe needs on both sides of a `->`. |
| `flow_check`   | Compiles a flow and describes what it resolves to, without dispatching anything. A syntax error comes back with its line and column.                                                |
| `flow_run`     | Compiles, checks the call targets, runs the flow, and returns its declared outputs as one object.                                                                                   |

## Client-side streaming with `flow_run`

Models typically consume the collected object result. Application clients can
instead stream inputs into a running flow and receive incremental outputs:

```python
call = a11.Action(flow_tools.FLOW_RUN_SCHEMA)
await call.call()

# Access flow inputs and outputs through their deterministic node IDs.
said = session.node_map.get(flow_tools.flow_output_node_id(call.get_id(), "said"))
words = session.node_map.get(flow_tools.flow_input_node_id(call.get_id(), "words"))
words.attach_stream(stream)

# Declare streamed inputs and finalize static parameters.
await call["input_streams"].finalize(["words"])
await call["source"].finalize(SOURCE)

# Stream inputs and read incremental responses.
await words.put("one")
response = await said.next_object()
await words.finalize()
```

Finalize or close every port named in `input_streams` so the receiving action can
observe the end of input.

## What the model is not allowed to compose

A flow dispatches through the registry, underneath the layer that decides which
actions a model may call. So `flow_run` and `flow_check` make that decision
again themselves, against the same `x-a11-allowed-llm-actions` header
`a11.sdk.llm_tools.runner.collect_tools` reads: every `call` in the submitted
source is checked, and one naming an action the caller may not reach is refused
with `PERMISSION_DENIED` **before anything runs**. A flow may
not call the flow tools either — that would be a way around the same check, and
a way to recurse.

With no such header there is no restriction from this layer: a script or a test
driving these handlers is not a model being held to an allow-list.

## The instructions

The model needs to be told the capability exists, and it needs the language.
Both are one text, available in whichever shape the host prefers:

```python
flow_tools.get_system_prompt()  # for composing into a larger system prompt
flow_tools.get_skill()  # the same words as an a11.sdk.skill.Skill
```

The text embeds `a11.flow.REFERENCE`, the compact
[language reference](../api/flow.md), so prompts use the implemented syntax.
`a11/sdk/flow_tools/SKILL.md` is generated from the same constants and checked
in, for a host that loads skills from disk; a test fails when the file and the
code disagree.

## What a composed step looks like

Given `web-search`, `web-fetch` and `summarize`, a model asked a research
question can send this as one `flow_run` call:

```a11flow
flow answer-from-the-web {
  in  question: string required
  out answer:   string
  out sources:  string stream

  search = run web-search(query: question, limit: 3)
  brief  = run summarize(question: question)

  nodes fetched {
    for hit in search.hits parallel 2 {
      page = try run web-fetch(url: hit.url)
      hit.url -> sources
      page.text | truncate 2000 -> brief.pages
      skip page.bytes
    }
  }

  brief.summary -> answer
  skip search.debug
}
```

The result is `{"answer": ..., "sources": [...]}`. The `nodes fetched` block
keeps fetched and trimmed pages off the wire, while the model receives only the
declared answer and source values. Keep declared outputs small enough for the
model to read.

::: a11.sdk.flow_tools

::: a11.sdk.flow_tools.prompt

::: a11.sdk.flow_tools.handlers
