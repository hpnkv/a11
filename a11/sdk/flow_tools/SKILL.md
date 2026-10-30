---
description: Compose the tools you already have into one A11 Flow and run it as a
  single step, so the values that move between them never pass through your context.
  Use it when a task needs several tools and each one's output feeds the next, when
  the same shape of work repeats over a list of things, or when an intermediate result
  is large and you only need what it reduces to. The tools are flow_actions, flow_check
  and flow_run.
name: a11-flow

---
# Compose tools into one flow

A flow connects existing actions and dispatches the composition as one step. Use
`flow_actions` to discover available actions, `flow_check` to validate the
source, and `flow_run` to execute it.

For example, a flow can fetch several pages, stream each result into a
summarizer, and return only the summary. Intermediate pages move directly
between actions instead of entering your context.

## When to use a flow

Use a flow when:

- **Data flows tool to tool.** The output of one call is the input of the next,
  and you do not otherwise need to see it.
- **The same shape repeats.** The same two or three calls for each item in a
  list. `for` runs a block per value, `parallel n` runs several at a time.
- **An intermediate is large.** A page, a transcript, a file listing, a
  recording. Trim it inside the flow with `| truncate`, `| first`, `| where`,
  and return what it reduces to.
- **Steps should overlap.** Steps run concurrently and data dependencies
  establish their order. A summarizer can read pages as the fetches complete.

A direct tool call is clearer for one action. Split the work into separate flows
when you need to inspect a result or make a decision between calls.

## Write and run a flow

1. Call **`flow_actions`** to get action names, port names, and each action's
   `runnable` flag.
2. Write the flow with the inputs you will supply and the outputs you want back.
3. Call **`flow_check`** with the source. It resolves the composition without
   dispatching actions and reports diagnostics with line and column positions.
4. Call **`flow_run`** with `source` and an `inputs` object keyed by input port.
   Pass a list to a `stream` port and one value to any other port. The result is
   an object keyed by the flow's output ports.

## Runtime choices

- **Declare focused outputs.** Return the answer, count, or selected hits rather
  than large intermediate values you do not need to inspect.
- **`run` what is `runnable`, `call` what is not.** The two verbs are two
  different things: `run` executes the action where the flow is running, `call`
  puts it on the stream the flow is attached to and lets the peer do it.
  `flow_actions` marks each one, and almost everything it offers you is
  `runnable: true` — so `run` is the usual verb, and a `run` of something
  without a handler is refused rather than quietly sent elsewhere.
- **Use listed actions.** `flow_actions` is the set of names the flow may
  resolve.
- **Every output port of every step is read.** The runtime drains outputs you do
  not bind. `skip 1 x.rows` drops a port's first value for *every* reader, which
  is how you throw away a header line, and several statements naming one port
  add up.
- **A failing step ends the flow** unless you wrote `try`. When a failure is one
  the composition should handle, write `try run` (or `try call`) and then `wait`
  to find out how it went.
- **Give a port the type it wants.** When an action's input is a real type
  rather than a bag of keys, write `a11::sdk::Interaction{role: "user",
  content: [...]}` (or `{...} as a11::sdk::Interaction`) and it is validated
  into that type, defaults and all. `std::to_chunk(value)` makes the `Chunk`
  such a type's content is made of. Which tags exist is the host's — ask
  `flow_actions` what the ports are and match them.
- **A stream of fragments is not a stream of things.** `| group EXPR` gathers
  values until `EXPR` holds of the one just added, then hands over the list: `|
  group std::endswith(std::trim(it), [".", "?"]) | map std::trim(std::join(it, "
  "))` is partial utterances becoming whole sentences. Reach for it whenever one
  value is only part of what you need.
- **Order between two streams is `| then`.** Two statements writing to the same
  port interleave by arrival. When one lot has to come before another — a
  conversation's history before the new message — write `history | then asked ->
  port` and the flow reads them in that order.
- **Keep computation in actions.** Flow expressions read, compare, transform,
  and assemble values; use an action for application-specific computation.
- **Say when a loop stops.** A `repeat` needs an `until`/`while`, or a `max n`,
  or both. There is no default bound: a loop with neither is refused rather than
  quietly stopping after some number of passes and calling that success.
- **Gate side effects with data or `after`.** `log`, `fail`, `cancel`, and
  `abort` evaluate their arguments and otherwise overlap other statements. An
  `if` or loop body supplies a condition; `log "ready" after prepared` supplies
  an ordering dependency.
- **`log` needs no port.** `log "searching" after plan` and `logf "found %s" n
  after search` write to a log the flow already has: nothing declares or drains
  it, and it is not a model-facing output. As a stage, `| log` and `| logf "saw
  %s" it` say what is going past and pass every value on unchanged, which is the
  way to see into a pipeline without changing it.

## The language

```
A11 Flow composes streaming actions into actions.
Keywords are lower or UPPER
case. Statements overlap; data and after dependencies determine execution.

flow NAME [exported] {
  in input: string [stream] [required]
  out output: string [stream]
  job = run local_action(input: input) timeout 30s
  job.output | map std::upper(it) -> output
}
call invokes a peer; run invokes a local handler. All outputs are read.

x = expression or T x = expression initializes once per scope, eagerly.
Reassignment follows source order, including child-scope updates. Parallel
conflicting updates are rejected. Names and destructuring patterns are lvalues.
x = take source consumes one successive item; EOF fails.
x = try take source returns {present: bool, value: T when present}; errors fail.
A real null item has present=true. Read the RHS of destructuring only once.

for item in source [parallel N] { body }
  [until EXPR | while EXPR] [after subject]
repeat [state = INITIAL] [max N] {
  body
  state <- NEXT
} [until EXPR | while EXPR]
Postconditions run after the entire pass, in that pass's scope. Postconditions
require sequential loops. A repeat needs a condition or an explicit bound.

node() creates a node. close NODE ends it normally; abort NODE [CODE [MESSAGE]]
ends it with an error. wait SUBJECT observes completion; status SUBJECT is its
status. cancel ACTION cancels an action. fail [CODE [MESSAGE]] fails the scope.
try run/call or try { body } exposes a failure through the operation's status.
wait first of a, b chooses a completion; wait all of a, b waits for every one.
Bare wait all is a scope-local execution barrier: all preceding work must
complete successfully before any following statement activates. This includes
initializers, arguments, eager deadlines, owned calls, feeds, draining, and child
scopes. Work within a phase remains concurrent. Barriers inside branches,
blocks, and loop passes affect only that scope or pass. Stream aliases do not
read or exhaust streams; cross-phase producer/consumer cycles are errors.
Bare wait all accepts no modifiers or value binding. wait all of a, b remains
an explicit-subject wait and does not gate following statements.
after a, b { body } gates a scope. Bound blocks and loops expose completion.

Call and wait modifiers: timeout DURATION_EXPRESSION, deadline TIME_EXPRESSION,
after SUBJECTS. Deadlines evaluate once eagerly; an after clause delays the
whole expression. deadline std::now() + 30s after action and the parenthesized
form have identical meaning. timeout timeout and deadline deadline are valid
when the names hold the corresponding values. The earlier bound applies when
both modifiers are present. Expiry is DEADLINE_EXCEEDED.
Other call modifiers: via MAP, tee, id EXPR, with "header": EXPR,
forward headers "name". Local nodes stay off the wire.

source | stage -> destination [, destination]; -> _ discards results.
source | run flow_name(config: value) | call module::flow_name() composes
whole-stream invocations. After explicit arguments, exactly one input must
remain (optional inputs count too), and exactly one output must be declared.
Unary inputs validate zero/one or exactly one item according to required.
Declare/import external interfaces before using them as stages. Calls retain
ordinary dispatch, modifiers, draining, failure, and cleanup semantics.
try run/call tolerates the invocation, not failures reading its output;
use a try block for the whole pipeline, or combine a try invocation with a
try pipe to tolerate transfer errors too. No item is dropped.

Stages include map EXPR, where EXPR, first N, drop N, collect, flatten,
scan INITIAL as state, EXPR, fold INITIAL as state, EXPR, sum, count,
take while EXPR, take until EXPR, as TYPE, json, packb, timeout DURATION.
Stages stay unqualified. Functions are qualified: std::len, std::upper,
std::join, std::match, std::strformat, std::now, std::duration, std::time.
from std import now makes now() available without a prefix.
std::parse_time(text) and std::parse_duration(text) fail on invalid text;
std::time(text) and std::duration(text) retain permissive conversions.

mod name [exported] { declarations }
declare mod name [exported] { contracts, declared modules, full structs }
declare flow company::search::find { interface }
struct Row [exported] {
  name: string required
  count: int
}
import "path/library.flow" [as alias] loads a module; default name is library.
from library import member [as alias], other; from library import *.
export library::member [as alias] publishes an accessible existing symbol.
Modules cannot be created inside flows or structs. std is reserved.
:: qualifies static modules/types; . projects runtime fields/ports.
Host type source a11::sdk::Interaction retains wire tag a11.sdk.Interaction.

Primitive prelude: int/integer, bool, number, string, bytes, object, null;
literals true, false, null. Exposed types expose their conversions too.
Other standard types: std::status, std::time, std::duration, std::list[T],
std::option[T], std::stream[T]. object[V] has string keys and one value type;
bare object is heterogeneous. Typed initialization uses existing coercions and
checks the target invariant. Initializers may not be omitted.

File imports require explicitly approved source_roots; none means filesystem
imports are disallowed. Explicit in-memory modules remain available. Parsing
and compilation have configurable depth, size, work and time limits.
```

## An example

The action names below are examples — use the ones `flow_actions` gives you.

```a11flow
flow answer-from-the-web {
  describe "Search, read the best hits, and answer from them."

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
    }
  }

  brief.summary -> answer
}
```

Run that with `inputs` of `{"question": "..."}` and you get back `{"answer":
"...", "sources": ["...", "..."]}` — two small values, out of three pages you
never had to read. The `nodes fetched` block is what keeps the pages off the
wire; without it they would be replicated to whoever dispatched the composition.
