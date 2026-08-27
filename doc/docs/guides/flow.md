# Compose actions through streamed data

This guide composes a voice interaction from existing actions. The client
captures microphone audio, a GPU host transcribes it, and a model receives the
first complete sentence with the conversation history. The answer streams back
as it is generated.

`capture_audio`, `transcribe_audio`, and `interact_with_llm` are independently
registered actions. [Flow](../api/flow.md) connects their existing ports in a
document that the receiving runtime compiles. The document can be stored with
the application or supplied at runtime by a client or model. Its handlers and
their deployment locations do not change.

The composition keeps high-rate audio and transcript fragments inside the
runtime. Buffers pass directly from each step's output port to the next input
port; only the completed sentence and model response cross the external
boundary.

## Execution follows the data

A flow is not a stored action graph. Its calls are started eagerly and run
concurrently. Reading an input waits for data, writing an output makes data
available, and the runtime closes a destination after all of its writers
finish. Use `after` or `wait` only when ordering cannot be expressed by a data
dependency.

Calls may therefore be declared before all of their inputs are supplied:

```a11flow
search = run web-search(query: question, limit: 3)
brief  = run llm-summarize(question: question)

for hit in search.hits parallel 3 {
  page = run web-fetch(url: hit.url)
  page.text | truncate 2000 -> brief.pages
}

brief.summary -> answer
```

The summarizer is available while searches and fetches are running. Pages
stream into its open `pages` port, so it can begin consuming the first page
without waiting for a collected list. Declaring calls first gives the document
a graph-like overview when that is useful; ports still provide the scheduling
and synchronization.

## Compose what is available at runtime

Flow is a runtime language over an `ActionRegistry`. A client can ask a host
which actions it currently provides, construct a document using their declared
ports, check that document, and run it immediately. A model uses the same path
through `flow_actions`, `flow_check`, and `flow_run`.

This remains controlled dynamic composition:

- parsing and resolution report malformed syntax, unknown actions, missing
  ports, and incompatible connections before execution;
- `flow_run` checks every model-authored call against the turn's action
  allow-list before any step runs;
- a document can use only registered actions, host-registered value types, and
  Flow's bounded language constructs; it cannot import or define code;
- deadlines, cancellation, stream failure, and sandbox limits are runtime
  contracts;
- intermediate values pipe between actions without another model turn and can
  stay in local nodes instead of crossing the wire.

The composition may change for each request while its available capabilities
and policy remain explicit. This is useful for user-designed automations,
model-authored tool plans, and services that assemble a response from whatever
actions are deployed on the receiving host.

## The interface

A flow begins with the same contract as an action: its purpose, inputs, and
outputs.

```a11flow
flow interact-on-full-sentence {
  describe "Listen; the first full sentence becomes the next turn of a conversation."

  in  asr:     object required "Speech recognition options; model required."
  in  device:  object required "Which input to open on the client."
  in  history: string required "Id of the node holding the turns so far."

  out sentence: string                     "The sentence that became the question."
  out reply:    string stream              "The model's answer, as it is written."
  out turn:     a11.sdk.Interaction stream "What to remember of this turn."
}
```

A port declares its value type, followed by its cardinality and requirement. It
carries one value unless marked `stream`, and is optional unless marked
`required`. These declarations become the
[`ActionSchema`][a11.actions.action.ActionSchema] used by callers and by models
selecting a tool.

`history` is the **ID of a node** that holds the conversation. The caller owns
the prior turns, and the stateless flow attaches to that stream without copying
it.

## Opening the microphone

```a11flow
  mic = call capture_audio(options: device) timeout 600s
  skip mic.events  # <- we never read them, so declare as skipped
```

`run` executes a handler registered with the process running the flow. `call`
dispatches through the flow's attached stream. Because this composition runs on
a gateway while the microphone belongs to the client, capture uses `call`.

`skip` drains an output port without retaining its values. Undrained outputs
stall their producers. Use `-> _` when a pipeline must execute but its result is
not needed: `pages | map summarise(it) -> _` processes every page, while
`skip pages` performs no summarisation.

The `timeout` bounds the wait for a sentence. Expiry propagates as a flow
failure.

## Recognising what was said

```a11flow
  nodes scratch  # local nodes that are never sent to the client

  transcribe = run transcribe_audio(
    asr_options: asr, audio: mic.audio | packb
  ) via scratch  # <- never send transcription outputs to the client directly
  skip transcribe.events
```

`mic.audio` is the client's capture stream, and `audio:` is the recogniser's
input port. The pipe sends each buffer directly from the remote `call` output to
the local `run` input:

* buffers are not assembled into one in-memory value;
* binary audio is not rendered as text;
* audio buffers do not enter the model context.

`| packb` encodes values as `application/x-msgpack`. It is a no-op when the
producer already wrote MessagePack, so the destination receives packed bytes
for either input representation.

`nodes scratch` and `via scratch` keep intermediate nodes local. A `nodes` block
gives its steps a separate [`NodeMap`][a11.nodes.async_node.NodeMap], so their
ports do not appear in the session's node map and their fragments are not sent
to the dispatching peer. Only the completed sentence reaches the client.

Because nothing reads transcription errors, omitting `try` propagates them to
the caller.

## The first full sentence

Recognition arrives as pieces, and a piece is not a sentence.

```a11flow
  said = node() in scratch

  transcribe.transcription_pieces
    | group ends-with(trim(it), [".", "?", "!"])
    | first 1
    | map trim(join(it, " "))
    -> said

  said -> sentence
```

`| group EXPR` collects values until the expression is true for the newest
value. Here, fragments accumulate until one ends with a full stop, question
mark, or exclamation mark. Joining that group produces a sentence, and
`| first 1` ends the pipeline after the first sentence.

`said` is a node of the flow's own because the sentence is wanted **twice** — by
the client, on `sentence`, and by the model, as the question. A node is how a
stream gets two readers: reading one stream twice would hand each reader half of
it, because a node has one cursor. It is `in scratch`, so the only copy that
crosses the wire is the one on `sentence`.

## Stopping, once there is something to answer

```a11flow
  # Wait for a sentence before stopping audio capture.
  {"command": "stop"} -> mic.control_events after said
```

This is the only ordering statement in the file. Everything else in the flow
body runs concurrently, with data dependencies providing synchronization.
`after` closes the microphone only once a sentence exists.

## The conversation

```a11flow
  earlier = node(history)

  asked = node() in scratch
  said | map a11.sdk.Interaction{
    role: "user",
    content: [to_chunk({
      role: "user",
      content: [{type: "text", text: it}]
    })]
  } -> asked
```

`node(history)` **attaches** to an existing node by the id provided
on the `history` port, reading an external stream. Each interaction arrives
as the type it was written as and preserves its structured payload.

`TYPE{...}` builds a named value. The sentence becomes an
`a11.sdk.Interaction`, and validation reports incompatible fields before the
model call. The host defines available types: its serialisation registries
resolve dotted tags, while a flow cannot import modules.

`asked` is its own node for the same reason `said` was: both the model and the
client's history want it.

## Asking, and answering

```a11flow
  interact = run interact_with_llm(
    interactions: earlier then asked,
    config: {}
  )
      forward headers "x-a11-llm-*"
      via scratch

  skip interact.event_stream
  skip interact.thoughts

  interact.text_output -> reply

  asked then interact.new_interactions -> turn
```

`earlier then asked` concatenates the prior turns and the new question in that
order. Direct writes from two statements would interleave by arrival; `then`
preserves conversation order.

`forward headers "x-a11-llm-*"` passes the caller's headers to the step that
needs them. The caller selects the model and provider without changing the
composition.

`interact.text_output -> reply` is the answer, streamed to the client token by
token as the model writes it. And `turn` hands back what to remember: the
question, then the answer and any tool interactions the model made on the way to
it. The client appends those to the conversation and hands its node back as
`history` next time round.

## The complete flow

```a11flow
--8<-- "intellij-plugin/src/main/resources/flows/interact-on-full-sentence.flow"
```

The document records where each step runs, how values move between steps, which
outputs are discarded, which calls remain local, and where execution order
matters.

## Running it

In process, the source is a string and compiling it is one call:

```python
from a11 import flow

program = flow.register(source, registry, "interact-on-full-sentence.flow")
result = await program["interact-on-full-sentence"].invoke(
    asr=asr_options.model_dump(),
    device=capture_options.model_dump(),
    history=node.get_id(),
    registry=registry,
)
```

[`register`][a11.flow.register] compiles and publishes every flow in the source as
an action, after which a [`Session`][a11.service.session.Session] dispatches them
like anything else. [`invoke`][a11.flow.plan.FlowPlan.invoke] is the convenience
path for scripts and tests. A flow that will not compile raises
[`FlowSyntaxError`][a11.flow.diagnostics.FlowSyntaxError] with the line and column.

Across a session it is the same source, sent: a gateway serves `flow_check` and
`flow_run`, so a client hands over the text and reads the outputs off published
nodes. `scripts/flow_playground.py` is that, runnable — it asks the gateway what
it can compose, has it check this flow, then listens and answers, turn after turn:

```sh
a11 gateway run                     # the other end
python scripts/flow_playground.py   # --check_only compiles without a microphone
```

The gateway already serves these actions; the composition arrives as an
argument.

## ...including by a model

[`a11.sdk.flow_tools`](../llm-sdk/flow-skill.md) exposes three tools to a model:
`flow_actions` lists composable actions and their ports, `flow_check` compiles a
flow without running it, and `flow_run` executes it.

Once dispatched, the runtime pipes intermediate values between actions. Large
or high-rate values such as audio buffers, fetched pages, and transcript
fragments stay out of the model context. In this example, the model receives one
completed sentence and the caller receives the streamed answer.

## Four common composition patterns

The following patterns cover common extensions to a composition.

**Work on several values at once.** A `map` whose expression is expensive — a
coercion, a round trip through the host — may say how many values it may have in
hand. Downstream stages still receive results in input order:

```a11flow
urls | map fetch_page(it) parallel 8 -> bodies
```

**One bad value out of a thousand.** `try` on a stage says a value the stage
cannot do is not a reason to abandon the stream, and `into` says where those go
as status records:

```a11flow
docs | try map it as Order into rejected -> orders
```

**Two streams, whichever arrives.** `interleave` reads several at once and gives
each value as it comes, so a slow source does not hold up a fast one — which is
what a flow watching a model and a tool at the same time wants:

```a11flow
interleave(llm.text_output, tool.progress) -> shown
```

**Whichever call answers first.** `wait first of` holds until one of several
calls finishes and lets the rest carry on. It is a value as well as a barrier:
the number of the one that won, counted from zero, so the flow can act on *which*
answer it got.

```a11flow
fast = call cached(query: q)
slow = call searched(query: q)
wait first of fast, slow -> whose_answer
```

**Arithmetic over a stream.** `| sum`, `| min`, `| max`, `| avg` and `| sort`
read the whole stream and are written where they read:

```a11flow
orders | sum it.price -> revenue
hits   | sort by it.score desc | first 10 -> best
```

`| fold 0 as total, total + it` emits the final accumulated value. `| scan` emits
each intermediate accumulated value, supporting streams whose values depend on
prior input:

```a11flow
lines | scan 0 as n, n + 1 -> numbered
```

`| window 2` creates overlapping lists. Unlike `batch`, it can detect a pattern
that spans a group boundary.

`| timeout 30s` limits gaps between values, and `| pace 100ms` enforces a minimum
interval between emitted values.

## Where to go from here

The rest of the language — durations and arithmetic, `repeat`, `for`, `if`,
`try`/`wait`/`fail`, `log`/`logf`, `match`, `strformat`, and sandbox limits
— is in the [Flow language reference](../api/flow.md). `a11.flow.REFERENCE`
provides a compact version suitable for a model prompt.

The runnable examples are in
[`examples/003-flow-dsl`](https://github.com/hpnkv/a11/tree/main/examples/003-flow-dsl).
Three of those files need nothing but the toy actions beside them; the other
three — `assistant.flow`, `ops.flow` and `dictate.flow` — compose what a real
`a11 gateway run` serves.

For writing flows: editor support is in
[`editors/`](https://github.com/hpnkv/a11/tree/main/editors), and the
[A11 plugin for JetBrains IDEs](https://github.com/hpnkv/a11/tree/main/intellij-plugin)
highlights `.flow` files *and* flows written inside a string literal, which is
where most of them live. [Checking flows from a toolchain](flow-tooling.md) is the
same language as a set of machine-readable answers, for CI and for editors of your
own.
