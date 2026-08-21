# Compose actions without deploying code

Say you want this: hold this machine's microphone open, recognise what is said on
the machine that has the GPU, take the first sentence that ends in a full stop,
put it to a model along with everything said earlier in the conversation, and
stream the answer back as it is written.

Every piece of that already exists. `capture_audio`, `transcribe_audio` and
`interact_with_llm` are deployed, registered and doing their jobs. What is
missing is forty lines saying how they connect — and written in Python, those
forty lines are a commit, a review, a release and a restart of whatever they run
on.

[Flow](../api/flow.md) expresses those connections as text that the runtime
compiles wherever it is dispatched. This page builds the composition one
statement at a time.

The composition keeps high-rate audio and transcript fragments inside the
runtime. Buffers pass directly from each step's output port to the next input
port; only the completed sentence and model response cross the external
boundary.

## The interface

A flow is an action, so it starts by saying what an action says: what it is for,
and what goes in and out.

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

A port says what it holds first and what it is like afterwards: it carries one
value unless it says `stream`, and is optional unless it says `required`. The
descriptions are not decoration — this is the [`ActionSchema`][a11.actions.action.ActionSchema]
whoever dispatches the flow will read, and if that is a model, this is what it
reads to decide whether the flow is the thing it wants.

Note what `history` is: not the conversation, but the **id of a node** holding it.
The flow is stateless; the turns so far belong to the caller, and the flow will
attach to that stream rather than have it copied in.

## Opening the microphone

```a11flow
  mic = call capture_audio(options: device) timeout 600s
  skip mic.events  # <- we never read them, so declare as skipped
```

`call`, not `run`. A11 has two verbs for getting an action done and a flow says
which it means: `run` executes the handler registered where the flow is running,
`call` puts the action on the stream the flow is attached to and lets the peer do
it. This composition runs on a gateway, and the microphone is not there — so the
one step that needs the client is the one written with `call`.

`skip` reads an output port and keeps nothing. It is needed because an output
nobody drains stalls the action producing it; saying it in the flow is how a
composition stays explicit about what it is not interested in.

The `timeout` is the whole flow's patience for a sentence. If it runs out, the
error propagates and the flow fails, which is the wanted behaviour: nobody said
anything, and that is worth reporting rather than waiting on.

## Recognising what was said

```a11flow
  nodes scratch  # local nodes that are never sent to the client

  transcribe = run transcribe_audio(
    asr_options: asr, audio: mic.audio | packb
  ) via scratch  # <- never send transcription outputs to the client directly
  skip transcribe.events
```

This is the line the whole page is about. `mic.audio` is the client's capture
stream and `audio:` is the recogniser's input port, and putting one into the
other is the entire instruction. A hundred-odd buffers a second flow from a
`call` step's output straight into a `run` step's input, and:

* they are never assembled into a value anybody holds;
* they are never rendered as text, because there is no text to render;
* they are never seen by a model, and cost nothing in a context window.

`| packb` says they travel as `application/x-msgpack` rather than JSON. It is a
no-op when the producer already wrote MessagePack, so it is safe in front of a
port that wants packed bytes whatever is upstream.

`nodes scratch` and `via scratch` are the other half of "nothing in the middle is
read by anybody". A `nodes` block gives the steps inside it a
[`NodeMap`][a11.nodes.async_node.NodeMap] of their own, so their ports are not in
the session's node map and the peer that dispatched the flow neither sees them nor
receives their fragments. Transcription fragments are for this flow; the client
gets the sentence.

Without a reader for transcription errors, omitting `try` allows unhandled
errors to propagate immediately to the caller rather than failing silently.

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

`| group EXPR` is `batch` with a question instead of a count: values gather into a
list, and the list closes when the expression holds of the value just added. So
fragments accumulate until one of them ends in a full stop, a question mark or an
exclamation mark — and that list, joined, is a sentence. `| first 1` takes one of
them and the pipeline is over.

`said` is a node of the flow's own because the sentence is wanted **twice** — by
the client, on `sentence`, and by the model, as the question. A node is how a
stream gets two readers: reading one stream twice would hand each reader half of
it, because a node has one cursor. It is `in scratch`, so the only copy that
crosses the wire is the one on `sentence`.

## Stopping, once there is something to answer

```a11flow
  # synchronisation point: wait until we have a sentence, then stop audio capture
  {"command": "stop"} -> mic.control_events after said
```

This is the only ordering statement in the file. Everything else in a flow's body
runs at once — **steps run concurrently, and order comes from the data** — so
`after` exists for the cases where order is the point. Here it is: the microphone
should close when there is a sentence, and not before.

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

`TYPE{...}` builds a value of a named type — the sentence becomes an
`a11.sdk.Interaction`, validated into the type, so a field that will not fit is an
error here rather than a surprise inside the model call. Which types exist is the
host's decision: a dotted name is a tag the running process's serialisation
registries resolve, and a flow cannot import anything.

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

`earlier then asked` is one stream and then the other: the turns so far, then the
question just heard. Two statements writing to the same node would interleave by
arrival, which is fine for fetched pages and wrong for a conversation — `then` is
how a flow says which comes first, and it is what makes a multi-turn chat
expressible at all.

`forward headers "x-a11-llm-*"` passes the caller's own headers on to the step
that needs them, so which provider and which model stay the caller's decision and
this file never mentions either.

`interact.text_output -> reply` is the answer, streamed to the client token by
token as the model writes it. And `turn` hands back what to remember: the
question, then the answer and any tool interactions the model made on the way to
it. The client appends those to the conversation and hands its node back as
`history` next time round.

## The whole thing

```a11flow
--8<-- "intellij-plugin/src/main/resources/flows/interact-on-full-sentence.flow"
```

Twenty-odd statements, more comment than code, and no deployment. Read as a list
of what it decided, it is one line per decision: who runs each step, what goes
where, what is not interesting, what stays local, and the single place order
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

## Four shapes worth knowing about

The following patterns cover common extensions to a composition.

**Work on several values at once.** A `map` whose expression is expensive — a
coercion, a round trip through the host — may say how many values it may have in
hand. What follows still reads them in the order they arrived, so this can be
added to a pipeline nobody else changed:

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

`| fold 0 as total, total + it` is the general form, and `| scan` is that same
fold with every value it passed through published rather than only the last —
which is how a stream whose meaning depends on what came before it is written:

```a11flow
lines | scan 0 as n, n + 1 -> numbered
```

`| window 2` is `batch` with the lists overlapping, for a question about
neighbours rather than about groups — a pattern spanning two lines is invisible
to a `batch`, because a boundary falls somewhere and half the matches fall on it.

`| timeout 30s` and `| pace 100ms` are the two stages about *when* a value
arrives rather than what it is.

## Where to go from here

The rest of the language — durations and arithmetic, `repeat`, `for`, `if`,
`try`/`wait`/`fail`, `log`/`logf`, `match`, `strformat`, and sandbox limits
— is in the [Flow language reference](../api/flow.md), and `a11.flow.REFERENCE` is
the same thing sized for a prompt.

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
