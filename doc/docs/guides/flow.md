# Compose actions without deploying code

Some compositions do not deserve a repository. A gateway needs to search, read
the three best hits, and summarise them; a client needs to run four existing
actions in a particular shape, once, for one customer. The actions already exist
and are already deployed. What is missing is a description of how they connect --
and writing that in Python means a commit, a review, a release, and a restart.

[Flow](../api/flow.md) is that description, as text. A flow declares ports and
headers, calls actions, pipes their streaming ports into one another, loops and
branches, and presents an ordinary
[`ActionSchema`][a11.actions.action.ActionSchema] to whoever dispatches it. It is
an action made of actions, and it arrives as a string.

## One flow, read from the top

```
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

```
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

```
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

```
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

```
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

```
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

```
pieces | group ends-with(trim(it), [".", "?", "!"]) | map trim(join(it, " "))
```

— which turns partial utterances into sentences. Whatever is still gathered when
the stream ends comes out too, because a partial group is still what was said.

`| then SOURCE` is the other direction: this stream, and then that one.

```
history then asked -> llm.interactions
```

`then` and `where` may drop the pipe, because both read as words joining the
things they sit between rather than as transformations applied to a stream:
`history then asked`, `hits where it.ok`. Every other stage keeps its `|`,
which is what stops a stage name from swallowing a port that shares it — a
port really called `then` still reads as one.

### Text, times, and how long something took

`strformat("%s of %s", got, wanted)` is printf, because a format string is
something people already know how to read: `%s` for text, `%d`, `%f` and `%x`
for numbers, printf's own flags and precision (`%-8s`, `%06.2f`), `%2$s` to pick
a value by number, and `%%` for a literal percent. `| strformat "fmt"` is the
one-value shorthand for `| map strformat("fmt", it)`, which is nearly every use
of it.

printf rather than a Python template deliberately: `str.format` reads
attributes, so `{0.__class__.__init__.__globals__}` would be a way out of the
sandbox, and flow templates can come from a model. A printf conversion has
nowhere to walk to. A conversion with no value behind it is left as written
rather than raising, because a visible `%3$s` in a log line is easier to
diagnose than a flow that died formatting one.

Durations are written the way a timeout is — `500ns`, `250ms`, `30s`, `2m`,
`1h`, and compounded as `1m30s500ms` — and are ordinary values. `now()` is the
clock, and the arithmetic is the arithmetic A11's own types allow:

```
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

```
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

```
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

```
seen = node()
reader = run take-notes(pages: page.text) with "x-a11-progress-node": seen.id
seen -> progress
drain seen after reader          # the flow lent the node; the flow ends it
```

## Failures a flow expects

A composition that calls four actions will sometimes have one of them fail, and
often that is not a reason to abandon the other three. `try` says so — on
either verb — and from there the flow is in charge:

```
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
[`fail`](../api/flow.md) takes any of Abseil's canonical codes by name in either
case (`not_found`, `NOT_FOUND`), a number computed at runtime, or a whole status
record — `fail outcome` re-raises exactly what happened, and
`fail invalid_argument outcome.message` says it again in the caller's terms.

Waiting on something that finished badly ends the flow with *that* status, unless
it was a `try`: those are the failures the flow said it would handle.

## Loops, branches, and state

```
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

## Running one

```python
from a11 import flow

program = flow.register(source, registry, "research.flow")   # compile + publish
result = await program["research"].invoke(
    question="how do nodes and actions stream", registry=registry
)
```

[`register`][a11.flow.register] compiles the source and publishes every flow in
it as an action; after that a [`Session`][a11.service.session.Session] dispatches
them like anything else, and one flow may call another by name.
[`invoke`][a11.flow.plan.FlowPlan.invoke] is the convenience path for scripts and
tests: it feeds the inputs, collects every output port, and hands back a value
for each ordinary port and a list for each `stream` one.

A flow that will not compile raises
[`FlowSyntaxError`][a11.flow.diagnostics.FlowSyntaxError] with the line and column, and
[`to_status`][a11.flow.diagnostics.FlowSyntaxError.to_status] turns that into the
`INVALID_ARGUMENT` a caller should be told about.

## What a flow deliberately cannot do

Beyond `+` and `-` there is no arithmetic, no way to define a function, and no way to call out to
code. An expression reads values, compares them, takes them apart with `.field`
and `[i]`, and builds new ones — with a fixed set of functions (`len`, `lower`,
`join`, `merge`, `default`, and a handful more). That is the whole of it, which is
what makes accepting a flow from somewhere else and running it a reasonable thing
to do: it can only call the actions it names, and it can only move their streams
around.

The runnable version of everything above is in
[`examples/003-flow-dsl`](https://github.com/hpnkv/a11/tree/main/examples/003-flow-dsl).
Three of those files need nothing but the toy actions beside them; the other
three — `assistant.flow`, `ops.flow` and `dictate.flow` — compose what a real
`a11 gateway run` serves: its shell, its microphone, and the same
`interact_with_llm` `a11 chat` uses. `ask-the-pages` is the one worth reading
twice, because its retrieval actions run in the example's own process and its
model runs on the gateway, and the flow does not distinguish between them.

## Writing one

Editor support lives in
[`editors/`](https://github.com/hpnkv/a11/tree/main/editors): a Sublime Text
syntax definition, and — in the
[A11 plugin for JetBrains IDEs](https://github.com/hpnkv/a11/tree/main/intellij-plugin)
— highlighting for `.flow` files *and* for flows written inside a string
literal, which is where most of them live:

```python
program = flow.loads("""
    flow shout {                          # highlighted from here
      in  words:   string stream
      out loudest: string

      say = run text-upper(text: words)
      say.upper | first 1 -> loudest
    }
""")
```

Nothing has to be configured — a string that opens with a flow declaration is
treated as one — and `# language=A11Flow` covers a fragment that cannot say so
itself.

To put the language in front of a model that has to write one,
`a11.flow.REFERENCE` is a cheat sheet sized for a prompt.

## Handing the language to a model

A composition is most useful written by whoever is already holding the problem,
and increasingly that is a model with a handful of tools.
[`a11.sdk.flow_tools`](../llm-sdk/flow-skill.md) gives one the ability directly:
`flow_actions` tells it what it may compose and what each action's ports are
called, `flow_check` compiles a flow without running it, and `flow_run` runs
one. `flow_tools.get_system_prompt()` — or `get_skill()`, for a host that loads
`SKILL.md`-style skills — is the text that teaches it when to bother.

The reason to bother is the same one the language exists for: the values a
composition moves between steps never pass through the model. Three fetched
pages become one answer, and the answer is all it reads.
