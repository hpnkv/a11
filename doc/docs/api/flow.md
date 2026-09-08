# Flow language

Flow composes actions into a new action. A host loads a document with application
code or receives and checks it at runtime, then resolves it against the current
action registry. Documents cannot import code or call capabilities outside that
registry.

This page is the language reference and Python API. Start with
[Compose actions through streamed data](../guides/flow.md) for a guided example.
`a11.flow.REFERENCE` provides a compact version suitable for model prompts.

## Find a topic

<link rel="stylesheet" href="../assets/navigation-cards.css">
<nav class="a11-card-nav" aria-label="Flow language topics">
  <a href="#one-flow-read-from-the-top">
    <strong>Read a complete flow</strong>
    <span>See declarations, calls, loops, pipes, and skipped outputs.</span>
  </a>
  <a href="#what-a-port-holds">
    <strong>Declare ports and values</strong>
    <span>Choose types, media types, cardinality, and requirements.</span>
  </a>
  <a href="#descriptions">
    <strong>Describe and construct values</strong>
    <span>Document a flow and create registered structured types.</span>
  </a>
  <a href="#action-composition">
    <strong>Compose actions</strong>
    <span>Run locally, call remotely, and connect streamed ports.</span>
  </a>
  <a href="#handle-expected-failures">
    <strong>Handle expected failures</strong>
    <span>Use status values and recovery paths without hiding errors.</span>
  </a>
  <a href="#loops-branches-and-state">
    <strong>Control work and state</strong>
    <span>Use bounded loops, branches, variables, and synchronization.</span>
  </a>
  <a href="#flow-boundaries-and-sandbox-limits">
    <strong>Understand the boundary</strong>
    <span>See what source can access and how the host limits it.</span>
  </a>
  <a href="#the-tables-as-data">
    <strong>Inspect language data</strong>
    <span>Read the vocabulary and stable diagnostic tables.</span>
  </a>
  <a href="#compiling">
    <strong>Compile and run</strong>
    <span>Load flows, inspect plans, and execute programs.</span>
  </a>
  <a href="#diagnostics">
    <strong>Report diagnostics</strong>
    <span>Consume syntax and resolution failures from Python.</span>
  </a>
</nav>

## Complete flow

```a11flow
flow research {
  describe "Search, read the best hits, and answer from them."

  in  question: string required
  out answer:   string
  out sources:  string stream

  header "x-a11-deadline" as deadline

  search = run web-search(query: question, limit: 3)

  brief = run llm-summarize(question: question)
      with "x-a11-deadline": deadline

  nodes fetched {
    for hit in search.hits parallel 2 {
      page = run web-fetch(url: hit.url)
      hit.url -> sources
      page.text | truncate 200 -> brief.pages
      skip page.bytes
    }
  }

  brief.summary -> answer
  skip search.debug
}
```

`x = run action(port: source)` runs a local action and binds its ports to `x`.
`x = call action(...)` dispatches an action on the flow's attached stream.
`source -> destination` pipes a stream into a node.

`brief` starts before pages arrive. The loop feeds its streaming `pages` port,
which A11 closes after the loop's last writer finishes. Steps run concurrently;
use `after`, `wait`, or `drain` to require an order.

A port carries one value unless declared `stream`, and is optional unless
declared `required`. Keywords accept lower or upper case, such as `for` and
`FOR`. Mixed-case words are identifiers.


## Port values

```a11flow
in  question: string required
in  frames:   list[a11.NodeFragment] stream
out audio:    a11.sdk.AudioBuffer stream
out raw:      "application/x-msgpack"
```

Besides the built-in names — `string`, `text`, `number`, `integer`, `bool`,
`object`, `json`, `list`, `bytes`, and `any` — a type may use an unquoted
serialisation tag such as `a11.sdk.AudioBuffer`. Dotted names are tags and are
preserved without importing the module that defines the type. Brackets specify
container contents, as in `list[string]` and `list[a11.NodeFragment]`. A quoted
type is a media type that describes the representation.

## Descriptions

Descriptions help callers, including models, decide when to use a flow. They may
follow a declaration or use a multiline string:

```a11flow
flow research {
  describe """
    Search the web, read the best hits, and answer from them.

      Costs one search and up to `budget` fetches.
    """

  in  question: string required
    "What to find out — as long as this needs to be, on its own line."
}
```

A `"""` string preserves line breaks after dedenting. Dedenting removes a blank
first line, a whitespace-only final line and its preceding break, and the common
indentation from all remaining lines. Additional indentation on individual
lines remains. Escapes match ordinary strings.

A description may also appear alone on the line below a port, header, or
`describe` declaration. A string followed by another token, such as
`"a literal" -> out`, remains a statement.

`a11 flow fmt` indents a description under its declaration and lines up the
columns of a run of declarations around it.

## Constructing typed values

A flow can construct a registered type such as `Interaction` or `AudioBuffer`
from fields with `TYPE{...}`:

```a11flow
a11.sdk.Interaction{
  role: "user",
  content: [to_chunk({"role": "user", "content": [{"type": "text", "text": said}]})]
}
```

`EXPR as TYPE` performs the same conversion and supports generic types such as
`pieces as list[string]`. Both forms validate the value, apply defaults, and
report incompatible fields.
`to_chunk` and `from_chunk` create and read a
[`Chunk`][a11.data.types.Chunk]. Content-bearing types store their content in
chunks.

Available types come from the host's serialisation registries. A flow cannot
import types. `TYPE{...}` is unavailable where `{` opens a block, including an
`if` condition or a `for` source. Parentheses permit a typed construction in
these positions: `if (T{done: true}).done {`.

## Action composition

### Local runs and remote calls

`run some-action(...)` executes a handler registered in the local process.
`call some-action(...)` dispatches the action on the flow's attached stream.
The verb selects the execution location:

```a11flow
search = run web-search(query: question)
llm    = call interact_with_llm(...)
```

`run` requires a local handler. `call` requires only a local schema for
resolution and always dispatches to the peer, even when a local handler has the
same name.

The deployment determines which verb is available.
[`flow_actions`][a11.sdk.flow_tools] reports this through each entry's
`runnable` field.

`try` goes in front of either: `try run`, `try call`.

Either verb may name another flow in the same file without a registry entry:

```a11flow
flow ask-twice {
  in  question: string
  out answers:  string stream
  first  = run ask(question: question)
  second = run ask(question: question)
  first.answer then second.answer -> answers
}

flow ask {
  in  question: string
  out answer:   string stream
  said = run answer-question(question: question)
  said.text -> answer
}
```

A program is a set of flows, so declaration order does not affect execution.
Compilation checks calls between flows against their declared ports. This keeps
reusable compositions in one source file with a single entry point for
`flow_run` and the gateway.

### Discarding stream values

An undrained output port stalls its producer. `skip page.bytes` consumes one
value without retaining it. The runtime drains declared outputs that the flow
does not reference.

`skip n port` removes values from the node before fan-out. Every reader starts
after those values. In comparison, `| drop 1` trims only its pipeline:

```a11flow
rows = run read-csv(path: path)
skip 1 rows.lines
rows.lines | count -> data-rows
rows.lines -> passed-through
```

Counts on one node are additive. `skip 1 x` and `skip 2 x` remove three values,
independent of statement order, because compilation sums the counts. The target
must be a port or node; pipelines are reader-specific.

`-> _` executes a pipeline and discards its result:

```a11flow
pages | map summarise(it) | logf info "summarised %s" it.url -> _
```

Every page is summarised and every line is logged, but the result is discarded.
`_` is valid only as a destination; it cannot be bound or read. `_ = node()`,
`_ | count -> n`, `drain _` and `in _: string` are each refused while the flow is
compiled. It may stand beside a real destination (`a -> b, _`), where it adds a
reader that discards its values.

### Putting a stream back together

`| group EXPR` is `batch` with a question instead of a count: values gather into
a list, and the list closes when the expression holds of the value just added.
For example, it can assemble partial utterances into sentences:

```a11flow
pieces | group ends-with(trim(it), [".", "?", "!"]) | map trim(join(it, " "))
```

Any partial final group is emitted when the stream ends.

`| then SOURCE` concatenates this stream with the source that follows.

```a11flow
history then asked -> llm.interactions
```

`then` and `where` may omit the pipe: `history then asked` and
`hits where it.ok`. Every other stage requires `|`, which distinguishes stage
names from identically named ports.

`| flatten` expands a stream of lists into their values.

```a11flow
pages | map it.lines | flatten -> lines
```

Lists are expanded and other values pass through unchanged, so `flatten` also
accepts a mixed stream.

`| window N` is `batch` with the lists overlapping: one list of the last `N`
values per value, once `N` have arrived.

```a11flow
lines | window 2 | where contains(join(it, "\n"), needle) -> hits
```

Unlike `batch`, a window can detect patterns that span arbitrary batch
boundaries. It retains at most `N` values, so memory use remains bounded for an
unending stream. A stream shorter than `N` produces no window, while `batch`
may emit a shorter final list.

`interleave(a, b, ...)` reads all sources concurrently and emits each value on
arrival. `zip` reads its sources in step and emits one tuple per round:

```a11flow
interleave(llm.text_output, tool.progress) -> shown
```

Values retain their arrival order across sources. The combined stream ends when
every source ends; a source failure ends it with that status.

### Reducing a stream to one value

Arithmetic reducers calculate one result from a complete stream:

```a11flow
orders | sum it.price -> revenue
runs   | avg it.elapsed -> typical
hits   | max it.score -> best
```

`sum`, `min`, `max`, and `avg` read the whole stream and yield one value. With no
expression they use each value directly; `| sum it.price` is equivalent to
`| map it.price | sum`. Durations add and average as durations. For an empty
stream, `min`, `max`, and `avg` emit no value, while `sum` emits `0`.

`| fold` defines a custom reduction:

```a11flow
orders | fold 0 as total, total + it.price -> revenue
```

The name is bound to the previous accumulated value and `it` to the current
input. The starting value is a literal, not an expression: otherwise
`fold 0 as total` could be parsed as a cast of `0` to a type called `total`.
A **record** literal is allowed because its braces remove this ambiguity.

### Carrying state along a stream

`| scan` uses the same syntax as `fold`. `fold` yields one value when the stream
ends; `scan` yields the current accumulator for each input value.

```a11flow
lines | scan 0 as n, n + 1 -> numbered
```

`scan` carries state forward for each stream value. `repeat` also carries state,
but rereads its stream from the start on each pass; `for` reads one value per
pass but does not carry state between passes. `scan` retains one state value,
not the complete stream.

The state may also be a record:

```a11flow
lines
  | scan {"inside": false, "line": ""} as s,
      {"inside": starts-with(it, "BEGIN") or (s.inside and not starts-with(it, "END")),
       "line": it}
  | where it.inside and not starts-with(it.line, "BEGIN")
  | map it.line
  -> body
```

The stage uses constant memory by retaining only one state value.

`| sort` puts a stream in order:

```a11flow
hits | sort by it.score desc | first 10 -> best
```

`sort` buffers the complete stream before emitting values. Comparison follows
`<`; `by` selects the comparison value, `desc` reverses the order, and equal
values retain their input order.

### Stream timing

Two stages control stream timing.

```a11flow
tokens   | timeout 30s  -> answer
requests | pace 100ms   -> to_api
```

`timeout` limits the gap between values. A longer gap ends the flow with
`deadline_exceeded`. Use `wait ... timeout` to limit an entire step.

`pace` delays values to enforce a minimum interval without dropping them. The
producer blocks when the buffer is full.

### Working on several values at once

A per-value stage can bound its concurrent work:

```a11flow
urls | map fetch_page(it) parallel 8 -> bodies
```

Downstream stages still receive values in input order. The parallel stage
reorders completed work before emitting it. Add `unordered` to emit results as
soon as they complete:

```a11flow
urls | map fetch_page(it) parallel 8 unordered -> bodies
```

Use `parallel` for substantial per-value work such as host round trips,
coercions, or large chunks. It adds overhead to simple field access. Stages that
gather or order values do not accept `parallel`.

### Text and time

`strformat("%s of %s", got, wanted)` uses printf conversions: `%s` for text;
`%d`, `%f`, and `%x` for numbers; flags and precision such as `%-8s` and
`%06.2f`; `%2$s` for a positional value; and `%%` for a literal percent.
`| strformat "fmt"` abbreviates `| map strformat("fmt", it)`.

Flow uses printf-style conversions because `str.format` reads attributes, which
could escape sandboxing for untrusted expressions. A printf conversion operates
only on supplied values. A conversion with no corresponding value remains
unchanged to expose the invalid conversion.

Durations are written the way a timeout is — `500ns`, `250ms`, `30s`, `2m`,
`1h`, and compounded as `1m30s500ms` — and are ordinary values. `now()` is the
clock, and the arithmetic is the arithmetic A11's own types allow:

```a11flow
started = node()
now() -> started
work = run slow-thing(input: pages)
done = wait work
let took = now() - started
strformat("took %s", took) -> log after done
```

Steps run concurrently, so source order alone does not delay a clock read.
`now() -> started` needs no barrier because it records the start. A clock read
that measures produced work requires `after`; otherwise the compiler reports
`flow.barrier.unordered-clock`.

An `after` applies to the complete statement, including arguments. In
`run act(p: now() - started) after done`, the argument is evaluated after
`done`.

`+` and `-` are the language's only arithmetic operators. A bare number beside a
duration counts as seconds; `seconds(d)` returns the numeric value. Subtraction
can produce a negative duration and does not use A11's infinite-timeout
convention. `-` requires spaces because `text-upper` is an identifier.

Formatting: `%s` renders a duration as `1m30s` and an instant as RFC 3339. A
unit in the parenthesised spec gives one number — `%(ns)d`, `%(us)d`, `%(ms)d`,
`%(s)d`, `%(m)d`, `%(h)d` — and `%(%H:%M:%S)s` or `%(epoch)d` formats an
instant.

`duration(x)` and `time(x)` parse the formats described above:

```a11flow
deadline = time(header-deadline)          # "2026-08-11T09:14:22Z"
budget   = duration(header-budget)        # "1m30s", or a number of seconds
if now() + budget > deadline { fail deadline_exceeded "not enough time left" }
```

These functions accept text from headers, JSON fields, or model output without
a separate format string.

Two statements writing to the same node interleave by arrival. Use `then` when
order matters, such as sending prior conversation turns before the current one.

### Reducing data before serialization

`| truncate 200` shortens each page before writing it to the summariser. Dropped
data is not serialized, sent to a peer, or included in model input. The same
applies to `| first 3`, `| where it.ok`, `| mime "text/*"`, and `| drop 1`.

### Value representation

`| packb` writes a value as `application/x-msgpack` instead of JSON. Existing
MessagePack chunks pass through unchanged, including their type tag. Other
representations are re-encoded.

### Header propagation

Headers carry call metadata such as model selection, identity, and deadlines.
Nested actions automatically receive their parent's `x-a11-` headers. Use
`forward headers` for other headers:

```a11flow
answer = run interact_with_llm(interactions: asked, config: {})
    forward headers "authorization", "x-tenant-*"
```

Names are forwarded unchanged, and `*` matches a family of names. Missing
optional headers are ignored. Use `with "header": expr` for computed values. A
`with` value overrides a forwarded header with the same name.

### Local step traffic

`nodes fetched { ... }` gives its calls a private
[`NodeMap`][a11.nodes.async_node.NodeMap]. Their ports and fragments remain
outside the session's node map. For example, four fetched pages can remain local
while one answer returns to the peer. A `run` step keeps its nodes local unless
it specifies `tee`; a `nodes` block also applies to `call` steps.

### Nodes of the flow's own

`x = node()` creates a stream that several loop passes can write and another
step can read. Parentheses distinguish the constructor from an identifier named
`node`.

```a11flow
best = node()

for url in urls {
  page = try run web-fetch(url: url)
  page.text | truncate 120 -> best
}

best | first 1 -> text
```

The node uses the active node map: the enclosing `nodes` block's map or the
action's map. `x = node(existing-id)` attaches to an existing node, and `x.id`
passes its identifier to an action that writes to it:

```a11flow
seen = node()
reader = run take-notes(pages: page.text) with "x-a11-progress-node": seen.id
seen -> progress
drain seen after reader
```

The final `after` delays `drain` until `reader` finishes writing through
`seen.id`. Without the dependency, the node would close immediately. The
compiler reports `flow.barrier.wait-lends-node` for `wait seen` in this pattern.

## Handle expected failures

`try` converts an expected step failure into a status the flow can handle. It
applies to either call verb:

```a11flow
page = try run web-fetch(url: url)
outcome = wait page

if outcome.ok {
  page.text | truncate 120 -> text
} else {
  fail unavailable outcome.message
}
```

`wait` holds until a call or flow-written node finishes. A bound `wait` also
returns the outcome. `status x` returns the same value in an expression, and
`drain node` reads the outcome while ending the named node.

A status is data:

```json
{"ok": false, "code": "NOT_FOUND", "number": 5, "message": "no such page"}
```

A flow can branch on a status, write it to an output, or raise it again. `fail`
accepts an Abseil canonical code in lower or upper case (`not_found`,
`NOT_FOUND`), a number computed at runtime, or a complete status record. `fail
outcome` preserves the status; `fail invalid_argument outcome.message` changes
the code while retaining its message.

Waiting on a failed step ends the flow with that status unless the step used
`try`.

`wait first of a, b` holds until the first call finishes and leaves the others
running. `wait all of a, b` holds until every call finishes. These forms accept
calls; node completion uses `wait` or `drain` on the node.

A race also produces the zero-based index of the winning call:

```a11flow
won = wait first of primary, backup
wait first of primary, backup -> chosen
let n = wait first of primary, backup
```

`wait all of` has no single winner, so it is a barrier only.

### A failure one value at a time

A `try` on a stage drops values that the stage cannot process while allowing the
stream to continue.

```a11flow
docs | try map it as Order -> good
```

The value is dropped and the failure logged once at warning. Where the failures
matter, `into` sends them somewhere:

```a11flow
docs | try map it as Order into rejected -> good
```

Failures sent through `into` use the same status-record shape as `status x`.
They can be counted, written to a port, or read by the caller. Without `try`, a
stage failure ends the flow.

## Loops, branches, and state

```a11flow
repeat state = {"round": 0} max 6 {
  step = run triage-step(state: state)
  state <- step.next
  until step.next.confidence >= 0.8

  if step.next.done {
    step.next.verdict -> verdict
  }
}
```

`repeat` carries one value from each pass to the next: `state` starts at the
literal and becomes whatever `<-` names. `until` (or `while`) ends the loop, and
`max` bounds it regardless. One of the two is required because `repeat` has no
default bound.

`match` pulls named fields out of text, as a stage over a stream and as a
function over one value: `lines | match "name={name} age={age:int}"` turns
`name=Alice   age=27` into a record with `name` and `age`. Literal text matches
itself, a run of spaces or tabs matches any run, and a hole may say what to read
itself as (`int`, `number`, `bool`, `word`, `line`, `rest`, `duration`, `time`,
`json`). The pattern searches anywhere in the input, so it requires no
wildcards. A hole stays on its line unless specified otherwise. The stage drops a
value the pattern does not fit and the function answers null. Where the pattern
is written out, the fields are known: `it.name` is completed and a typo is
reported.

`try` also goes in front of a **pipe**. It converts a source or destination
failure into a value instead of ending the flow:

```a11flow
moved = try findings -> seen
status moved | map it.message -> why
```

Bind a tried pipe to retain its status. An unbound failed pipe closes its
destination early without exposing the failure to a reader, so the compiler
reports it. A tried stage can fail once per value and uses `into` for dropped
values. A tried pipe fails once and stops.

A `[s =] [try] { ... }` block runs its statements as one step. Statements in a
flow body run concurrently, while a block groups their outcome. Reading a value
blocks only the statements inside the braces. A bound block yields a status like
a call. With `try`, the flow handles a block failure; otherwise the failure ends
the flow.

`for v in stream` runs its block once per value; `parallel n` runs up to `n`
passes concurrently. A stream read inside a loop or branch is materialised once
and replayed to every pass. Its buffer grows during reads, so each pass waits
only for the requested value. It does not wait for the source stream to finish.

A named loop returns its outcome in the same shape as `s = try { .. }`:

```a11flow
taken = node()
done = for line in input.lines { line -> taken }
drain taken after done
```

The loop counts as one writer of an outer node for its lifetime. Its final
release closes `taken`; the explicit `drain` records that dependency in the
source. `for` and `repeat` also accept `after`.

A `for` can use `until` or `while`. The condition runs after each pass, so the
body runs at least once and includes the value that ends the loop.

```a11flow
for line in input.lines {
  line -> seen
  until line == "quit"
}
```

The loop stops reading without cancelling the producer, matching `| first n`.
This condition cannot be combined with `parallel`, because several passes in
flight would make the stopping point depend on scheduling. `<-` applies only to
`repeat`; a `for` obtains each value from its source stream.

`advance` reads successive values without creating a loop. Compilation assigns
its offset, so three uses read the first, second, and third values. A name bound
outside a loop cannot be advanced inside it.

### Stream completion and failure

`drain node` writes both of the two facts that end a stream: the node is marked
**final**, so an ordered reader stops, and its writer is **closed**, so the store
admits nothing more. Then it reads what is left, and its name binds the outcome.

`abort node` ends a node with a failure:

```a11flow
if not status page.ok { abort findings unavailable "the source went away" }
```

Readers receive the failure status. `abort` accepts the same code and message as
`fail` and does not wait. Place it in an `if`, a loop body, or a statement with
`after` when ordering matters.

Only a node this flow **writes** can be aborted by it.

Flow unifies node completion into full endings (`drain` or `abort`): marking a
node final also closes its writer to keep reader semantics consistent.

### Ending a step early

`cancel x` aborts a step, ending the run with status `cancelled`.

To request graceful completion, send a stop command following standard action
conventions:
```a11flow
if tick.number == 3 { {"command": "stop"} -> clock.control_events }
```

Standard library actions treat `{"command": "stop"}` on their control port as
an end of input, closing their ports normally so downstream readers observe a
clean stream termination.

Stage limits (`| first n` and `for` with `until`) stop reading while leaving
the upstream producer undisturbed. An active step terminates via its control
port, `cancel`, or an assigned deadline. `cancel` evaluates immediately, so it
belongs within a conditional, loop body, or `after` clause.


## Flow boundaries and sandbox limits

Beyond `+` and `-`, Flow provides no arithmetic, function definitions, or
direct calls to host code. Expressions read and compare values, access fields
with `.field` and `[i]`, and construct records with built-in functions such as
`len`, `lower`, `join`, `merge`, and `default`. A flow operates only through
declared action streams.

## Language tables

::: a11.flow
    options:
      # Everything the package re-exports has its own section below; what
      # belongs to the package itself is the language's own tables.
      members:
        - REFERENCE
        - EXTENSION
        - BUILTINS
        - STAGES
        - FAIL_CODES

## Compiling

[`loads`][a11.flow.loads] compiles source that arrived as a string,
[`load`][a11.flow.load] a `.flow` file, and [`register`][a11.flow.register] does
both and publishes the result as actions in one call. A problem raises
[`FlowSyntaxError`][a11.flow.diagnostics.FlowSyntaxError], which carries the line and
column and converts to an A11 status.

::: a11.flow.loads

::: a11.flow.load

::: a11.flow.register

::: a11.flow.diagnostics.FlowSyntaxError

## Programs and flows

::: a11.flow.plan.Program

::: a11.flow.plan.FlowPlan

## The compiled graph

A compiled flow is data: [`describe`][a11.flow.plan.FlowPlan.describe] renders
the whole composition, which is what makes one reviewable before it is run.

::: a11.flow.plan
    options:
      # The two classes have their own section above; what is left of the module
      # is the type table and the compiler entry point.
      members:
        - TYPE_NAMES
        - compile_source

## Runtime API

::: a11.flow.runtime

## Running a program

A file with a `flow { ... }` is a program, and running one is a different call
from running a flow: it gets `argv`, a policy, this process's standard streams,
and returns its exit code as a result.

```sh
a11 flow run greet.flow -- Helena
a11 flow run --root /var/log --timeout 30s watch.flow -- /var/log/system.log
```

`a11 flow run` and the standalone `a11-flow-run` use the same interpreter. Their
hosts expose different actions: the standalone binary provides the Flow standard
library, while the Python process can also provide registered Python actions.

```sh
a11 flow run ask.flow --allow-llm --allow-net \
    --allow-env ANTHROPIC_API_KEY -- "why is the sky blue"
```

`interact_with_llm` requires a provider SDK and credential from Python, so
`examples/006-flow-programs/ask.flow` runs under the Python host. `--allow-llm`
is separate from `--allow-net`: **flow policy does not bound host-registered
actions**. Policy governs the standard library; registered Python handlers
retain their own capabilities. The host exposes no optional action by default.

From Python directly:

::: a11.flow.run_program

::: a11.flow.check_program

!!! important "Run off-loop for async action handlers"

    `run_program` runs the program to completion, so it blocks the thread it is
    called on. An `async def` handler needs a loop to drive it, and if that loop
    is on *this* thread it cannot run while the call is blocking it -- so the
    program waits forever on its own handler. `await asyncio.to_thread(...)` is
    the pattern, and it is what `a11 flow run` does.

## Diagnostics

The CLI, editors, and CI integrations use one
[`Diagnostic`][a11.flow.diagnostics.Diagnostic] shape. See
[Checking flows from a toolchain](../guides/flow-tooling.md) for the envelopes it
travels in.

::: a11.flow.diagnostics
    options:
      members:
        - DIAGNOSTICS_FORMAT
        - CODES_FORMAT
        - TOKENS_FORMAT
        - PLAN_FORMAT
        - SYNTAX_FORMAT
        - Diagnostic
        - Severity
        - Family
        - Position
        - Range
        - Edit
        - Fix
        - CodeInfo
        - known_codes
        - find_code
        - sort_diagnostics
        - diagnostics_envelope
        - codes_envelope
        - sarif_log
        - LineIndex
