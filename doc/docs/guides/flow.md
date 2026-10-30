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
stream into its open `pages` port, so it can begin reading the first page
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
  out turn:     a11::sdk::Interaction stream "What to remember of this turn."
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
```

`run` executes a handler registered with the process running the flow. `call`
dispatches through the flow's attached stream. Because this composition runs on
a gateway while the microphone belongs to the client, capture uses `call`.

Unbound call outputs are drained automatically. Use `-> _` when a pipeline must
execute but its result is not needed: `pages | map summarise(it) -> _` processes
every page and discards the results.

The `timeout` bounds the wait for a sentence. Expiry propagates as a flow
failure.

## Recognising what was said

```a11flow
  nodes scratch

  transcribe = run transcribe_audio(
    asr_options: asr, audio: mic.audio | packb
  ) via scratch
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

  string said = take (transcribe.transcription_pieces
    | group std::endswith(std::trim(it), [".", "?", "!"])
    | first 1
    | map std::trim(std::join(it, " ")))
  said -> sentence
```

`| group EXPR` gathers fragments until the newest one ends a sentence.
`take` captures that sentence once as a string. Both the client output and the
model can use `said` without another node or another stream read.

## Stop capture after the sentence

```a11flow
  {"command": "stop"} -> mic.control_events after said
```

This is the only ordering statement in the file. Everything else in the flow
body runs concurrently, with data dependencies providing synchronization.
`after` closes the microphone only once a sentence exists.

## The conversation

```a11flow
  earlier = node(history)

  asked = a11::sdk::Interaction{
    role: "user",
    content: [std::to_chunk({
      role: "user",
      content: [{type: "text", text: said}]
    })]
  }
```

`node(history)` attaches to the caller's conversation by node id. `asked` is one
validated interaction value, reused by the model and the `turn` output.
`a11::sdk::Interaction` names a host type; its wire tag remains
`a11.sdk.Interaction`.

## Generate and return the answer

```a11flow
  interact = run interact_with_llm(
    interactions: earlier then asked,
    config: {}
  )
      forward headers "x-a11-llm-*"
      via scratch

  interact.text_output -> reply

  asked then interact.new_interactions -> turn
```

`earlier then asked` concatenates the prior turns and the new question in that
order. Direct writes from two statements would interleave by arrival; `then`
preserves conversation order.

`forward headers "x-a11-llm-*"` passes the caller's headers to the step that
needs them. The caller selects the model and provider without changing the
composition.

`interact.text_output -> reply` streams the answer to the client as the model
writes it. `turn` returns the question, answer, and tool interactions for
conversation state. The client appends them to the conversation and passes its
node back as `history` on the next turn.

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

[`register`][a11.flow.register] compiles and publishes every named root implementation in the source as
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

## Model-authored compositions

[`a11.sdk.flow_tools`](../llm-sdk/flow-skill.md) exposes three tools to a model:
`flow_actions` lists composable actions and their ports, `flow_check` compiles a
flow without running it, and `flow_run` executes it.

Once dispatched, the runtime pipes intermediate values between actions. Large
or high-rate values such as audio buffers, fetched pages, and transcript
fragments stay out of the model context. In this example, the model receives one
completed sentence and the caller receives the streamed answer.

## Values, assignment, and loop termination

Capture an item with `take`, then use the value in ordinary expressions.
Assignments run once when their dependencies are ready, including unused
initializers. Later reads observe the preceding assignment in structured source
order; independent actions still overlap.

```a11flow
from std import startswith

flow between-markers {
  in lines: string stream
  out body: string stream
  out count: int

  bool inside = false
  int matched = 0
  for line in lines {
    if startswith(line, "BEGIN") {
      inside = true
    } else if startswith(line, "END") {
      inside = false
    } else if inside {
      line -> body
      matched = matched + 1
    }
  }
  matched -> count
}
```

Child scopes publish updates when they finish. Sequential loop passes carry the
updates forward; a read after the loop sees the final value. Parallel passes
cannot update the same outer variable without explicit supported ordering.

A trailing `until` or `while` tests after the entire pass finishes:

```a11flow
flow first-success {
  in urls: string stream
  out text: string

  bool found = false
  for url in urls {
    page = try run fetch(url: url)
    std::status outcome = wait page
    if outcome.ok {
      page.text -> text
      found = true
    }
  } until found
}
```

The successful pass finishes and its text is included. Stopping the loop releases
its reader; the producer keeps its ordinary ownership and lifetime.

## Common composition patterns

The following patterns cover common extensions to a composition.

**Work on several values at once.** A `map` that performs a coercion or host
round trip may bound its concurrent values. Downstream stages still receive
results in input order:

```a11flow
urls | map fetch_page(it) parallel 8 -> bodies
```

**Handle per-value failures.** `try` on a stage drops values the stage cannot
process, and `into` sends their status records to a destination:

```a11flow
docs | try map it as Order into rejected -> orders
```

**Merge by arrival.** `interleave` reads several streams concurrently and emits
each value on arrival. A slow source does not block a fast one:

```a11flow
std::interleave(llm.text_output, tool.progress) -> shown
```

**Whichever subject completes first.** `wait first of` holds until one of several
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

## Related references

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
highlights `.flow` files and Flow source inside string literals. [Checking flows
from a toolchain](flow-tooling.md) documents machine-readable responses for CI
and editor integrations.

## Use operation results inside expressions

Select an output directly when a call is used once:

```a11flow
flow shrink {
  in request: object
  out result: object
  nodes scratch
  (run llm_context_compress(request: request) via scratch).result -> result
}
```

`run`, `call`, `node`, `status`, `wait`, and `close` can appear inside
expressions. Parenthesize an invocation before selecting one of its outputs:
`(run fetch(url: url)).metadata`. Keep the handle when several consumers use it.

Name a value explicitly when it is used in several places:

```a11flow
page = run fetch(url: url)
metadata = take page.metadata
report = {title: metadata.title, source: url}
```

`take` returns the next item as soon as it arrives. `try take` returns an option
on normal EOF; upstream errors still fail. `consume` waits for the stream to
end and requires exactly one item. `collect` returns a list of all items.

```a11flow
if first = try take candidates {
  chosen = run inspect(candidate: first)
  chosen.report -> reports
}
```

Optional binding evaluates its option once and binds the payload only inside
the success branch. It tests presence, so null, false, zero and empty payloads
enter that branch. Bind a computed option with `if value = candidate` to reuse
it. The name must be fresh and cannot shadow an enclosing variable. Else-if
initializers run only after preceding branches were not selected. Explicit
`candidate.has_value` and `candidate.value` access remains available; an absent
option's `.value` raises an error. Ordinary records need a validated
`std::option(...)` conversion before optional binding.

An operation activates when its containing expression is demanded. `and` and
`or` short-circuit, and skipped branches do not start their operations.
Independent statements and argument feeds remain concurrent. Bind a call when
several consumers need its identity or several outputs.

## Order transfers through continuation sinks

`a -> b -> out` associates to the right and forwards all of b, then all of a.
Pipelines bind more tightly than arrows; a terminal comma list broadcasts.
Bind a transfer to share its continuation without repeating it:

```a11flow
tail = asked -> turn
interact.new_interactions -> tail
```

Independent attachments interleave after the prefix. Handle completion includes
all registered attachments. A handle is a destination or completion capability,
never ordinary payload or a source. Virtual sinks add no intermediate node or
queue. [Continuation sinks](flow-continuation-sinks.md) describes grouping,
reservations, failure and cancellation.

## Own work through scopes

Each activated anonymous operation belongs to the executing flow, block, loop
pass, or stage invocation. Releasing its last reader drains remaining outputs.
The scope joins its activated children before completing and releases its
local node allocations. Attachments to existing nodes retain their external
owner. Completed task registrations are removed while the scope runs.

`wait first of` accepts calls, node-backed ports, nodes, and named block, loop,
or wait completions in one list. It returns the winning zero-based index.
Node observation leaves the read cursor and writer state untouched; it does
not read the first item or close the node. Duplicate candidates share an
observation. Already completed candidates use list order; later completions
use the order observed under the execution monitor.

```a11flow
winner = take (wait first of try run primary(), try run backup())
```

The loser continues. The winner can feed another step immediately, while the
owning scope still joins the loser. A tolerated failure remains that operation's
outcome. Scope failure or cancellation cancels pending and running owned work;
Flow keeps the initiating status, message, and details. Child actions use the
existing cancellation request without a new reason parameter.

These ownership bounds limit retained bookkeeping at fixed concurrency. They
do not impose a deadline on an external action or a stream that never ends.
Use the existing timeout and cancellation facilities where a deadline is needed.

## Declare action interfaces and import modules

A declaration supplies the same inputs, outputs, descriptions, and headers as
a flow definition, with no executable statements:

```a11flow
declare flow render {
  describe "Render a report"
  header "x-tenant" as tenant default "public"
  in data: object required
  out text: string stream
}
```

Declarations participate in port checking, completion, and schema analysis.
Compatible contributions combine into the strictest known interface. A concrete
implementation can narrow a declared type and add members. It cannot broaden
a declared payload, change stream cardinality, weaken requiredness, omit a
declared member, or change a header default's bytes. Added required inputs need
feeds or autofill. Relations unsupported by available metadata remain unproven
offline and must be proved when binding an executable closure.

Before execution, Flow resolves and pins every transitive dependency, including
calls inside untaken branches. A used unresolved declaration, ambiguous body,
or incompatible implementation fails before headers, nodes, feeds, or handlers
activate. Unrelated unused declarations do not need implementations. A registry
schema alone supplies no local `run` handler; `call` also requires a dispatch
route. Preflight cannot prove a remote peer's future availability.

The root module is the source passed to `loads`, `load`, or the native compiler.
Its named flows can be registered and its local types used without `exported`.
Imported modules are private by default. Put `exported` after a flow, declaration,
or struct name to expose it to importers. Imports create namespaces at module
scope or directly inside an implemented flow. They are hoisted within that
lexical scope; nested imports in branches or loops are invalid.

```a11flow
import "report.flow" as report

flow publish {
  in data: object
  out text: string stream
  (run report::render(data: data)).text -> text
}
```

Imported implementations capture their defining module's private dependencies.
Imports do not re-export a dependency. Interfaces exposed to importers use public
types. Root interfaces can use their own structs without export attributes.
Cycles and diamond imports share canonical module snapshots. Compatible repeated
interfaces are allowed; distinct executable bodies remain ambiguous. Identical
struct contributions of the same identity are accepted; different module-local
struct identities remain distinct.

Approve source roots explicitly when loading filesystem dependencies:

```python
program = flow.load("pipelines/main.flow", source_roots=["pipelines"])
```

Imports are relative to their defining file and cannot read outside those
roots. No approved roots means no filesystem imports. Symlinks are rejected
when traversing imported paths, including links whose targets are inside a root.
The filesystem importer uses POSIX directory handles; other platforms report an
unavailable importer. Explicit in-memory sources work on every platform. For generated or embedded
source, supply an in-memory map:

```python
program = flow.loads(
    'import "library" as lib\nflow main { (run lib::work()).result -> _ }',
    "root",
    modules={"library": 'flow work exported { out result: int\n 42 -> result }'},
)
```

`register_all` publishes every named root implementation, including flows without
`exported`. Imported implementations remain in the linked closure and are not
automatically registered. Interface-only declarations are not registered either.
The same document needs explicit exports when imported by another module,
including an import back to the root through a dependency cycle.

## Organize a public module interface

Modules are private by default. Expose a module and selected members explicitly:

```a11flow
mod reports exported {
  struct Summary { text: string required }
  declare flow build { in source: string
    out result: Summary }
  export Summary
  export build as summarize
}
```

`reports::summarize` is a public alias of the declared contract. Types and flows
keep their identities through aliases and reexports. A `declare mod` contains
contracts and complete structs; implementations can be supplied by the host.
`declare flow company::search::find` creates the declared module prefixes.

The reserved `std` module is always available. Import selected names to shorten
expressions: `from std import join, trim`. Imports scope to a module or flow;
`import "reports.flow"` uses the filename stem as its namespace. Dot access
continues to select runtime fields and action ports.

## Separate concurrent execution phases

`wait all` waits for all preceding work in its executable scope before
activating following statements. Work within each phase runs concurrently:

```a11flow
flow prepare {
  run warm_cache()
  run load_configuration()
  wait all
  run serve() deadline std::now() + 30s
}
```

Both preparation calls complete before `serve` activates. Its arguments,
initializers, and eager deadline expression also wait for the barrier. Completion
includes input feeds, required output draining, owned calls, and child scopes.
An ordinary failure prevents the next phase; tolerated failures retain their
ordinary behavior.

A barrier in a block or branch affects only that scope. A loop applies it
independently to every iteration, including parallel iterations. Sibling scopes
continue independently. The explicit-subject form `wait all of a, b` observes
only those subjects and does not establish execution phases.

A stream alias alone does no work. A barrier does not exhaust aliases, collect
streams, add buffering, or cancel producers. Keep a producer and its required
consumer in the same phase:

```a11flow
flow consume {
  work = run read_rows()
  work.rows -> _
  wait all
  log "Rows consumed"
}
```

Moving `work.rows -> _` after the barrier would make completion depend on work
that cannot activate. Native diagnostics report provable cross-phase cycles.
Bare `wait all` accepts no `timeout`, `deadline`, or `after` modifier and cannot
be bound as a value. Builders expose it as `Statement.wait_all()` in Python and
`Statement::WaitAll()` in C++.

## Bound execution time

A duration timeout and an absolute deadline can be named like other values:

```a11flow
from std import now, time, duration

flow search {
  in query: string
  out text: string
  duration timeout = 30s
  time deadline = now() + 1m
  results = run find(query: query) timeout timeout deadline deadline
  results.text -> text
}
```

A deadline expression is eager and fixed while other arguments become ready.
`deadline now() + 30s after prepared` evaluates the whole expression after
`prepared` finishes. If both limits are present, the earlier one applies.

## Maintain generated modules

The editable document API uses the same native parser, linker, and schema
analysis as execution. Handles carry a document identity and revision; stale or
overlapping edits fail atomically. Compiled programs retain immutable snapshots.

```python
from a11.flow import Expression as E, FlowBuilder, ModuleDocument, StructBuilder, Type

module = ModuleDocument("reports")
row = StructBuilder("Row")
row.exported = True
row.field("title", Type("string"), required=True)
module.insert_struct(row)

render = FlowBuilder("render")
render.exported = True
render.port("text", Type("string"), output=True)
render.pipe(E.literal("ready"), E.reference("text"))
module.insert(render)
program = module.compile()
```

`module.upsert_declaration(action_schema, symbol=owned_handle)` synchronizes one
owned declaration. Unrepresentable metadata returns diagnostics. Unchanged
source tokens, comments, and whitespace survive synchronization, and repeated
synchronization produces no edits. `effective_schema(name, modules=...)` queries
the merged interface. `rename_import` updates lexically bound namespace uses;
`rename_in_graph` prepares validated edits and updated sources for the supplied
module graph, reporting the boundary for external users of an exported API.

The equivalent C++ surface lives in `a11/flow/document.h`:

```cpp
a11::flow::ModuleDocument module("reports");
a11::flow::FlowBuilder render("render");
render.interface().exported = true;
a11::flow::syntax::TypeExpression text;
text.name = "string";
ABSL_RETURN_IF_ERROR(render.AddPort(
    "text", text, a11::flow::syntax::PortDirection::kOutput));
ABSL_ASSIGN_OR_RETURN(auto literal, a11::flow::Expression::Literal("ready"));
ABSL_ASSIGN_OR_RETURN(auto output, a11::flow::Expression::Reference("text"));
ABSL_RETURN_IF_ERROR(render.AddPipe(literal, output));
ABSL_ASSIGN_OR_RETURN(auto change, module.Insert(render));
```

`DeclareAction` in `a11/flow/runtime.h` converts a native action schema into
interface and struct descriptors for `UpsertDeclaration`. File access and
registry bindings stay in the host adapters.


Import a document with `import "library.flow"`, then select its symbols with
`from library import render, Report as LocalReport`.
They bind public source members at module scope or directly inside an implemented
flow. A same-name callable declaration constrains the imported implementation;
compatible contracts share their strictest interface. Conflicts report the
local requirement and link the contributing sources. Distinct executable bodies
under one binding are ambiguous when used; aliases keep them independently
addressable.

An implemented flow may contain `declare flow` and `struct` declarations. These
names are hoisted within the flow and remain private to it. A local callable
contract can resolve a module implementation, a directly imported symbol, or a
registry action; unresolved used contracts fail preflight. Each local struct has
a scoped type identity. Executable flow definitions belong at module scope, and
an exported imported-module interface cannot expose a local struct. Completion,
navigation, document symbols and formatting account for local declarations and
direct imports.


`log`, `logf`, `fail`, and `abort` evaluate their value arguments before acting.
Reading an input stream, an action output, a completion status, or an initialized value
derived from those supplies a data dependency. For example,
`logf "result: %s" (work.output | consume)` waits for the consumed output without
an `after`. Constant-only arguments, including initializers containing only constants, need
an `if`, a loop body, or an explicit `after`. Reading a node's `id` supplies no
stream dependency. `cancel` still requires a condition, loop body, or `after`;
naming its target does not wait for that action.


## Forward a stream prefix and clean up

`take while CONDITION` forwards values while the condition is true.
`take until CONDITION` forwards values until the condition becomes true.
Both exclude the first value that ends the prefix. Predicates use `it` and
run one at a time in input order. The stages release their reader at the
boundary; the producer continues under its ordinary scope ownership.

```a11flow
flow {
  stdin = run read_stdin()
  echo = run write_stdout()

  forwarded = stdin.lines | take until std::lower(it) == "/exit"
    | map std::strformat("%s\n", it) -> echo.content

  after forwarded {
    {"command": "stop"} -> stdin.control_events
    run write_stderr(content: "Input finished.\n")
  }
}
```

The echo receives each line as it arrives. `/exit` ends forwarding and closes
its destination when the last writer finishes. The `after` block then requests
that stdin stop, so the flow can finish without waiting for terminal EOF.

`after a, b { ... }` waits for both completion subjects before activating its
body, including argument evaluation and anonymous calls. Its statements run
concurrently; use data dependencies or another `after` inside the block when
cleanup operations need ordering. Local bindings stay inside the block.
`cleanup = after done { ... }` names the group's completion, including its
children. `cleanup = try after done { ... }` tolerates a body failure and keeps
its status available through `status cleanup`.

These groups follow ordinary scope failure and cancellation. Cleanup that must
run during parent failure belongs to the host action's lifetime management.
A `try take while` or `try take until` stage drops an item whose predicate fails
and continues with the next item. Source failures propagate. Neither stage
accepts `parallel` or `unordered`, because evaluating later predicates could
activate work beyond the stopping boundary.


`forwarded = SOURCE | STAGE -> DEST` names a pipeline's completion. It is
shorthand for `forwarded = { SOURCE | STAGE -> DEST }`: completion includes
all destinations and anonymous children, with normal failure propagation.
The source may span lines and may be a literal, a stream, or an operation's
output. Suffix `after` gates the enclosed pipeline as usual. Write
`rows = SOURCE | STAGE` for a stream alias and `row = take rows` for one item.

## Invoke flows inside pipelines

Use `| run` to apply a flow or a declared action to a whole stream. Use `| call`
to dispatch that invocation to the peer. Both forms accept ordinary call
modifiers, including headers, `after`, `timeout`, `deadline`, and node placement.

```a11flow
flow clean_lines {
  in input: string stream
  out output: string stream
  input | map std::trim(it) | where std::len(it) > 0 -> output
}

flow main {
  in lines: string stream
  out text: string stream
  lines | run clean_lines() | first 10 -> text
}
```

The invocation starts when the pipeline is read and runs once per activation.
Readers of a shared pipeline share that invocation. A loop creates one
activation per pass; a skipped branch creates none. A custom stage can filter,
aggregate, keep state, and invoke other actions. Per-item invocation remains
`lines | map (run process_line(input: it)).output`.

Exactly one input must remain after named arguments, counting optional inputs
as well. Every other configuration input must be supplied explicitly. Exactly
one output must be declared. Use an adapter flow when an interface has several
outputs or does not identify a unique input. `lines | run prefix(label: "> ")`
feeds the sole remaining input and exposes the unique output. The compiler
requires a known interface, including for external actions.

Streaming and unary ports retain their contracts. A unary input validates its
cardinality before forwarding a value: a required input needs exactly one item,
and an optional input permits zero or one. No list expansion, collection, or
first-item selection is implicit.

Stopping a downstream reader with `first` retains ordinary draining and child
ownership. The enclosing scope waits for the invocation, its feeds, and owned
children; a failure after partial output still fails the scope. `try run` and
`try call` tolerate invocation completion just as in ordinary calls. Errors
reading an aborted output still propagate. A surrounding `try` block handles
the complete pipeline. A `try` pipe tolerates transfer errors; combine it with
`try run` or `try call` to tolerate invocation failure too. This differs from
`try map`, which can drop an individual item.

The plan describes the selected ports and whole-stream activation. Hover and
completion show the adapter interface; both dispatch verbs are available, with
`run` preferred. The native builder provides `Expression.pipeline_invoke`.

An unresolved qualified call such as `run missing::work()` is a compiler error
at the target name. Declare or import the callable contract before using it.


Single-item transport is enabled by default. Use
`flow.loads(source, single_item_slots=False)` for baseline execution. See
[single-item transport](flow-single-item-slots.md) for proofs, eligibility,
explanation output, baseline comparison and measurements.
