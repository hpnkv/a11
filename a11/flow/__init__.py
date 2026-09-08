# Copyright 2026 The A11 Authors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

r'''Flow: a small language for composing A11 actions.

A *flow* is a composition of existing actions that is itself an action. It
declares ports and headers, calls other actions, pipes their streaming ports
into one another, loops, branches, and hands its own outputs back. Because it
presents an ordinary [ActionSchema][a11.actions.action.ActionSchema], anything
that can dispatch an action can run one without knowing it is a composition.

Flow documents can arrive through a gateway, client, or model and run without a
repository change or deployment.

```a11flow
flow shout {
  in  words:   string stream
  out loudest: string

  say = run text-upper(text: words)
  say.upper | first 1 -> loudest
}
```

## Reading a flow

Every statement is one of a handful of shapes, and the whole language fits on a
page:

| Statement | Means |
| --- | --- |
| `x = run an-action(port: src)` | run an action here, feeding a port |
| `x = call an-action(port: src)` | dispatch one on the attached stream |
| `x = node([id]) [in map]` | a node of the flow's own, to write and read back |
| `nodes map` | declare a node map to keep traffic out of the session |
| `source -> port, port` | pipe a stream into one or more node(s) |
| `source \| stage \| stage -> port` | reshape it on the way |
| `source \| stage -> _` | the same, keeping no result; `_` is not a name |
| `skip source` | read a stream to its end and discard the values |
| `skip n port` | drop a node's first `n` values, for every reader |
| `s = wait x` | hold until `x` is finished, and say how it went |
| `drain node` | end a node: mark it final and close it |
| `abort node [code] [msg]` | end a node with a *failure*, so readers see why |
| `status x` | the same outcome, read where a value is expected |
| `[name =] for v in source [parallel n] { }` | run a block per value |
| `repeat s = start [max n] { }` | repeat a block with carried state |
| `s <- source`, `until e` | what a `repeat` carries, and when a loop stops |
| `if e { } else { }` | run one block or the other |
| `s = try { }` | run a block as one step, and say how it went |
| `p = try source -> port` | turn a pipe failure into a value |
| `cancel x` | ask a called action to stop |
| `fail [code] [message]` | end the flow with a status |

Every significant word may be written in lower case or upper case — `for` or
`FOR`, `stream` or `STREAM`, `not_found` or `NOT_FOUND`. Mixed case is not a
keyword, so `For` is a name.

**Steps run concurrently.** Statement order does not define execution order. A
call starts immediately and receives inputs as they arrive. Use `after`, `wait`,
or `drain` when execution order matters.

## A11 operations in Flow

Flow provides syntax for these A11 operations:

* **`run` and `call`, which are two different things.** `run some-action(...)`
  executes the handler registered where the flow is running; `call
  some-action(...)` puts the action on the stream the flow is attached to and
  lets the peer do it. This matches `Action::Run` and `Action::Call`. A
  composition written against a gateway's
  actions `call`s them; one composing actions of its own `run`s them; a client
  flow doing retrieval here and inference there does both, in the same flow.
  `run` requires a local handler and does not fall back to the session.
* **`skip`, and stages that cut a stream down.** `skip x.debug` reads and
  discards an output, preventing an undrained output from stalling its producer.
  `skip 1 x.rows` takes the first
  value off the node itself, for *every* reader of it, which is how a header
  line stops being everybody's problem — `| drop 1` only trims the one reader
  that says it. Several of them naming the same node add up. `-> _` is the
  complementary form: `skip` bypasses processing, while `_` discards the result
  after the pipeline runs. `pages | map summarise(it) -> _` therefore
  summarises every page. `_` is a destination, not a name, and cannot be read.
  `| first 3`,
  `| truncate 4000`, `| where it.ok` and
  `| mime "text/*"` throw values away *before* they reach the next step. This
  bounds the input sent to a model. `| packb` writes a value as MessagePack. An
  existing MessagePack chunk passes through without re-encoding.
* **`nodes` blocks.** Calls inside one get a node map of their own, so their
  ports are not in the session's map and their fragments are not replicated to
  the peer that dispatched the flow. A composition that fetches ten pages and
  sends one summary back should move one summary over the wire, not ten pages.
  A `run` step already keeps its nodes off the wire unless it asks for `tee`.
* **`wait`, `status` and `drain`.** Completion and having-written are different
  events in A11, and a composition needs both. Either statement can be bound to
  a name and named in another statement's `after` — and a bound `wait` is also
  how a flow *reads* an outcome, because waiting and finding out are the same
  moment. `after` also takes a port or a node directly, meaning "once that
  stream is finished": `-> mic.control_events after sentence` stops the
  microphone as soon as there is a sentence, with no barrier to name. It holds
  the whole statement, **arguments included** — `run act(p: now() - started)
  after done` reads the clock once `done` has happened — so what a barriered
  statement reports is what was true by the time it ran. A `wait` on a node the
  flow lends to another step ends the node immediately. Use `drain n after
  <call>` to wait for that step first.
* **Flow-owned nodes.** `x = node()` makes a stream the flow can write
  from several places and read back from one; `x = node(existing-id)` attaches
  to an existing node, and `x.id` passes a node identifier to an action that
  expects to be told where to write. A node lands in the active node map,
  which is what keeps it off the wire.
* **Headers.** A11 automatically gives a nested action every `x-a11-` header of
  its parent. For other headers, such as
  an `authorization`, a tenant id — `forward headers "authorization"` sends on
  what the flow was called with, as it arrived, and `"x-tenant-*"` sends on a
  family. `with "header": expr` is the other half, for a value the flow
  *computes*; naming both, the `with` wins, because it is the more specific of
  the two.

## Failures a flow expects

Use `try` with either verb when the flow will handle a failure:

```a11flow
page = try run web-fetch(url: url)
outcome = wait page                       # wait and read the status
if not outcome.ok {
  fail unavailable outcome.message        # or `fail outcome`, unchanged
}
```

A status is data — `{"ok": .., "code": "NOT_FOUND", "number": 5,
"message": ..}` — so a flow can branch on it, put it on an output, or raise it
again. `fail` takes
any canonical code by name in either case, or a number computed at runtime, or a
status record to re-raise as it stands. Waiting on something that finished badly
ends the flow with *that* status, unless it was a `try`: those are the failures
a flow said it would handle.

## A file of several flows

A file may declare more than one flow, and a flow may `run` or `call` any of the
others by name — in whichever order they are written, and with nothing
registered for them. This supports extracting reusable or complex sections into
named flows while keeping one source document and entry point.
Ports are checked between them at compile time, exactly as they are against a
registered action, so an incompatible rename is reported before execution.

```a11flow
flow ask {                        # the piece, reusable on its own
  in  question: string
  out answer:   string stream
  said = run answer-question(question: question)
  said.text -> answer
}

flow ask-twice {                  # and a composition of it
  in  question: string
  out answers:  string stream
  first  = run ask(question: question)
  second = run ask(question: question)
  first.answer then second.answer -> answers
}
```

## Grammar

```
program    := flow+
flow       := "flow" name "{" item* "}"
item       := "describe" description
            | ("in"|"out") name ":" type ["stream"] ["required"] [description]
            | "header" string ["as" name] ["default" literal] [description]
            | statement
statement  := [name "="] call
            | name "=" "node" "(" [expr] ")" ["in" name]
            | [name "="] "wait" reference ["timeout" duration]
            | [name "="] "drain" reference
            | pipeline "->" destination ("," destination)*
            | "skip" (number reference | skip-target ("," skip-target)*)
destination := reference | "_"      # `_` keeps nothing, and is not a name
skip-target := pipeline
            | name ("," name)* "of" name
            | "(" name ("," name)* [ "of" name ] ")"
            | "cancel" name
            | "abort" reference [expr [expr]]
            | "fail" [expr [expr]]
            | "log" [level] expr
            | "logf" [level] string [expr ("," expr)*]
            | "for" name "in" pipeline ["parallel" number] block
            | "repeat" [name "=" expr] ["max" number] block
            | name "<-" pipeline
            | ("until" | "while") expr
            | "if" expr block ["else" (block | if)]
            | "nodes" name [block]
call       := ["try"] ("run" | "call") action "(" [name ":" pipeline, ...] ")"
                  modifier*
modifier   := "tee" | "via" name | "timeout" duration
            | "after" name ("," name)* | "id" expr
            | "with" string ":" expr ("," string ":" expr)*
            | "forward" "headers" string ("," string)*
pipeline   := expr (("|" stage) | bare-stage)*
bare-stage := ("then" source | "where" expr)   # the pipe is optional here
stage      := "first" n | "drop" n | "truncate" n | "batch" n | "window" n
            | "group" expr | "scan" literal "as" name "," expr
            | "match" pattern
            | "then" source | "where" expr | "map" expr | "join" [string]
            | "strformat" string | "mime" string | "collect" | "count"
            | "distinct" | "text" | "json" | "packb"
            | "log" [level] [expr] | "logf" [level] string [expr ("," expr)*]
level      := "debug" | "info" | "warning" | "error" | "critical"
type       := name ("." name)* ["[" type ("," type)* "]"] | string
description := string | newline string   # alone on its line, at any indent
string     := '"' ... '"' | '"""' ... '"""'   # the second may hold line breaks
expr        := literal | name | expr "." name | expr "[" expr "]"
            | builtin "(" expr* ")" | "(" pipeline ")" | "it"
            | "status" reference | name ".id"
            | type "{" [name ":" expr, ...] "}" | expr "as" type
            | expr ("==" | "!=" | "<" | "<=" | ">" | ">=" | "in") expr
            | expr ("+" | "-") expr        # numbers, durations, instants
            | expr ("and" | "or") expr | "not" expr
```

### Pattern matching

`match` extracts named fields from text as a stream stage or value function.
Literal text matches itself, whitespace matches any run of spaces or tabs, and
`{name}` captures text up to the next literal. For example,
`lines | match "name={name} age={age:int}"` converts `name=Alice   age=27` into a
record with `name` and `age` fields.

A hole can specify `int`, `number`, `bool`, `word`, `line`, `rest`, `duration`,
`time`, or `json`. `{}` creates a positional capture read as `it[0]`; `{{` and
`}}` match literal braces. Patterns search anywhere in a value without leading
or trailing wildcards. A hole remains on its line unless its type permits line
breaks.

The stage drops values that do not match; the function returns **null**. Literal
patterns expose their named fields to completion and diagnostics. Invalid
literal patterns produce a diagnostic at their source location.

### Field checking

Fields come from declared `struct` types and named `match` holes. Missing fields
produce diagnostics for both sources. Values typed as `object` or `json`, an
`it` value without a pattern, and positional patterns have no named-field check.
Checking stops after one level: `src.meta.title` checks `meta` and does not
validate `title`.

Types are `string`, `text`, `number`, `integer`, `bool`, `object`, `json`,
`list`, `bytes`, `any`, a quoted mimetype, or the tag a serialisation registry
knows a type by — `a11.sdk.AudioBuffer`, written unquoted, and recognised as a
tag because it is dotted. A container says what it holds in brackets:
`list[string]`, `list[a11.NodeFragment]`. The type comes first and what the
*port* is like follows it: a port carries one value unless it says `stream`, and
is optional unless it says `required`. Status codes are Abseil's canonical ones,
by name (`not_found`,
`NOT_FOUND`) or by number. Durations are written `500ns`, `250ms`, `30s`, `5m`,
`1h`, and compound as `1m30s`. Comments start with `#`. The only arithmetic is
`+` and `-`, which are there for times; there is no way to call out to code, so
an expression can read values, compare them, do that arithmetic, take them apart
and build new ones, and that is all — which is what makes a flow safe to accept
and run.

## Descriptions and docstrings

Descriptions document flows, ports, and headers. A quoted string may follow a
declaration or appear on the next line:

```a11flow
flow documented {
  describe """
    What this flow is for, at the length that actually takes.

      An indented line stays indented, relative to the rest.
    """

  in  question: string required
    "What to find out — as long as it needs to be, on its own line."
}
```

A `"""` string may hold line breaks, and its value is *dedented*: a blank first
line goes away, a whitespace-only last line goes away with the break above it,
and the indentation every remaining line shares comes off. A long description
therefore aligns with its declaration while retaining its internal layout.
Escapes work as they do in a single-quoted string, and are resolved after the
dedent, so a hand-written `\\n` is a line break and never an indented line.

A description may also stand **alone on the line below** what it describes, at
any indentation or none. That is unambiguous because the string has to be alone:
`"a literal" -> out` is a statement, since something follows the string, and a
line holding nothing but a string is not a statement in this language.

A flow constructs registered values when an input port requires a concrete
type:

```a11flow
a11.sdk.Interaction{                          # or: {...} as a11.sdk.Interaction
  role: "user",
  content: [to_chunk({"type": "text", "text": said})]
}
```

Both spellings mean the same thing: take what the expression produced, partial
as hand-written things are, and make it that type — filling in what the type
defaults and failing where it will not fit. A tag resolves against the
serialisation registries of the process the flow runs in, so which types exist
is the host's decision: a flow cannot import anything. `Tag{...}` is not
available where a `{` would open a block instead — an `if` condition, a `for`'s
source — so `if step.next.done {` keeps meaning what it looks like; brackets
lift the restriction, as in `if (T{a: 1}).ok {`.

## Running one

```python
import a11
from a11 import flow

program = flow.loads(source, "shout.flow")
program.register_all(registry)              # now they are actions

result = await program["shout"].invoke(words=["hi", "there"])
```

[a11.flow.plan.FlowPlan.invoke][] is the convenience path; in a server, register
the flows and let the session dispatch them like anything else.

See also [a11.flow.plan][] for the compiled graph, [a11.flow.runtime][] for how
it executes, and the `REFERENCE` constant in this module for a cheat sheet
compact enough to put in a prompt.
'''

from __future__ import annotations

import os
from collections.abc import Mapping, Sequence
from typing import Any

from a11._native import flow as _flow
from a11.flow.diagnostics import FlowSyntaxError
from a11.flow.plan import TYPE_NAMES, FlowPlan, Program, compile_source
from a11.flow.runtime import invoke

#: Every function an expression may call, read from the language's own table.
BUILTINS: frozenset[str] = frozenset(_flow.vocabulary()["builtins"])

#: Every pipeline stage, and what each one takes after its name: ``"none"``,
#: ``"number"``, ``"expr"``, ``"string"``, ``"string?"`` or ``"stream"``.
STAGES: dict[str, str] = _flow.stages()

#: The names `fail` accepts, in the case they are canonically spelled in.
FAIL_CODES: tuple[str, ...] = tuple(
    sorted(code.upper() for code in _flow.vocabulary()["status_codes"])
)

#: A compact language reference for models that write flows. Action schemas
#: provide the remaining information needed to compose available actions.
REFERENCE = '''\
A11 Flow — a composition of actions that is itself an action.
Every keyword may be written in lower case or UPPER CASE, but not Mixed.

flow NAME {
  describe "what this does"
  in  PORT: TYPE [stream] [required] "description"     # no `stream` = one value
  out PORT: TYPE [stream] [required] "description"
  header "x-header-name" as ALIAS default LITERAL

  X = run some-action(port: SOURCE, ...) MODIFIERS   # a handler registered here
  X = call some-action(port: SOURCE, ...) MODIFIERS  # on the attached stream
  let V[, V...] = SOURCE                   # *one* value of that stream, named;
           # several names take it apart, by field or by position
  advance V                                # rebind V to the next value of it
  N = node([ID]) [in MAP]                  # a stream of the flow's own
  nodes MAP [{ ... }]                      # a node map; keeps traffic local
  SOURCE | STAGE | STAGE -> DEST, DEST     # pipe a stream into node(s)
  SOURCE | STAGE -> _                      # do the work, keep no result
  skip SOURCE[, SOURCE...]                 # read to the end, keep nothing
  skip N PORT                              # drop its first N, for all readers
  skip X                                   # every output of a call X
  skip O[, O...] of X                      # just those outputs of X
           # (also written `skip (O, O...) of X` or `skip (O, O... of X)`)
  S = wait SUBJECT [timeout 30s]           # finished; S is how it went
  S = drain NODE                           # end a node, and say how it ended
  abort NODE [CODE] [MESSAGE]              # end a node with a failure
  cancel X                                 # ask X to stop
  fail [CODE] [MESSAGE]                    # end the flow with a status
  log [LEVEL] WHAT                         # write to the flow's own log
  logf [LEVEL] "fmt" [ARG, ...]            # the same, formatted
           # `fail`, `cancel`, `abort` and `log` wait for nothing, so they
           # go in an `if` or a loop body, or carry an `after`: at the top of
           # a body they race every other statement, and are refused there.
           # `drain NODE` and `abort NODE` are the two endings a stream can
           # have: drain marks it final and closes it, which says it is over;
           # abort says it went wrong. A reader cannot otherwise tell a stream
           # that finished from one cut short.
           # `cancel` aborts, and a cancelled run reports `cancelled`. Graceful
           # completion uses the standard-library control-port convention:
           # `{"command": "stop"} -> X.control_events`.
           # The log needs no declared port or manual drain and is created
           # only when used
  for V[, V...] in SOURCE [parallel N] { ... }   # once per value; several
                                           # names take a tuple apart
  repeat S = START [max N] { ... S <- SOURCE ... until EXPR }
           # a repeat needs an `until`/`while` or a `max`: there is no
           # default bound, and nothing ending a loop is refused
  if EXPR { ... } else { ... }
  [S =] [try] { ... }                      # these statements as one step;
           # S is how it went. A condition inside blocks only what is in
           # the braces, which is what a block is for; without `try` a
           # failure inside ends the flow, as a call's does
}

struct NAME {                              # a shape a port may be typed with
  describe "what these records are"
  FIELD: TYPE [required] [unique] [A..B] [matching "re"] [one of [..]]
         [default LITERAL] "description"
}

A description may use a "..." string or a dedented """...""" string with line
breaks. Either form may appear alone below its declaration at any indentation.
A string followed by another token is a value. Adjacent strings concatenate;
`\\"` inserts a quote. A keyword argument such as `matching` or `strformat`
accepts one literal to distinguish it from a following description.

A `struct` declares a record with named, typed, constrained fields. A port may
use the record as its type, and a value may be coerced into it. A declared shape
takes precedence over a serialisation tag with the same name and may contain or
be contained by another shape. `A..B` bounds a number, duration, instant, or the
*length* of a
string, a byte string or a list; either end may be left off (`1..`, `..200`).
A shape holding `bytes` anywhere in it cannot go through `| json`, which has
nothing to carry them in; `| packb` can.

ONE VALUE: every source is a stream. `let` reads and names one value.
      `let code = http.status_code` reads one value of that stream and
      binds it, and the name then stands *where an expression does*:
      `if code >= 200 and code < 300 { .. }`, `strformat("%d", code)`,
      `code == other`. It also acts as a one-value stream wherever a SOURCE is
      accepted: `let image = page.body` then
      `image | chunk 65536 -> upload.parts` cuts that one value into 64 KiB
      pieces. A `let` reads lazily on first use. The compiler reports unused
      bindings. An empty stream binds nothing, which `if not code` tests. A
      value is read-only.

      Reading a stream where a value belongs consumes one value. Two value reads
      from one stream receive different values from their shared view. Which
      reader receives each value is undefined; `after` orders separate statements.
      Within one statement there is no
      `after` that could order two reads of one node against each other, and the
      language reports it (`flow.barrier.value-read-twice`). A `let` is the fix:
      it names a value, and a value is shared. A stream the language can *prove*
      carries one value is
      the exception and is shared: a port that did not say
      `stream`, a header, a status, or a pipeline that reduced with `| collect`,
      `| count` or `| first 1`. Those promise one value, so a second arriving
      ends the flow with `invalid_argument`.

      `advance V` rebinds a `let` value to the *next* value of the same stream,
      which is how a flow reads several values of one stream one at a time and
      knows which is which: `let word = words`, use it, `advance word`, use it
      again. The guarantee is positional: the *k*th binding of a name is the
      *k*th value of its stream however the flow is
      scheduled — so it holds without a barrier. Statements written above an
      `advance` keep the value they were resolved against, which is what makes
      the name read top to bottom. Advancing past the end binds nothing.

      Several names take one value apart: `let name, age = user` by field, and
      `let first, second = pair` by position. The value determines the form:
      each name is looked up as a field, then as a position when no such field
      exists. For example:
      `let name, age = match("name={name} age={age:int}", line)`
      reads what a pattern named. A part has no stream of its own, so `advance`
      reports an error on it.

SOURCE is a port (in-port, X.out-port), a node, a loop variable, a `let` value,
a header alias, a literal, `status SUBJECT`, `N.id`, `zip(SOURCE, ...)`,
`interleave(SOURCE, ...)`, or any of those with `.field` / `[i]`.
`interleave(a, b, ...)` reads several streams *at once* and gives one stream of
their values in the order they arrive, so a fast stream is not held behind a
slow one. `zip` is the other shape: one tuple per round, in step.
`zip(a, b, ...)` reads several streams in step and gives one stream of tuples,
read as `it[0]`, `it[1]`, or taken apart by `for x, y in zip(a, b)`. A source
that ends *well* contributes a null to every tuple after it, so the longer
stream is still read to its end; one that ends with an *error* ends the whole
iteration with that status. It stops when every source has, and it is a stream
like any other — `wait`, `drain`, `| first n`, `| drop n`, `| count` all work
on one.
DEST is an out-port, X.in-port, or a node.
SUBJECT is a call, a node, a port, or a named wait/drain.
`wait first of a, b` holds until the first of several *calls* finishes and lets
the rest carry on; `wait all of a, b` holds for every one of them.
A race is a value too: which one won, from zero. `wait first of a, b -> n`,
`let n = wait first of a, b` and `n = wait first of a, b` all name it.
`wait all of` has no winner, so it is a barrier only.
MODIFIERS: tee | via MAP | timeout 30s | after X, Y (a step, or a port/node
           to wait for) |
           id EXPR | with "header": EXPR, ... |
           forward headers "x-name", "x-family-*" (send on the headers this
           flow was called with, as they arrived; `*` matches a family, and an
           explicit `with` of the same name wins. Every `x-a11-` header already
           reaches a step, so this is for the others.)
           ("try run"/"try call" tolerate
           failure). `run` needs a handler registered where the flow runs and
           keeps its nodes off the wire; `call` needs none and goes to the peer.
           Either may name another flow of the same file, in any order, and
           needs nothing registered for it: a composition can be factored into
           several flows and still arrive as one text.
STAGES: first N | last N | drop N | truncate N | batch N | window N | flatten |
        chunk N | group EXPR | sort [by EXPR] [desc] | then SOURCE |
        where EXPR | map EXPR | join "sep" | strformat "fmt" | mime "text/*" |
        collect | count | sum [EXPR] | min [EXPR] | max [EXPR] | avg [EXPR] |
        fold LITERAL as NAME, EXPR | scan LITERAL as NAME, EXPR | distinct |
        text | json | packb | timeout 30s | pace 100ms | log [LEVEL] [EXPR] |
        logf [LEVEL] "fmt" [ARG, ...]
      try SOURCE -> DEST is the pipe's own form of the same word: a failure
      arriving from the source, or refused by the destination, becomes a status
      value. Bind it -- `p = try src -> dest` -- and
      `status p` says how it went; unbound, a failure is silence and the
      language says so. It differs from `try` on a *stage*: a stage fails once
      per value and carries on, while a pipe fails once and stops.
      A loop may be named too: `done = for x in s { .. }` reads as the loop's
      own outcome, so `drain taken after done` is how a flow says "once the
      loop is over, that node is over". `for`/`repeat` also take an `after`.
      Any stage may be written `try STAGE` — a value the stage cannot do is
      dropped and logged instead of ending the flow — and a `try` stage may say
      `into DEST` to send those failures somewhere as status records:
      `docs | try map it as Order into bad -> good`.
      A per-value stage may say `parallel N` to work on N values at once, and
      what follows still reads them in the order they arrived. `unordered`
      gives that up for whatever it saves:
      `urls | map fetch(it) parallel 8 -> bodies`. Use it for substantial
      per-value work such as a host round trip or coercion.
      chunk N cuts each value into pieces of at most N *bytes* — the sizes
      people write are byte counts, because they are about a frame or a buffer.
      Text chunks stop at character boundaries. A value
      with nothing to cut goes through whole; `batch N` is the one that groups
      several values into one.
      then and where may drop the `|`: `history then asked`, `hits where
      it.ok`. Every other stage keeps it.
      strformat "fmt" is `map strformat(fmt, it)`, the one-value shorthand.
      log and logf say what is going past and pass every value on unchanged,
      so a stage may be dropped into a pipeline and taken out again without
      touching what comes out of it. `| log` with nothing written logs the
      value itself; otherwise `it` is the value in hand, as in a `map`.
      then SOURCE reads this stream and then that one, in that order --
      `history | then asked` is how a conversation keeps its turns straight,
      which two writers to one node cannot.
      sum/min/max/avg read the whole stream and give one value; with an
      expression they read one field of each (`| sum it.price`). min/max/avg of
      an *empty* stream give nothing, because the smallest of no values is not
      a value; `| sum` of one is 0. fold is the general form:
      `| fold 0 as total, total + it.price` binds `total` to what the last
      value produced and `it` to the value in hand. `+` is arithmetic, not
      concatenation -- `| join` is what puts strings together.
      scan publishes each computed accumulator; fold publishes only the last.
      It produces one value per input while carrying state forward, defining a
      state machine over the stream. `repeat` carries state but reads its stream
      from the start on every pass; `for` walks a stream without carrying state
      between passes.
      `| scan 0 as n, n + 1` numbers a stream, and the start may be a record
      when the state has more than one part:
      `| scan {"in": false} as s, {"in": starts-with(it, "BEGIN") or s.in}`.
      window N is batch's overlapping form: one list of the last N values per
      value, once N have arrived. It is what a question about *neighbours*
      needs — a pattern spanning two lines is invisible to a `batch`, because a
      boundary falls somewhere and half the matches fall on it. It holds N
      values and no more, so a window over an endless stream costs nothing that
      grows, and a stream shorter than N yields nothing.
      sort reads the whole stream (nothing comes out until it ends), compares
      the way `<` does, and is stable: values that tie stay in the order they
      were written. `by` names what to compare and `desc` reverses it.
      flatten is the inverse of batch: a stream of lists becomes a stream of
      what they held. A value that is not a list goes through as itself.
      timeout 30s fails the flow when the *gap* between two values exceeds it,
      which is what a stalled producer looks like; a whole-step budget is
      `wait ... timeout` instead. pace 100ms spaces values out and drops
      nothing -- the producer is held behind the buffer.
      group EXPR gathers values into a list and closes it when EXPR holds of
      the one just added — `| group ends-with(it, [".", "?"]) | map join(it)`
      is how partial pieces become whole sentences. packb writes a value as
      application/x-msgpack.
TYPES: string text number integer bool duration time object json list bytes any,
      a shape this file declares, a quoted mimetype, or a registry tag written
      unquoted: a11.sdk.AudioBuffer.
      A container says what it holds: list[string], list[a11.NodeFragment],
      and `T[]` is the same thing as `list[T]`.
      A value is made into one with TYPE{field: expr, ...} or EXPR as TYPE --
      partial in, valid value of that type out. Not `TYPE{` where a `{` would
      open a block (an if/for header); put it in brackets there.
EXPR: literals, it (the value a where/map sees), .field, [i], (pipe | count),
      == != < <= > >= in, and/or/not, + and - (numbers, durations, instants;
      `-` needs its spaces, since `text-upper` is one name), and the functions
      len lower upper trim text number bool keys values get join split merge
      contains starts-with ends-with replace slice default to_chunk from_chunk
      strformat b64encode b64decode b64urlencode b64urldecode
      now duration time seconds
      A list or object literal may spread another in: [...xs, y] and
      {...it, "tags": [..]}, where a later key wins.
      b64encode/b64urlencode give text and b64decode/b64urldecode give bytes;
      the url pair uses the web-safe alphabet and does not insist on padding.
      starts-with/ends-with take one ending or a list of them; to_chunk(v[,
      mime]) makes a Chunk, from_chunk(c) reads one back.
      strformat("%s of %s", a, b) is printf: %s as text, %d %f %x as numbers,
      printf's flags/width/precision (%-8s, %06.2f), %2$s to pick a value by
      number, %% for a literal percent, and %(SPEC)s to apply a duration unit
      or a strftime pattern first. Not a Python template: there is nothing for
      a slot to read into, which is what makes one safe to accept from a model.
TIME: durations are written 500ns, 250ms, 30s, 2m, 1h, compound as 1m30s500ms,
      and are values like any other. now() is the clock; instant - instant is a
      duration; instant +/- duration is an instant; duration +/- duration is a
      duration, and a bare number on either side counts as seconds. A duration
      the other way round is below zero and says so.
      duration(x) and time(x) read a value in: a number of seconds, or the text
      the language writes: duration("1m30s") or
      time("2026-08-11T09:14:22Z")
      — so a duration or an instant that arrived as a string is a value again.
      seconds(d) is the number of seconds.
      Formatting: %s gives `1m30s` and `2026-08-11T09:14:22Z`,
      %(ns)d %(us)d %(ms)d %(s)d %(m)d %(h)d give a duration as one unit, and
      %(%H:%M:%S)s or %(epoch)d formats an instant.
STATUS: a record {"ok": bool, "code": "NOT_FOUND", "number": 5, "message": str}.
      `try` keeps a failure from ending the flow; `wait`/`status` say what
      happened; `fail CODE MSG`, `fail NUMBER MSG` or `fail STATUS` ends it.
      Codes are Abseil's: ok cancelled unknown invalid_argument not_found
      deadline_exceeded already_exists permission_denied resource_exhausted
      failed_precondition aborted out_of_range unimplemented internal
      unavailable data_loss unauthenticated.

Steps run concurrently; order comes from the data. Every output of a step is
read, whether the flow uses it or not. Stages that shrink a stream (first,
truncate, where) do so before the next step ever sees it, `skip N PORT` does it
for every reader at once, `-> _` performs a pipeline and keeps none of it, and
`nodes` blocks keep a step's traffic off the wire.
'''

#: The conventional extension for a file of flows.
EXTENSION = ".flow"


def loads(source: str, source_name: str = "") -> Program:
    """Compile Flow source into a [Program][a11.flow.plan.Program].

    Args:
        source: The text of one or more ``flow`` declarations.
        source_name: A name for error messages, usually a file path.

    Raises:
        FlowSyntaxError: On any problem, with the line and column it was at.
    """
    return compile_source(source, source_name)


def load(path: str | os.PathLike[str]) -> Program:
    """Compile a ``.flow`` file."""
    with open(path, encoding="utf-8") as handle:
        return compile_source(handle.read(), str(path))


def register(source: str, registry: Any, source_name: str = "") -> Program:
    """Compile Flow source and register every flow in it as an action.

    The one call a service needs to accept a composition from outside and
    make it runnable: after this the flows are in the registry, and a session
    dispatches them like any other action.
    """
    return loads(source, source_name).register_all(registry)


def run_program(
    source: str,
    source_name: str = "",
    *,
    arguments: Sequence[str] | None = None,
    roots: Sequence[str] | None = None,
    allow_write: bool = False,
    allow_run: bool = False,
    allow_net: bool = False,
    allow_local_net: bool = False,
    allow_env: Sequence[str] | None = None,
    unrestricted: bool = False,
    timeout_seconds: float | None = None,
    standard_streams: bool = True,
    registry: Any = None,
    session: Any = None,
    dispatch_stream: Any = None,
) -> dict[str, Any]:
    """Run a Flow program's entry flow — the `flow { ... }` with no name.

    This calls the `a11-flow-run` interpreter in process. Pass a registry to
    expose actions from the host process:

    ```python
    from a11 import flow
    from a11.actions import ActionRegistry
    from a11.sdk import llm_tools

    registry = ActionRegistry()
    llm_tools.register(registry)
    flow.run_program(
        source,
        "summarise.flow",
        registry=registry,
        arguments=["summarise.flow", "notes.txt"],
    )
    ```

    A name already in `registry` is never replaced by the standard library's: a
    host that registered its own `read_file` meant its own `read_file`.

    !!! important "Run off-loop for async action handlers"

        This runs the program to completion, so it blocks the thread it is
        called on. A Python action handler written `async def` has to be driven
        by an asyncio loop — and if that loop is on *this* thread, it cannot run
        while this call is blocking it, and the program waits forever on its own
        handler. So call it in a thread:

        ```python
        outcome = await asyncio.to_thread(
            flow.run_program, source, "program.flow", registry=registry
        )
        ```

        A program that only uses the standard library needs none of this: those
        actions are native and take no GIL.

    Args:
        source: The program's text.
        source_name: What diagnostics should call it, usually the path.
        arguments: The program's `argv`. By convention `arguments[0]` is the
            file, as a C program's is, so what a user passed starts at index 1.
        roots: Directories the program may reach. Defaults to the working
            directory.
        allow_write: Whether it may write, inside those roots.
        allow_run: Whether it may run programs, confined by the kernel where
            the platform can.
        allow_net: Whether it may reach the network. Loopback, private and
            link-local addresses stay refused unless `allow_local_net`.
        allow_local_net: Also allow loopback, private, and link-local
            addresses (e.g. cloud metadata services).
        allow_env: Environment variables it may read.
        unrestricted: No filesystem sandbox. Use only with trusted source.
        timeout_seconds: A bound on the whole run, applied as the deadline
            header every standard-library action honours.
        standard_streams: Whether to bind this process's stdin/stdout/stderr.
            Clear it in a host with no useful standard input. A read then fails
            instead of waiting indefinitely.
        registry: An [ActionRegistry][a11.actions.ActionRegistry] whose actions
            the program may call. One is made when omitted.

    Returns:
        `{"exit_code": int, "diagnostics": [...]}` — the code the program put on
        an `out exit_code: integer` port (or 0), and whatever the compiler said
        that was not an error.

    Raises:
        Exception: When the source will not compile, declares no entry flow, or
            the program itself failed. What the flow failed with is the message.
    """
    return _flow.run_program(
        source,
        source_name,
        arguments=list(arguments or ()),
        roots=list(roots or ()),
        allow_write=allow_write,
        allow_run=allow_run,
        allow_net=allow_net,
        allow_local_net=allow_local_net,
        allow_env=list(allow_env or ()),
        unrestricted=unrestricted,
        timeout_seconds=timeout_seconds,
        standard_streams=standard_streams,
        registry=registry,
        session=session,
        dispatch_stream=dispatch_stream,
    )


def check_program(source: str, source_name: str = "") -> str:
    """What a program's entry flow is, compiling it and running nothing.

    Raises if the source will not compile or declares no `flow { ... }`.
    """
    return _flow.check_program(source, source_name)


def request(payload: Mapping[str, Any]) -> dict[str, Any]:
    """Ask the language one question about one document.

    The Python adapter over the one service every Flow frontend is a frontend
    of — the same `{"method": .., "source": ..}` request `a11-flow serve` and
    `a11 flow serve` answer, and the same envelope back. A capability added
    there is available here without anything being added here.

    ```python
    from a11 import flow

    problems = flow.request({"method": "check", "source": "flow t { }"})
    schema = flow.request(
        {"method": "schema", "source": src, "struct": "Source"}
    )
    ```
    """
    return _flow.request(dict(payload))


__all__ = [
    "BUILTINS",
    "EXTENSION",
    "FAIL_CODES",
    "FlowPlan",
    "FlowSyntaxError",
    "Program",
    "REFERENCE",
    "STAGES",
    "TYPE_NAMES",
    "compile_source",
    "invoke",
    "load",
    "loads",
    "register",
    "check_program",
    "request",
    "run_program",
]
