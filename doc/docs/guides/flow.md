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

[Flow](../api/flow.md) is those forty lines as **text**. It compiles at runtime,
from a string, wherever it lands: nothing is deployed, and whoever wrote it does
not have to be a person. This page builds the composition above one statement at a
time.

Two things about it are worth saying before the first line, because they are the
reason it is a flow rather than code:

* **A tool-calling model could not do this at all.** `capture_audio` produces
  `a11.sdk.AudioBuffer` values 256 frames at a time — well over a hundred a
  second at any usual sample rate. There is no version of an LLM tool loop where
  those pass through the model.
* **Nothing in the middle is read by anybody.** The buffers go from one step's
  output port to the next step's input port inside the runtime, and so do the
  transcript fragments. What crosses the wire is one sentence and one answer.

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

There is deliberately no `try` here. Nothing in this flow reads a transcription
failure, and a `try` whose status nobody looks at turns a loud failure into a
silent one — the flow would carry on with no sentences and no reason given.

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

`node(history)` **attaches** to a node somebody else owns, by the id that came in
on the `history` port. That is how a flow reads a stream it did not make. Each
interaction arrives as the type it was written as, and nothing here takes one
apart, so nothing here can lose a field of one.

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

Nothing about the gateway changed to make that work. It was already serving these
actions; the composition arrived as an argument.

## …including by a model

A model with three tools and a task that needs all three calls them one at a
time and reads every intermediate result on the way. It pays for each of them
twice — once to read it, once to quote it into the next call — and the values it
copies are only as accurate as its copying.

[`a11.sdk.flow_tools`](../llm-sdk/flow-skill.md) hands it the alternative as
three more tools: `flow_actions` says what may be composed and what each action's
ports are called, `flow_check` compiles a flow without running it, and `flow_run`
runs one. `flow_tools.get_system_prompt()` is the text that teaches it when to
bother.

The point is not that a model can write Flow. It is what the model then stops
having to do. Chaining ports programmatically means the intermediate values never
enter the context: the transcript fragments, the fetched pages, the file listings.
A model that composes instead of orchestrating spends its turn on the part that
needed a model — deciding *what* to build — rather than on copying one tool's
output into the next tool's input, which is work it is expensive at and not
especially good at.

That is also the honest reason this particular flow exists. Two of the three
things it does are ones a tool loop cannot reach:

* **impossible**: raw audio buffers are not something a model deals in;
* **impractical**: they arrive faster than a hundred a second, and a transcript
  in fragments is not much better;
* **and cheap anyway**: the model in this composition reads exactly one sentence.

## Where to go from here

The rest of the language — durations and arithmetic, `repeat`, `for`, `if`,
`try`/`wait`/`fail`, `match`, `strformat`, and what a flow deliberately cannot do
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
