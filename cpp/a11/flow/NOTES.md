# Flow: what the language cannot yet say

Written 2026-08-19 alongside the runtime optimisation pass in
`bench/FINDINGS.md`; **six of the nine gaps were closed on 2026-08-20** and are
recorded at the bottom rather than deleted, because what a language chose *not*
to have is as much a fact about it as what it has. This is about
expressiveness, not performance.

## What is there

Statements: `run` / `call`, `try` (a call, a block, or a **pipe**), `let`,
`advance`, `skip`, `wait` (also `wait first of` / `wait all of`), `drain`,
`abort`, `cancel`, `fail`, `log` / `logf`, `if` / `else`,
`for ... [parallel n]` and `repeat ... [max n]`, both with `until` / `while`,
both nameable and both taking `after`, `nodes`.

Stages: `first`, `last`, `drop`, `truncate`, `batch`, `window`, `flatten`,
`group`, `sort`, `where`, `map`, `scan`, `match`, `distinct`, `then`, `log`,
`logf`, `mime`, `strformat`, `chunk`, `collect`, `count`, `sum`, `min`, `max`,
`avg`, `fold`, `join`, `text`, `json`, `packb`, `timeout`, `pace` — any of them
as `try STAGE [into DEST]`, and the per-value ones with
`parallel n [unordered]`.

Sources: a port, a node, a header, an expression, `status x`, `zip(a, b)`,
`interleave(a, b)`.

Destinations: a node, an `out` port, a call's input port, and `_` -- which
performs the pipeline and keeps none of it.

Expression functions: `len`, `keys`, `values`, `get`, `join`, `split`, `slice`,
`replace`, `trim`, `lower`, `upper`, `contains`, `starts-with`, `ends-with`,
`default`, `merge`, `to_chunk`, `from_chunk`, `now`, `text`, `json`, and the
type constructors.

## What is still missing

### 1. Ordered concurrency, the other half

`parallel n` on a stage keeps the order; `for ... parallel n` still does not,
and a loop body is where a composition puts work that is more than one stage.
`for` has no ordered-parallel form, and the runtime measurement says to fix the
pool's spreading before adding one (see `bench/FINDINGS.md`: 16-wide is 1.5x the
per-pass cost of sequential).

### 2. `distinct` and `group` have no key

`distinct` compares whole values; `distinct by it.id` is the form records want.
The same argument applies to a `group by it.user` that gathers by key rather
than by a closing condition — a different feature from today's `group`, and the
collision of names is worth settling before either is added.

### 3. Time, beyond a gap and a rate

`| timeout` bounds the gap between values and `| pace` spaces them out. There is
still no `window 1s` (what arrived in the last second, as a list), no `debounce`,
and no `sample` — the *dropping* counterpart of `pace`.

`| window n` exists as of 2026-08-20 and is a **count**, not a duration. The two
are the same question asked of a different axis and the natural shape is one
stage with two argument kinds, which the `StageArgument` table already supports;
whoever adds the duration form should add it to `window` rather than beside it.

### 4. Every count is still a literal

`first`, `last`, `drop`, `truncate`, `batch`, `window` and `repeat max` all
refuse a name: `Expected a count for 'first', found 'want'`.

**The load-bearing case is closed** by `for ... until`, below: consuming exactly
the number of values something told you at run time -- the `Content-Length: n`
shape, where reading past the nth value is wrong and not merely wasteful -- is

```a11flow
let want = input.lines | first 1 | map number(it)
for line in input.lines | drop 1 {
  line -> taken
  until index + 1 >= want
}
```

and a `repeat` bounds itself the same way with `until index + 1 >= want`, so
`max n` never needs to be dynamic either.

What is left genuinely inexpressible is a runtime *width*: `truncate want`,
`batch want`, `window want`. None of those is a loop bound, so no `until`
rewrites them. Doing it properly means a `RefId` beside `Stage::count`, read once
before the stage starts, and `StageMakesOne` going conservative -- `first n` can
no longer be known to make one value when `n` is not known. Worth having; not
worth guessing at, since nothing has asked for it yet.

### 5. Reading a node out of order

A stage may now work on several values at once, but the values still arrive in
the reader's order. A node whose fragments arrive out of sequence — a remote
one, or one several peers write — could be read in *arrival* order, which is a
switch on the node's reader rather than anything the language says. Worth doing
where a pipeline does not care about order at all, and it needs a seq on the item
to be able to put things back.

## Closed on 2026-08-25: what an `after` holds

### An `after` holds the statement's arguments too

`Y after X` ordered when `Y` *ran* and not when `Y`'s arguments were *read*, so
`t = run act(p: strformat("%s", now() - started)) after done` reported 561us
against a 300ms call: the call waited and the argument did not. A language change
rather than a bug fix, and the direction is the one that makes the text true --
what a barriered statement reports is what was true by the time it ran.

Everything else already behaved. A step reads its sources inside `Scope::Execute`,
which `RunStep` only reaches once `step.after` is satisfied, and `Bus::Pump` waits
for a first reader on purpose. The leak was in the resolver: `ResolveCall` hung
the `after` on the **call step** and then emitted one feed pipe per argument with
none, so the feeds ran at once. They now carry the same barrier, which is what
`kPipe` and `ResolveSkipTarget` already did for the several steps one statement
becomes.

The trap: copy `after_waits_` into a local *before* resolving the arguments. An
argument may resolve an `after` of its own -- an inline `wait first of a, b` --
and overwrite the member half way through the loop.

Left alone deliberately, both noted rather than fixed:

* An inline `wait first of` read as a value inside a barriered statement does not
  inherit the barrier. `ResolveRaceValue` would have to know the enclosing
  statement's barrier, and a pipe resolves its source *before* its `after`, so
  the member is the previous statement's there -- a stale barrier silently
  applied is worse than one not applied.
* A materialised stream -- one read inside a loop or a branch -- is filled by
  `Buffer::Fill`, spawned unconditionally in `Scope::Run`, so its producer starts
  regardless of any `after`. It only shows when such a stream carries an
  effectful stage (`| map now()`), and gating it the way `Bus::Pump` is gated
  hangs a loop that runs zero passes and so never reads the replay. It needs its
  own "nobody will read this" signal.

### Four traps the language now names

The measurements that came with the change, each now a published code:

* `flow.barrier.unordered-clock` -- `now() - started -> elapsed` at the top of a
  body reports 409us, because it runs at once with everything else. Fires only
  when the clock is read *against* something the flow produced: `now() -> started`
  is a start stamp and stays quiet.
* `flow.barrier.wait-lends-node` -- `wait n` on a node the flow lends by `n.id`
  rather than writes returned in 472us and the callee died with
  `ChunkStoreWriter is closed`. `NodeOutcome` forces `Destination::End()` when the
  node has no writers here, so the barrier is an *ending*. `drain n after <call>`
  is the one that waits.
* `flow.barrier.value-read-twice` -- one statement reading one node twice where a
  value belongs takes two values off it: `"at=%s took=%s" started, now() - started`
  prints an instant for `took`, and the other way round prints nothing for `at`.
  Documented as undefined already; a `let` is the fix, because a value is shared.
* `flow.barrier.after-reads-subject` -- the one the change created. An argument
  reading an output of the call its own statement waits for cannot run at all
  now: the feed will not read until the call has finished, and the call cannot
  finish while nothing reads its output. An error, not a warning -- there is no
  arrangement of it that works.

## Closed on 2026-08-25: the discard

### `-> _` -- do the work, keep nothing

`skip` was the only way to say "I do not want this", and it says the wrong thing
about a pipeline: `skip` is about *values nobody wanted*, so a counted one is
taken off the stream where it is produced and the statement is elided entirely.
There was no way to say "run this and keep no result", which is what
`pages | map summarise(it) -> _` says. It is a `kPipe` step like any other, with
`Step::discard` set, `destination` left at `kNone`, and a reader slot of its own:
performed, unlike a counted `skip`, and holding nothing open, unlike a node.

`_` is a node of the grammar (`syntax::Discard`) and not a name, following `it`:
a word legal in exactly one position. It is refused as a source, a `wait`/`drain`
/`abort`/`skip` subject, inside any expression, and as any declared name --
`Resolver::Define` is the one place every name arrives, so one check there covers
ports, headers, calls, nodes, node maps, barriers, `let`s, loop variables and
carries. Two codes: `flow.name.discard-read` and `flow.name.discard-bound`.

### A `log` may print something it read

Not a language addition but a bug the language had all along: a `log`/`logf`
keeps its arguments in `graph::LogTail::arguments`, which `Sources`/`ValueSources`
did not enumerate. So a log naming a stream was an uncounted reader of it and the
flow died at run time with "has no reader slot left", having compiled clean --
which is why "how long did that take" had to be written as two timestamps.

## Closed on 2026-08-20, third pass: endings and failure

### A loop is a step you can name

`done = for x in s { .. }` binds a name that reads the loop's own outcome, and
`for`/`repeat` take an `after`. What this is *for* is saying a program's finished
state out loud: `drain taken after done` was previously inexpressible, and the
node was ended anyway -- a loop counts as one writer of an outer node for as long
as it runs, so the last `Release` closed it -- which meant the finished state had
to be inferred from writer counting rather than read off the text.

The trap, and there is a test pinning it: `ResolveAfter` makes `kWait` steps of
its own and `ResolveBind` names a statement's *first* step, so the `after` must
be resolved **after** the loop's own `NewStep` or the name silently binds to a
wait. The loop's outcome ref is made lazily, only when something names it, so an
unnamed loop's graph is byte-for-byte what it was.

### `abort node` -- the other ending

`drain` writes both facts that end a stream (final, then closed) and says it is
over; `abort` says it went wrong. Without it a reader could not tell a stream
that finished from one cut short by something the flow noticed. Reaches
`AsyncNode::AbortWithStatus`, which the language had no path to at all. Takes
`fail`'s code/message grammar, and only a node this flow *writes* may be aborted
by it.

The two half-endings stay absent on purpose: `Finalize({.close = false})` and a
bare `Close()` exist for a producer that cannot say which chunk was last, and a
flow always can. Offering them would let a flow leave a node closed but not
final, which a reader consuming it meets as `failed_precondition`.

### `try` on a pipe

`p = try src -> dest` makes a failure arriving from the source, or refused by the
destination, a value instead of the flow's end -- previously survivable *only*
when the source happened to be a `try` call's port. Both halves are under the one
`try`, because from the statement they are one event. Tolerated at the statement
and not in the reader: a reader's tolerance is a property of the ref, reached
through a bus shared by every reader of it.

Unbound, it is a weak warning (`flow.unused.try-pipe`): a tolerated pipe that
failed leaves its destination closed early and every reader sees an ordinary end
of stream. A bound multi-target pipe is refused, because that is several steps
and a name can only be one of them.

`try` now fronts three shapes told apart by what follows it, and the lookahead
is the sharp edge: `Peek(0)` is `Current()`, so asking about "the next word" from
offset 0 asks about the `try` itself -- which reclassified every `try run` in the
repository as a pipe until the corpus test caught it.

### `stop` was removed

Added earlier the same day and taken out again: making it a language construct
meant the *compiler* had to know `control_events`, an SDK convention, and a
compiler whose point is that it can check a file with no actions registered
should not know one library's habits. Asking an action to finish is
`{"command": "stop"} -> x.control_events`, spelled out. `examples/006-flow-programs/poll.flow`
does that, and `try` on that pipe is now available where the sugar never allowed
it.

## Closed on 2026-08-20, second pass

### Early exit from a `for` -- `until` / `while`

`for` takes `until`/`while` in its body, exactly as `repeat` does and with the
same meaning: asked at the *tail* of a pass, so the body always runs at least
once and the value that ended the loop was seen. It stops reading, which is what
`| first n` does, and like `first n` it does **not** cancel whatever was
producing -- see `Reader::Stop` for why that is deliberate, and note that the
`first` stage's own documentation used to overstate it.

Refused with `parallel n`: the question is about the pass that just finished, and
with several in flight there is no such pass, so which values were seen would
depend on scheduling. `<-` stays a `repeat`'s -- a `for` takes its value from its
stream and has nothing to hand the next pass.

### A deliberate stop that succeeds (`stop x`, since reverted -- see the third pass)

`cancel x` aborts a step and the run ends `cancelled` with a non-zero exit code.
That is right for a cancellation and wrong for a deliberate stop, and there was
no other word: ending an endless step cleanly meant writing
`{"command": "stop"}` to its `control_events` port by hand, which needs a node of
its own and a cycle through it from inside the loop reading that step's output.

`stop x` is that pipe, said in one word, and it is resolved as *exactly* that
pipe -- no step kind of its own and no runtime of its own. Resolving it as a
write is also what makes it work at all: an input port nothing writes is closed
as soon as the action starts (`CallHandle::unclosed`), so a `stop` built any
other way would be writing to a port that had already gone.
`examples/006-flow-programs/poll.flow` was three lines shorter and one node
lighter for it -- and it is back to the explicit pipe, because the word made the
compiler know an SDK port name. The finding that stands is the *mechanism*: an
input port nothing writes is closed as soon as the action starts, so the pipe has
to be written for the port to still be there.

### `advance` in a loop is refused

`advance` rewrites the name to `source | drop k | first 1` with `k` fixed while
the file is compiled, and a loop body is resolved *once* -- so advancing a name
bound outside the loop bound the same value on every pass. Measured: four passes
over `a b c d e` bound `a` four times, silently. Now
`flow.form.advance-in-loop`, an error, naming `for` as the thing to write
instead. A name bound *inside* the body is untouched: each pass rebinds it, so
advancing it reads two values per pass, which is what it says.

### A line break inside brackets is whitespace

`{"a": x\n or y}` was `Expected }, found 'or'` while a break straight after a
`,` in the same record was fine, because the loops that read a comma-separated
list skip newlines themselves. One rule now: inside `{ }`, `[ ]` or `( )` a break
ends nothing, so an operator may begin the next line. Outside them a break still
ends the statement, which is what keeps a `where` on the line below from being
read as a continuation of the pipe above it.

### The formatter indents a fold/scan argument under its stage

A comma at the end of a line says the statement is not over, so what follows is a
tail of it -- two levels, as a modifier tail gets, so the argument does not land
exactly where the next `|` would. Before this, a `fold`/`scan` expression on the
next line began with a `{` or a name, neither of which read as a tail, and was
pushed out to the *block's* indent.

## Closed later on 2026-08-20: state, and neighbours

Two gaps that this file had not named, found by trying to write four programs
that a pipeline cannot express (`examples/006-flow-programs/span.flow`,
`extract.flow`, `poll.flow`):

* **`scan`** — `fold` with every state it passed through published rather than
  only the last. This was the gap with no workaround at all: a state machine over
  a stream needs the state *at each value*, and neither of the two constructs
  that look like they should provide it does. `repeat` carries state across
  passes but re-reads its stream from the start on every pass; `for` walks a
  stream but carries nothing between passes. `scan` is one value of state and
  nothing per value of the stream, so it holds in constant memory.
  Its start may be a **record**, which `fold`'s could not be: the reason for that
  restriction is that `0 as total` reads as a cast, and `{ .. } as total` does
  not, because the braces are read before `as` is looked at.
* **`window n`** — `batch` with the lists overlapping. Not a convenience: a
  `batch` has to put a boundary *somewhere*, so a question about neighbouring
  values loses roughly one answer in `n` and says nothing about it. A phrase
  spanning a line break is the ordinary case, and `grep.flow` could not ask it.

Both are per-value and neither is in `ParallelStages`: `scan` carries state and
`window` carries order, so there is nothing for `parallel n` to do to either.
`scan` preserves the count (`StagePreservesCount`); `window` does not, because
its first `n - 1` values produce nothing.

## Closed on 2026-08-20

Six gaps, in the same order this file first listed them: **aggregation**
(`sum`/`min`/`max`/`avg`/`fold`, and `sort`), **flattening** (`flatten`),
**fan-in** (`interleave`, and `wait first of`), **per-item failure**
(`try STAGE`, `into DEST`), **stage timeout** (`timeout`), and **pacing**
(`pace`). What each of them settled, where the answer was not obvious:

* `interleave`, not `merge`: `merge` is already an expression function over
  records, and a source word of the same name would shadow it exactly where
  `merge(a, b) -> out` is a reasonable thing to write.
* `wait first of` is a **value**, not only a barrier: the number of whichever
  won, so `-> n`, `let n = ...` and `n = ...` all read it. `wait all of` has no
  winner and stays a barrier.
* `wait first of` races **calls**, not nodes. A node is finished when whoever
  writes it says so, which is what `wait` and `drain` are for — and a race over
  several status *reads* would need a fiber per candidate that nothing could
  wake once the race was over.
* `fold`'s starting value is a literal, because `fold 0 as total` read as an
  expression is a cast of `0` to a type called `total`.
* `try` on a stage drops and logs; `into` routes. A language whose failures are
  values should not silently discard one it was told to tolerate.
* `timeout` is a *gap*, not a total: a whole-step budget is `wait ... timeout`,
  which already existed.
* `pace` slows and never drops. The dropping form is `sample`, which is item 3
  above.

## What is deliberately absent and should stay absent

For the record, so these do not get re-proposed as gaps:

* **No user-defined functions.** A flow calls actions; the host defines what an
  action is. A function would be a second, weaker way to name computation.
* **No arithmetic beyond `+`, `-` and the folded constant expressions.** The
  language is about moving values between actions, and every operator invites
  the next one. (`| avg` divides, and that is a mean rather than the beginning
  of arithmetic.)
* **No mutable variables.** `let` names a value; a `repeat`'s carry and a
  `fold`'s accumulator are the two places a value crosses a boundary, and both
  are explicit.
* **No `import`.** A flow file is one document; composition is `call`.
