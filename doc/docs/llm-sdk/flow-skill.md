# Let a model compose its own tools

A model that calls tools one at a time must read each intermediate result and
include it in a later request. Large pages, transcripts, and file listings then
consume the context window, add input tokens, and require the model to copy
values between calls.

[`a11.sdk.flow_tools`][a11.sdk.flow_tools] offers the alternative as three
tools. The model writes a [flow](../guides/flow.md) — a composition of the
actions available for that turn — and runs it as one step. No new handler is
generated or deployed. The host checks the document against its current
registry and allow-list, then the values moving between steps bypass the model.

Flow does not add a persistent graph or another agent loop. The runtime starts
the declared actions, pipes their named ports, and lets data arrival coordinate
them. The model still chooses the composition, but the deterministic transfer
of intermediate values runs outside its context.

## Use skills for knowledge and Flow for a checked procedure

The [Agent Skills specification](https://agentskills.io/specification) defines
a portable folder containing `SKILL.md` instructions and optional scripts,
references, and assets. Compatible harnesses use progressive disclosure: they
advertise each skill's name and description, then load its instructions when a
task appears to match.

That format is useful for domain knowledge, judgment, and procedures whose
details vary with the task. A skill may bundle tested scripts, but its
`SKILL.md` procedure is still interpreted by the model. The model decides
whether the skill applies, selects each tool, and copies each tool result into
a later request. More detailed instructions can improve consistency, but they
do not guarantee that every described step occurs.

Flow is the stronger form when the procedure can be expressed through actions:

- `flow_check` resolves action names, ports, and types before execution, and
  `flow_run` checks the model turn's action permissions before dispatch;
- branches, loops, ordering, concurrency, and failure handling have defined
  runtime semantics;
- an output pipes directly to the next input without entering model context;
- large intermediate values can remain in local nodes and off the wire;
- only the Flow source and declared results need to occupy model context.

This distinction matters in a research task. A skill can tell a model to
search, fetch several pages, trim them, and summarize them. The model must still
perform that loop and observe the page contents. A Flow expresses the same
procedure once:

```a11flow
search = run web-search(query: question, limit: 3)
brief  = run summarize(question: question)

for hit in search.hits parallel 3 {
  page = run web-fetch(url: hit.url)
  page.text | truncate 2000 -> brief.pages
}

brief.summary -> answer
```

The runtime performs every declared pipe and control-flow construct. It does
not depend on the model remembering the next instruction after each tool call.
Model calls and external services remain variable, and concurrent streams may
arrive in different orders. The model sees the compact composition and final
answer, not every fetched page or transfer step.

Skills and Flow can work together. `a11.sdk.flow_tools` publishes its compact
language reference as an Agent Skill, helping a model decide when and how to
write a composition. `flow_check` then validates the document, and `flow_run`
executes the checked semantics. The skill supplies judgment; Flow supplies the
data path and control flow.

When a model chooses whether to call `flow_run`, that choice is still model
judgment. Once the document is submitted, completing its declared procedure no
longer depends on further skill activation or instruction following.

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
