# Flow language

Flow is a small language for describing a composition of actions that is itself
an action. This page is the whole of it, section by section, and then the Python
API that compiles and runs one.
[Compose actions without deploying code](../guides/flow.md) builds a real
composition a statement at a time and is the better place to start;
`a11.flow.REFERENCE` is the same material sized for a prompt, for a model that
has to write one.

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

`x = run action(port: source)` runs an action here and gives it a name, so its
ports can be named afterwards; `x = call action(...)` dispatches one on the
stream the flow is attached to instead. `source -> destination` pipes a stream into a
node. Everything else is a variation on those two.

Note what is *not* being said. `brief` is dispatched before it has any pages: it
is streaming, so the loop feeds it while it works, and A11 closes its `pages`
port when the loop's last writer is done. Nothing declares an order between the
search and the fetches either — **steps run concurrently, and order comes from
the data**. When an order really is needed, `after`, `wait` and `drain` say so
explicitly.

A port says what it holds first and what it is like afterwards: it carries one
value unless it says `stream`, and is optional unless it says `required`. Every
significant word may be written in lower case or upper case — `for` or `FOR`,
`stream` or `STREAM`. Mixed case is a name, not a keyword, which keeps the rule
easy to see.


## What a port holds

```a11flow
in  question: string required
in  frames:   list[a11.NodeFragment] stream
out audio:    a11.sdk.AudioBuffer stream
out raw:      "application/x-msgpack"
```

Besides the built-in names — `string`, `text`, `number`, `integer`, `bool`,
`object`, `json`, `list`, `bytes`, `any` — a type may be **a tag a
serialisation registry knows a type by**, written unquoted exactly as it is
registered: `a11.sdk.AudioBuffer`. A dotted name is read as a tag, which is what
tells one from a misspelt built-in, and it is carried through as written --
compiling a flow does not require the module defining the type to have been
imported. A container says what it holds in brackets: `list[string]`,
`list[a11.NodeFragment]`. A quoted type is a mimetype, for a port described by
its representation rather than by a type at all.

## Descriptions, at the length they need

A description is what a caller — often a model — reads to decide whether to use a
flow, so it is worth writing properly. Two spellings keep it from being squeezed
onto the end of a declaration:

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

A `"""` string may hold line breaks, and its value is **dedented**: a blank first
line goes, a whitespace-only last line goes with the break above it, and the
indentation every remaining line shares comes off. So the text sits at the
indentation of the flow it belongs to and still reads as prose, with whatever one
line has *extra* kept. Escapes work as they do in an ordinary string.

A description may also stand **alone on the line below** what it describes, at any
indentation or none — for a port, a header, or a `describe`. That is unambiguous
because the string has to be alone on its line: `"a literal" -> out` is a
statement, since something follows the string.

`a11 flow fmt` indents a description under its declaration and lines up the
columns of a run of declarations around it.

## Making a value of a type

A port often wants a real type — an `Interaction`, an `AudioBuffer` — and what a
flow has is the handful of fields it cared about. `TYPE{...}` bridges the two:

```
a11.sdk.Interaction{
  role: "user",
  content: [to_chunk({"role": "user", "content": [{"type": "text", "text": said}]})]
}
```

`EXPR as TYPE` is the same thing written the other way round, and the one to use
for a generic (`pieces as list[string]`) or when the value came from somewhere
else. Either way the value is *validated* into the type, so defaults are filled
in and a field that will not fit is an error rather than a surprise later.
`to_chunk` and `from_chunk` are the two builtins that make and read a
[`Chunk`][a11.data.types.Chunk], which is what a content-bearing type is made
of.

Which types exist is the host's decision, not the flow's: a tag resolves against
the serialisation registries of the process the flow runs in, and a flow cannot
import anything. `TYPE{...}` is unavailable where a `{` would open a block
instead — an `if` condition, a `for`'s source — so `if step.next.done {` keeps
reading the way it always has; wrap it in brackets if you really need one there.

## The parts that exist because A11 does

A few things A11 can do are awkward from glue code, and each is a piece of
syntax here.

### Running a step, and calling one

A11 has two verbs for getting an action done, and so does a flow.
`run some-action(...)` executes the handler registered where the flow is
running. `call some-action(...)` puts the action on the stream the flow is
attached to and lets the peer do it. They are not interchangeable, and the flow
says which it means rather than leaving it to whatever happens to be in a
registry:

```a11flow
search = run web-search(query: question)      # ours, here
llm    = call interact_with_llm(...)          # theirs, over there
```

`run` needs a handler and says so if there is none, instead of quietly going to
the session. `call` needs none — an action registered for its *schema* alone is
exactly how a composition written against somebody else's deployment learns the
port names while saying the work is not local — and it goes to the peer even
when a handler for that name does happen to be registered here.

Which one an action takes is a property of the deployment, not of the action, so
[`flow_actions`][a11.sdk.flow_tools] reports it: each entry carries `runnable`,
and a model writing a flow reads the verb off that rather than guessing.

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

A program is a set of flows, not a sequence, so which one is written first is
just reading order — and their ports are checked against each other while the
file is compiled, exactly as they are against a registered action. That is what
lets a composition be factored: the reusable piece becomes a flow, the caller
stays readable, and the whole thing is still one text with one entry point,
which is what `flow_run` and a gateway are handed.

### Reading a stream you do not want

An output port nobody drains stalls the action producing it. `skip page.bytes`
reads one and keeps nothing. The runtime also drains any declared output the flow
never mentions, so forgetting is not a way to deadlock a composition.

`skip n port` is a different statement wearing the same word. A Flow stream fans
out — every reader sees all of it — so `| drop 1` trims only the one reader that
says it. A count on `skip` takes the values off the node itself, before the
fan-out, so *every* reader starts after them:

```a11flow
rows = run read-csv(path: path)
skip 1 rows.lines            # the header line is nobody's
rows.lines | count -> data-rows
rows.lines -> passed-through # both readers start at the second line
```

Several of them naming one node add up — `skip 1 x` and `skip 2 x` leave three
values unread, in either order, because the count belongs to the node and is
summed while the flow is compiled. It takes a port or a node, not a pipeline:
there is no front to take values off a thing each reader derives for itself.

### Putting a stream back together

`| group EXPR` is `batch` with a question instead of a count: values gather into
a list, and the list closes when the expression holds of the value just added.
It is how a stream of fragments becomes whole things —

```a11flow
pieces | group ends-with(trim(it), [".", "?", "!"]) | map trim(join(it, " "))
```

— which turns partial utterances into sentences. Whatever is still gathered when
the stream ends comes out too, because a partial group is still what was said.

`| then SOURCE` is the other direction: this stream, and then that one.

```a11flow
history then asked -> llm.interactions
```

`then` and `where` may drop the pipe, because both read as words joining the
things they sit between rather than as transformations applied to a stream:
`history then asked`, `hits where it.ok`. Every other stage keeps its `|`,
which is what stops a stage name from swallowing a port that shares it — a
port really called `then` still reads as one.

`| flatten` is `batch` backwards: a stream of lists becomes a stream of what
they held.

```a11flow
pages | map it.lines | flatten -> lines
```

A value that is not a list goes through as itself, so a mixed stream is
flattened rather than refused.

`| window N` is `batch` with the lists overlapping: one list of the last `N`
values per value, once `N` have arrived.

```a11flow
lines | window 2 | where contains(join(it, "\n"), needle) -> hits
```

It exists because `batch` has to put a boundary *somewhere*, and a question
about neighbours is exactly the question a boundary hides: a pattern spanning
two lines is invisible to a `batch` whenever the boundary falls between them, so
roughly one match in `N` goes missing and nothing says so. A window holds `N`
values and no more, so one over a stream that never ends costs nothing that
grows — and a stream shorter than `N` yields nothing at all, whereas `batch`
may emit a shorter final list.

`interleave(a, b, ...)` is the other kind of fan-in. Where `zip` reads its
sources *in step* and gives a tuple per round, this reads them at once and gives
each value as it arrives, so a fast stream is not held behind a slow one:

```a11flow
interleave(llm.text_output, tool.progress) -> shown
```

The order between the sources is whatever the values did — that is the point of
asking — and it ends when every source has. A source that ends badly ends the
stream with its status.

### Reducing a stream to one value

`| collect` and `| count` were the whole of it; the arithmetic ones are here for
the same reason:

```a11flow
orders | sum it.price -> revenue
runs   | avg it.elapsed -> typical
hits   | max it.score -> best
```

`sum`, `min`, `max` and `avg` read the whole stream and yield one value. With no
expression they use the values themselves; with one they read a field of each, so
`| sum it.price` is `| map it.price | sum` said once. Durations add and average
as durations. `min`, `max` and `avg` of an *empty* stream yield **nothing** —
the smallest of no values is not a value — while `| sum` of one is `0`, because
adding nothing is.

`| fold` is the general form, for the shape none of those is:

```a11flow
orders | fold 0 as total, total + it.price -> revenue
```

The name is bound to what the last value produced and `it` to the value in hand.
The starting value is a literal, not an expression: `fold 0 as total` read as an
expression would be a cast of `0` to a type called `total`, and the language
should not have to guess which was meant. A **record** literal is allowed, and a
state worth carrying usually is one; the ambiguity does not arise there, because
`{ .. }` is read by its braces before `as` is looked at.

### Carrying state along a stream

`| scan` is written exactly as `fold` is, and the difference is where the values
go: `fold` yields one when the stream ends, `scan` yields one per value as it
arrives.

```a11flow
lines | scan 0 as n, n + 1 -> numbered
```

That is what a state machine is — a state carried forward and read at every step
— and it is the only way to write one over a stream. The two constructs that
look like they should do it cannot: `repeat` carries state across passes but
reads its stream from the start on *every* pass, and `for` walks a stream one
value at a time but carries nothing between passes. `scan` is the one that does
both, and it holds one value of state rather than the stream.

With a record start it is a state machine in the ordinary sense:

```a11flow
lines
  | scan {"inside": false, "line": ""} as s,
      {"inside": starts-with(it, "BEGIN") or (s.inside and not starts-with(it, "END")),
       "line": it}
  | where it.inside and not starts-with(it.line, "BEGIN")
  | map it.line
  -> body
```

The cost is one value of state and nothing per value of the stream, so this runs
in constant memory over an input of any size — which is the property that makes
it worth having as a stage rather than an action.

`| sort` puts a stream in order:

```a11flow
hits | sort by it.score desc | first 10 -> best
```

It reads the whole stream to find out what the order is, so nothing comes out
until the stream ends. Values compare the way `<` compares them, `by` names what
to compare, `desc` reverses it, and it is **stable**: values that tie stay in the
order they were written.

### When a value arrives

Two stages are about time rather than about values.

```a11flow
tokens   | timeout 30s  -> answer
requests | pace 100ms   -> to_api
```

`timeout` is a **gap**: a stream that keeps arriving runs as long as it likes,
and one that goes quiet for longer than this ends the flow with
`deadline_exceeded`. That is what a stalled producer looks like from here; a
budget for a whole step is `wait ... timeout`, which already existed.

`pace` spaces values out and **drops nothing** — whoever is producing them is
held back behind the buffer, which is what makes it a rate limit rather than a
sample. What it costs is latency, on purpose.

### Working on several values at once

A per-value stage may say how many values it may have in hand:

```a11flow
urls | map fetch_page(it) parallel 8 -> bodies
```

**What follows still reads them in the order they arrived.** The stage finishes
its values in whatever order it finishes them and puts the stream back together
before anything downstream sees it, so `parallel` can be added to a pipeline
nobody else changed. `unordered` gives that up for whatever it saves:

```a11flow
urls | map fetch_page(it) parallel 8 unordered -> bodies
```

It is worth writing where the per-value work is expensive — a round trip through
the host, a coercion, a large `chunk` — and nowhere else: eight workers taking a
field out of a record is eight fibres doing what one was already fast at. A
stage that gathers or orders values refuses `parallel`, because there is nothing
to run at once.

### Text, times, and how long something took

`strformat("%s of %s", got, wanted)` is printf, because a format string is
something people already know how to read: `%s` for text, `%d`, `%f` and `%x`
for numbers, printf's own flags and precision (`%-8s`, `%06.2f`), `%2$s` to pick
a value by number, and `%%` for a literal percent. `| strformat "fmt"` is the
one-value shorthand for `| map strformat("fmt", it)`, which is nearly every use
of it.

Flow uses printf-style conversions rather than Python template strings:
`str.format` reads attributes, which could escape sandboxing when formatting
untrusted expressions. A printf conversion operates strictly on supplied values
without attribute access. A conversion with no value behind it is left as written
rather than raising, because a visible `%3$s` in a log line is easier to
diagnose than a flow that died formatting one.

Durations are written the way a timeout is — `500ns`, `250ms`, `30s`, `2m`,
`1h`, and compounded as `1m30s500ms` — and are ordinary values. `now()` is the
clock, and the arithmetic is the arithmetic A11's own types allow:

```a11flow
started = node()
now() -> started
took = now() - started            # instant - instant is a duration
if took > 30s { fail deadline_exceeded strformat("gave up after %s", took) }
```

`+` and `-` are the only arithmetic the language has, and they exist for this:
a composition cannot otherwise say how long it took. A bare number beside a
duration counts as seconds; `seconds(d)` gives the number back. Subtracting the
other way round gives a length below zero, and it says so rather than meaning
"forever" the way a negative timeout does elsewhere in A11. `-` needs its
spaces, since `text-upper` is one name.

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

Two statements writing to the same node interleave by arrival, which is fine for
pages and wrong for a conversation. `then` is how a flow says which comes first,
and it is what makes a multi-turn chat expressible: the turns so far, then the
one just made.

### Throwing values away before they cost anything

`| truncate 200` cuts each page down before it is written to the summariser's
port. What is dropped is never serialised, never sent to a peer, and — when the
next step is a model — never charged for. `| first 3`, `| where it.ok`,
`| mime "text/*"` and `| drop 1` are the same lever at different granularities,
and they are the reason a model asked to *instrument* a composition can make it
cheaper without changing what it computes.

### Saying how a value travels

`| packb` writes a value as `application/x-msgpack` instead of JSON. It is a
no-op when the producer already wrote MessagePack — the chunk is passed on
untouched, type tag and all — so putting it in front of a port that wants
packed bytes is safe whatever is upstream, and costs a re-encode only when there
is really one to pay for.

### Passing on what the flow was told

Headers are how a call is told *about* itself — which model to answer with, who
is asking, when to give up. A11 already gives a nested action every `x-a11-`
header of its parent, so a deadline or a model reaches a step with the flow
saying nothing at all. For the headers outside that prefix, `forward headers`
says it in one line:

```a11flow
answer = run interact_with_llm(interactions: asked, config: {})
    forward headers "authorization", "x-tenant-*"
```

A name is forwarded as it arrived; a `*` matches a family of them; a header the
caller did not send is simply not forwarded, so an optional one cannot fail the
composition. `with "header": expr` remains the other half — for a value the flow
*computes* rather than passes on — and if both name the same header the `with`
wins, being the more specific of the two. Before this, moving one header one hop
took a `header` declaration to give it a name and a `with` on every step that
needed it.

### Keeping a step's traffic off the wire

`nodes fetched { ... }` gives the calls inside it a
[`NodeMap`][a11.nodes.async_node.NodeMap] of their own. Their ports are not in
the session's node map, so the peer that dispatched the flow neither sees them
nor receives their fragments: four fetched pages stay here, one answer goes back.
A `run` step already keeps its nodes off the wire unless it asks for `tee`; a
`nodes` block is the stronger statement, and it covers `call` steps too.

### Nodes of the flow's own

`x = node()` gives a flow a stream of its own: somewhere several passes of a loop
can write and one reader can read back, which a unary output port cannot be. The
parentheses are not decoration — making a node is the one thing in the language
that *does* something without naming an action, so it is written as the
construction it is, and `node` stays available as a name for anything else.

```a11flow
best = node()

for url in urls {
  page = try run web-fetch(url: url)
  page.text | truncate 120 -> best
}

best | first 1 -> text
```

The node lands in the contextually active node map — the enclosing `nodes`
block's, or the action's — so `nodes scratch` around it keeps it off the wire
like anything else. `x = node(where-they-said)` attaches to a node *somebody else*
named instead of making one, and `x.id` hands a node to an action that expects to
be told where to write:

```a11flow
seen = node()
reader = run take-notes(pages: page.text) with "x-a11-progress-node": seen.id
seen -> progress
drain seen after reader          # the flow lent the node; the flow ends it
```

## Failures a flow expects

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

A race is also a *value*: which one won, counted from zero. It is written where
a number is written, so the flow can act on the answer rather than only on the
fact that someone finished:

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
`max` bounds it regardless. One of the two is required: there is no default
bound, so a `repeat` with neither is refused rather than stopping after some
number of passes and reporting that as success.

`match` pulls named fields out of text, as a stage over a stream and as a
function over one value: `lines | match "name={name} age={age:int}"` turns
`name=Alice   age=27` into a record with `name` and `age`. Literal text matches
itself, a run of spaces or tabs matches any run, and a hole may say what to read
itself as (`int`, `number`, `bool`, `word`, `line`, `rest`, `duration`, `time`,
`json`). The pattern searches rather than anchors, so there are no wildcards to
write, and a hole stays on its line unless it says otherwise. The stage drops a
value the pattern does not fit and the function answers null. Where the pattern
is written out, the fields are known: `it.name` is completed and a typo is
reported.

`try` also goes in front of a **pipe**, and there it means the failure arriving
from the source — or refused by the destination — is a value rather than the end
of the flow:

```a11flow
moved = try findings -> seen
status moved | map it.message -> why
```

Bind it and read it. Unbound, a tolerated pipe that failed leaves its destination
closed early and every reader of it sees an ordinary end of stream, with nothing
saying why — so the language reports that. This is a different thing from `try`
on a *stage*: a stage fails once per value and carries on, which is why it has
`into` for the ones it dropped, while a pipe fails once and stops.

A `[s =] [try] { ... }` block runs its statements as one step. Everything in a
flow's body runs at once, which is the point of it; a block is how a flow says
"these together, and *this* is what came of them". Reading a value blocks where
it stands, so a condition inside a block holds up only what is in the braces and
not the rest of the body. Bound to a name it reads as a status, exactly as a call
does; `try` says a failure inside is the flow's to handle, and without it a
failure ends the flow the way a call's does.

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
is worked out while the file is compiled, so it reads the first, second and third
value where it is written out three times, and advancing a name bound outside a
loop is refused rather than binding the same value on every pass.

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

Flow unifies node completion into full endings (`drain` or `abort`): marking a node
final also closes its writer to ensure consistent reader semantics.

### Ending a step early

`cancel x` aborts a step, ending the run with status `cancelled`.

To request that a step finish gracefully rather than cancelling, send a stop command
following standard action conventions:
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

Beyond `+` and `-` there is no arithmetic, no way to define a function, and no way to call out to
arbitrary host code. An expression reads values, compares them, accesses fields via `.field`
and `[i]`, and constructs new records using built-in functions (`len`, `lower`,
`join`, `merge`, `default`, etc.). A flow operates strictly by orchestrating declared
action streams within these sandbox boundaries.

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
and its exit code is a result rather than an exception.

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
