# Flow language

Flow describes a composition of actions as an action. This page is the language
reference and Python API. Start with
[Compose actions without deploying code](../guides/flow.md) for a guided
example. `a11.flow.REFERENCE` provides a compact version suitable for model
prompts.

## One flow, read from the top

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


## What a port holds

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

## Making a value of a type

A flow can construct a registered type such as `Interaction` or `AudioBuffer`
from fields with `TYPE{...}`:

```
a11.sdk.Interaction{
  role: "user",
  content: [to_chunk({"role": "user", "content": [{"type": "text", "text": said}]})]
}
```

`EXPR as TYPE` performs the same conversion and supports generic types such as
`pieces as list[string]`. Both forms validate the value, apply defaults, and
report incompatible fields.
`to_chunk` and `from_chunk` are the two builtins that make and read a
[`Chunk`][a11.data.types.Chunk], which is what a content-bearing type is made
of.

Which types exist is the host's decision, not the flow's: a tag resolves against
the serialisation registries of the process the flow runs in, and a flow cannot
import anything. `TYPE{...}` is unavailable where a `{` would open a block
instead — an `if` condition, a `for`'s source — so `if step.next.done {` keeps
reading the way it always has; wrap it in brackets if you really need one there.

## Action composition

### Running a step, and calling one

`run some-action(...)` executes a handler registered in the local process.
`call some-action(...)` dispatches the action on the flow's attached stream.
Choose the verb explicitly:

```a11flow
search = run web-search(query: question)      # ours, here
llm    = call interact_with_llm(...)          # theirs, over there
```

`run` requires a local handler. `call` requires only a local schema for
resolution and always dispatches to the peer, even when a local handler has the
same name.

The deployment determines which verb is available.
[`flow_actions`][a11.sdk.flow_tools] reports this through each entry's
`runnable` field.

`try` goes in front of either: `try run`, `try call`.

Either verb may also name **another flow of the same file**, with nothing
registered for it:

```a11flow
flow ask-twice {
  in  question: string
  out answers:  string stream
  first  = run ask(question: question)   # `ask` is declared below
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

`skip n port` is a different statement wearing the same word. A Flow stream fans
out — every reader sees all of it — so `| drop 1` trims only the one reader that
says it. A count on `skip` takes the values off the node itself, before the
fan-out, so *every* reader starts after them:

```a11flow
rows = run read-csv(path: path)
skip 1 rows.lines            # discard the header line
rows.lines | count -> data-rows
rows.lines -> passed-through # both readers start at the second line
```

Several of them naming one node add up — `skip 1 x` and `skip 2 x` leave three
values unread, in either order, because the count belongs to the node and is
summed while the flow is compiled. It takes a port or a node, not a pipeline:
there is no front to take values off a thing each reader derives for itself.

`-> _` is the third of these, and the one that does the work. `skip` says the
values were never wanted, and a counted one is taken off the stream before
anybody sees it; `_` says the *result* is not wanted, and the pipeline that
produced it still runs:

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

`| then SOURCE` is the other direction: this stream, and then that one.

```a11flow
history then asked -> llm.interactions
```

`then` and `where` may omit the pipe: `history then asked` and
`hits where it.ok`. Every other stage requires `|`, which distinguishes stage
names from identically named ports.

`| flatten` is `batch` backwards: a stream of lists becomes a stream of what
they held.

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

`interleave(a, b, ...)` is the other kind of fan-in. Where `zip` reads its
sources *in step* and gives a tuple per round, this reads them at once and gives
each value as it arrives, so a fast stream is not held behind a slow one:

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

`| fold` is the general form, for the shape none of those is:

```a11flow
orders | fold 0 as total, total + it.price -> revenue
```

The name is bound to the previous accumulated value and `it` to the current
input. The starting value is a literal, not an expression: otherwise
`fold 0 as total` could be parsed as a cast of `0` to a type called `total`.
A **record** literal is allowed because its braces remove this ambiguity.

### Carrying state along a stream

`| scan` is written exactly as `fold` is, and the difference is where the values
go: `fold` yields one when the stream ends, `scan` yields one per value as it
arrives.

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

### When a value arrives

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

A per-value stage may say how many values it may have in hand:

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

### Text, times, and how long something took

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
let took = now() - started        # instant - instant is a duration
strformat("took %s", took) -> log after done
```

Steps run concurrently, so source order alone does not delay a clock read.
`now() -> started` needs no barrier because it records the start. A clock read
that measures produced work requires `after`; otherwise the compiler reports
`flow.barrier.unordered-clock`.

An `after` applies to the complete statement, including arguments. In
`run act(p: now() - started) after done`, the argument is evaluated after
`done`.

`+` and `-` are the only arithmetic the language has, and they exist for this:
a composition cannot otherwise say how long it took. A bare number beside a
duration counts as seconds; `seconds(d)` gives the number back. Subtracting in
the other order produces a negative duration; it does not use the
infinite-timeout convention found elsewhere in A11. `-` requires spaces because
`text-upper` is an identifier.

Formatting: `%s` renders a duration as `1m30s` and an instant as RFC 3339. A
unit in the parenthesised spec gives one number — `%(ns)d`, `%(us)d`, `%(ms)d`,
`%(s)d`, `%(m)d`, `%(h)d` — and `%(%H:%M:%S)s` or `%(epoch)d` formats an
instant.

`duration(x)` and `time(x)` are the way back in, and they read exactly what the
formatting writes:

```
deadline = time(header-deadline)          # "2026-08-11T09:14:22Z"
budget   = duration(header-budget)        # "1m30s", or a number of seconds
if now() + budget > deadline { fail deadline_exceeded "not enough time left" }
```

A timestamp or a timeout that arrived as text — from a header, a JSON field, a
model's answer — is a value again, in one call and without a format string to
get wrong.

Two statements writing to the same node interleave by arrival. Use `then` when
order matters, such as sending prior conversation turns before the current one.

### Reducing data before serialization

`| truncate 200` shortens each page before writing it to the summariser. Dropped
data is not serialized, sent to a peer, or included in model input. The same
applies to `| first 3`, `| where it.ok`, `| mime "text/*"`, and `| drop 1`.

### Saying how a value travels

`| packb` writes a value as `application/x-msgpack` instead of JSON. Existing
MessagePack chunks pass through unchanged, including their type tag. Other
representations are re-encoded.

### Passing on what the flow was told

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

### Keeping a step's traffic off the wire

`nodes fetched { ... }` gives the calls inside it a
[`NodeMap`][a11.nodes.async_node.NodeMap] of their own. Their ports are not in
the session's node map, so the peer that dispatched the flow neither sees them
nor receives their fragments: four fetched pages stay here, one answer goes back.
A `run` step already keeps its nodes off the wire unless it asks for `tee`; a
`nodes` block is the stronger statement, and it covers `call` steps too.

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
drain seen after reader          # the flow lent the node; the flow ends it
```

The final `after` delays `drain` until `reader` finishes writing through
`seen.id`. Without the dependency, the node would close immediately. The
compiler reports `flow.barrier.wait-lends-node` for `wait seen` in this pattern.

## Handle expected failures

A composition that calls four actions will sometimes have one of them fail, and
often that is not a reason to abandon the other three. `try` says so — on
either verb — and from there the flow is in charge:

```a11flow
page = try run web-fetch(url: url)
outcome = wait page

if outcome.ok {
  page.text | truncate 120 -> text
} else {
  fail unavailable outcome.message
}
```

`wait` holds until its subject is finished — a call, or a node this flow writes
-- and bound to a name it is also how the flow *reads* that outcome, because
waiting and finding out are the same moment. `status x` is the same value where an
expression is expected, and `drain node` is the spelling to use beside the port it
is about.

A status is data:

```json
{"ok": false, "code": "NOT_FOUND", "number": 5, "message": "no such page"}
```

so a flow can branch on it, put it on one of its own outputs, or raise it again.
`fail` takes any of Abseil's canonical codes by name in either
case (`not_found`, `NOT_FOUND`), a number computed at runtime, or a whole status
record — `fail outcome` re-raises exactly what happened, and
`fail invalid_argument outcome.message` says it again in the caller's terms.

Waiting on something that finished badly ends the flow with *that* status, unless
it was a `try`: those are the failures the flow said it would handle.

`wait first of a, b` holds until the first of several calls finishes and leaves
the rest running; `wait all of a, b` holds for every one of them. A race is
between *calls* — a node is finished when whoever writes it says so, which is
what `wait` and `drain` are for.

A race also produces the zero-based index of the winning call:

```a11flow
won = wait first of primary, backup        # 0 or 1
wait first of primary, backup -> chosen    # ...or straight to a port
let n = wait first of primary, backup      # ...or named
```

`wait all of` has no single winner, so it is a barrier only.

### A failure one value at a time

A `try` on a *stage* is the same idea inside a pipeline: one value the stage
cannot do is not a reason to abandon the stream.

```a11flow
docs | try map it as Order -> good
```

The value is dropped and the failure logged once at warning. Where the failures
matter, `into` sends them somewhere:

```a11flow
docs | try map it as Order into rejected -> good
```

They arrive as status records — the same shape `status x` yields — so a stream of
failures is an ordinary stream: countable, writable to a port, readable by the
caller. Without `try`, a value a stage cannot do ends the flow, which is the
right default for a composition that is not expecting one.

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

Bind it and read it. Unbound, a tolerated pipe that failed leaves its destination
closed early and every reader of it sees an ordinary end of stream, with nothing
saying why — so the language reports that. This is a different thing from `try`
on a *stage*: a stage fails once per value and carries on, which is why it has
`into` for the ones it dropped, while a pipe fails once and stops.

A `[s =] [try] { ... }` block runs its statements as one step. Statements in a
flow body run concurrently, while a block groups their outcome. Reading a value
blocks only the statements inside the braces. A bound block yields a status like
a call. With `try`, the flow handles a block failure; otherwise the failure ends
the flow.

`for v in stream` runs its block once per value, `parallel n` runs `n` passes at
a time. A stream read *inside* a loop or branch is materialised: the runtime
buffers it once and replays it to every pass, which is what lets each pass see
the same outer value. The buffer grows while it is read, so a pass waits for the
value it asks for and not for the stream to finish — a loop reading a stream
that is still open is not held up by it, and neither is anything written after
the loop.

A loop may be **named**, and then it reads as its own outcome — the same shape
`s = try { .. }` has:

```a11flow
taken = node()
done = for line in input.lines { line -> taken }
drain taken after done
```

That last line is what a flow could not say before. The node was already ended
when the loop finished — a loop counts as one writer of an outer node for as
long as it runs, so the last `Release` closes it — but nothing in the *text*
said so, and a program whose finished state has to be inferred from writer
counting is a program that reads as unfinished. `for` and `repeat` also take an
`after`, because a loop is a step like any other.

A `for` takes `until`/`while` too, and it means what it means in a `repeat`:
asked at the tail of a pass, so the body always runs at least once and the value
that ended the loop was seen.

```a11flow
for line in input.lines {
  line -> seen
  until line == "quit"
}
```

That is how a loop over a stream stops before the stream does. It stops
*reading*, exactly as `| first n` does, and like `first n` it does not cancel
whatever was producing — see below. It cannot be written with `parallel`: the
question is about the pass that just finished, and with several in flight there
is no such pass, so which values were seen would depend on scheduling. `<-`
stays a `repeat`'s, because a `for` takes its value from its stream and has
nothing to hand the next pass.

`advance` is the other way to walk a stream, and it is *not* a loop: its offset
is determined during compilation, so three uses read the first, second, and
third values. A name bound outside a loop cannot be advanced inside the loop.

### Ending a stream, and ending it badly

`drain node` writes both of the two facts that end a stream: the node is marked
**final**, so an ordered reader stops, and its writer is **closed**, so the store
admits nothing more. Then it reads what is left, and its name binds the outcome.

`abort node` is the other ending:

```a11flow
if not status page.ok { abort findings unavailable "the source went away" }
```

The difference is what a *reader* is told. Both end the stream; only this one
says it went wrong, and without it a stream cut short by something the flow
noticed is indistinguishable from one that finished. It takes the code and
message a `fail` takes, and waits for nothing for the same reason, so it belongs
in an `if` or a loop body or carries an `after`.

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

## The tables, as data

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

## Running one

::: a11.flow.runtime

## Running a program

A file with a `flow { ... }` is a program, and running one is a different call
from running a flow: it gets `argv`, a policy, this process's standard streams,
and returns its exit code as a result.

```sh
a11 flow run greet.flow -- Helena
a11 flow run --root /var/log --timeout 30s watch.flow -- /var/log/system.log
```

`a11 flow run` and the standalone `a11-flow-run` are the **same interpreter**, so
a program behaves identically whichever started it. What differs is what the host
can offer it, and that is the entire reason the Python one exists: a program may
only call actions that exist where it runs, the binary has exactly the Flow
standard library, and this process has whatever Python has.

```sh
a11 flow run ask.flow --allow-llm --allow-net \
    --allow-env ANTHROPIC_API_KEY -- "why is the sky blue"
```

`interact_with_llm` needs a provider SDK and a credential, both of which live in
Python, so `examples/006-flow-programs/ask.flow` runs this way and no other.
`--allow-llm` is its own flag and not part of `--allow-net` because **a
host-registered action is not bounded by the flow policy**: the policy governs
what the standard library may do and can say nothing about what a Python handler
does. Offering one is therefore a separate decision, and the default is to offer
nothing.

From Python directly:

::: a11.flow.run_program

::: a11.flow.check_program

!!! important "Call it off the loop when your actions are `async`"

    `run_program` runs the program to completion, so it blocks the thread it is
    called on. An `async def` handler needs a loop to drive it, and if that loop
    is on *this* thread it cannot run while the call is blocking it -- so the
    program waits forever on its own handler. `await asyncio.to_thread(...)` is
    the pattern, and it is what `a11 flow run` does.

## Diagnostics

Everything that reports on a flow -- the CLI, an editor, a CI job -- renders the
one [`Diagnostic`][a11.flow.diagnostics.Diagnostic] shape. See
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
