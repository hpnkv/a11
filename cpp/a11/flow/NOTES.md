# Flow: what the language cannot yet say

Written 2026-08-19 alongside the runtime optimisation pass in
`bench/FINDINGS.md`; **six of the nine gaps were closed on 2026-08-20** and are
recorded at the bottom rather than deleted, because what a language chose *not*
to have is as much a fact about it as what it has. This is about
expressiveness, not performance.

## What is there

Statements: `run` / `call`, `try`, `let`, `advance`, `skip`, `wait` (also
`wait first of` / `wait all of`), `drain`, `cancel`, `fail`, `log` / `logf`,
`if` / `else`, `for ... [parallel n]`, `repeat ... [max n]` with `until` /
`while`, `nodes`.

Stages: `first`, `last`, `drop`, `truncate`, `batch`, `flatten`, `group`,
`sort`, `where`, `map`, `match`, `distinct`, `then`, `log`, `logf`, `mime`,
`strformat`, `chunk`, `collect`, `count`, `sum`, `min`, `max`, `avg`, `fold`,
`join`, `text`, `json`, `packb`, `timeout`, `pace` — any of them as
`try STAGE [into DEST]`, and the per-value ones with `parallel n [unordered]`.

Sources: a port, a node, a header, an expression, `status x`, `zip(a, b)`,
`interleave(a, b)`.

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

### 4. Early exit from a `for`

`repeat` has `until` / `while` and `max`; `for` has neither. Stopping a `for`
early is `first n` on its source, which needs the number in advance rather than
discovering it. `break` / `continue`, or an `until` clause on `for`.

### 5. Reading a node out of order

A stage may now work on several values at once, but the values still arrive in
the reader's order. A node whose fragments arrive out of sequence — a remote
one, or one several peers write — could be read in *arrival* order, which is a
switch on the node's reader rather than anything the language says. Worth doing
where a pipeline does not care about order at all, and it needs a seq on the item
to be able to put things back.

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
  several status *reads* would need a fibre per candidate that nothing could
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
