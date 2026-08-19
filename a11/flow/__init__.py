r'''Flow: a small language for composing A11 actions.

A *flow* is a composition of existing actions that is itself an action. It
declares ports and headers, calls other actions, pipes their streaming ports
into one another, loops, branches, and hands its own outputs back. Because it
presents an ordinary [ActionSchema][a11.actions.action.ActionSchema], anything
that can dispatch an action can run one without knowing it is a composition.

Flows are text. That is the point: a gateway, a client, or a model can be handed
a composition of actions it has never seen before and run it, with no repository
change and no redeploy.

```
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
| `skip source` | read a stream to its end and discard the values |
| `skip n port` | drop a node's first `n` values, for every reader |
| `s = wait x` | hold until `x` is finished, and say how it went |
| `drain node` | end a node: its writers are done and its bytes landed |
| `status x` | the same outcome, read where a value is expected |
| `for v in source [parallel n] { }` | run a block per value |
| `repeat s = start [max n] { }` | run a block repeatedly, carrying a value; say when it stops |
| `s <- source`, `until e` | what a `repeat` carries, and when it stops |
| `if e { } else { }` | run one block or the other |
| `s = try { }` | run a block as one step, and say how it went |
| `cancel x` | ask a called action to stop |
| `fail [code] [message]` | end the flow with a status: in an `if`, or with an `after` |

Every significant word may be written in lower case or upper case — `for` or
`FOR`, `stream` or `STREAM`, `not_found` or `NOT_FOUND`. Mixed case is not a
keyword, so `For` is a name; the rule is easy to state and easy to see.

**Steps run concurrently.** A flow is dataflow, not a script: statement order is
just reading order. A call is dispatched at once and its inputs stream in while
it works, which is what makes a composition of streaming actions worth writing.
Where order genuinely matters, say so with `after`, `wait` or `drain`.

## Why the language has the shape it does

Three of A11's abilities are hard to use well from glue code, and each has
first-class syntax here:

* **`run` and `call`, which are two different things.** `run some-action(...)`
  executes the handler registered where the flow is running; `call
  some-action(...)` puts the action on the stream the flow is attached to and
  lets the peer do it. That is A11's own distinction — `Action::Run` against
  `Action::Call` — and the flow says which it means rather than letting the
  contents of a registry decide. A composition written against a gateway's
  actions `call`s them; one composing actions of its own `run`s them; a client
  flow doing retrieval here and inference there does both, in the same flow.
  `run` needs a handler and says so if there is none, instead of quietly going
  to the session.
* **`skip`, and stages that cut a stream down.** `skip x.debug` reads an output
  and keeps nothing, which is how an output nobody wants stops stalling the
  action producing it. `skip 1 x.rows` is the other half: it takes the first
  value off the node itself, for *every* reader of it, which is how a header
  line stops being everybody's problem — `| drop 1` only trims the one reader
  that says it. Several of them naming the same node add up. `| first 3`,
  `| truncate 4000`, `| where it.ok` and
  `| mime "text/*"` throw values away *before* they reach the next step --
  which, when the next step is a model, is the difference between a cheap call
  and an expensive one. `| packb` is the other side of the same coin: it says a
  value travels as MessagePack rather than JSON, and costs nothing when the
  producer already wrote it that way.
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
  microphone as soon as there is a sentence, with no barrier to name.
* **Nodes of a flow's own.** `x = node()` makes a stream the flow can write
  from several places and read back from one; `x = node(where-they-said)`
  attaches
  to a node somebody else named, and `x.id` hands a node to an action that
  expects to be told where to write. A node lands in the active node map,
  which is what keeps it off the wire.
* **Headers, which are how a call is told *about* itself.** A11 gives a nested
  action every `x-a11-` header of its parent already, so a model or a deadline
  reaches a step without the flow saying anything. For the rest --
  an `authorization`, a tenant id — `forward headers "authorization"` sends on
  what the flow was called with, as it arrived, and `"x-tenant-*"` sends on a
  family. `with "header": expr` is the other half, for a value the flow
  *computes*; naming both, the `with` wins, because it is the more specific of
  the two.

## Failures a flow expects

Not every failure should end a composition. `try` says so — on either verb --
and from there the flow is in charge:

```
page = try run web-fetch(url: url)
outcome = wait page                       # waits, and says how it went
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
registered for them. That is what lets a composition be *factored*: the part
worth reusing becomes a flow of its own, the part that reads badly inline becomes
another, and the whole thing still arrives as one text with one entry point.
Ports are checked between them at compile time, exactly as they are against a
registered action, so a rename that breaks a caller says so before anything runs.

```
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
            | pipeline "->" reference ("," reference)*
            | "skip" (number reference | skip-target ("," skip-target)*)
skip-target := pipeline
            | name ("," name)* "of" name   # or "(" name ("," name)* [ "of" name ] ")"
            | "cancel" name
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
stage      := "first" n | "drop" n | "truncate" n | "batch" n | "group" expr
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

MATCHING: `match` pulls named fields out of text, as a stage over a stream and
      as a function over one value. Literal text matches itself, a run of spaces
      or tabs matches any run, and `{name}` captures up to whatever follows it:
      `lines | match "name={name} age={age:int}"` turns `name=Alice   age=27`
      into a record with `name` and `age`. A hole may say what to read it as:
      `int`, `number`, `bool`, `word`, `line`, `rest`, `duration`, `time`,
      `json`; `{}` captures without a name and is read as `it[0]`. `{{` and `}}`
      are literal braces. The pattern *searches*, so it matches anywhere in the
      value and needs no leading or trailing wildcards, and a hole stays on its
      line unless its type says otherwise. The stage **drops** a value the
      pattern does not fit, so it is a `where` and a `map` at once; the function
      answers **null**, which `if not obj` asks about. Where the pattern is
      written out rather than computed, the fields it names are known, so
      `it.name` is completed and a typo in it is reported. A pattern that cannot
      be read at all is refused where it is written, because it is a literal
      almost every time and a silent no-match would hide the typo.

FIELDS: two things say what a value holds, and reading a field it does not have is
      reported for both: a port declared with a `struct`, and a `match` pattern,
      whose holes *are* its fields. Where the file never said -- a port carrying
      `object` or `json`, `it` with no pattern behind it, a positional pattern --
      nothing is checked, because a value that may hold anything does. One level
      is checked: a field holding a record of its own says nothing about *its*
      keys, so `src.meta.title` checks `meta` and stops.

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

## Prose

A description is prose, and prose does not fit on the line of the declaration it
belongs to. Two spellings deal with that, and they compose:

```
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
and the indentation every remaining line shares comes off. So a long description
sits at the indentation of the flow it describes and still reads as prose.
Escapes work as they do in a single-quoted string, and are resolved after the
dedent, so a hand-written `\\n` is a line break and never an indented line.

A description may also stand **alone on the line below** what it describes, at
any indentation or none. That is unambiguous because the string has to be alone:
`"a literal" -> out` is a statement, since something follows the string, and a
line holding nothing but a string is not a statement in this language.

A type is also something a value can be *made into*, which is how a flow feeds a
port that wants a real type rather than a bag of keys:

```
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
from collections.abc import Mapping
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

#: A compact description of the language, for putting in front of a model that
#: has to write one. Short on purpose: a flow is meant to be writable from the
#: shape of the actions available, not from a manual.
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
  skip SOURCE[, SOURCE...]                 # read to the end, keep nothing
  skip N PORT                              # drop its first N, for all readers
  skip X                                   # every output of a call X
  skip O[, O...] of X                      # just those outputs of X
           # (also written `skip (O, O...) of X` or `skip (O, O... of X)`)
  S = wait SUBJECT [timeout 30s]           # finished; S is how it went
  S = drain NODE                           # end a node, and say how it ended
  cancel X                                 # ask X to stop
  fail [CODE] [MESSAGE]                    # end the flow with a status
  log [LEVEL] WHAT                         # write to the flow's own log
  logf [LEVEL] "fmt" [ARG, ...]            # the same, formatted
           # `fail`, `cancel` and `log` wait for nothing, so they go in an
           # `if` or a loop body, or carry an `after`: at the top of a body
           # they race every other statement, and are refused there.
           # No port declares the log, nothing drains it, and a flow that
           # never logs pays nothing for it
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

A description may be a "..." string, a """...""" one that holds line breaks and
gives back the indentation the source put in front of it, or either of those
alone on the line below what it describes, at any indentation. A string with
anything after it on its line is a value, as it always was. Strings written next
to each other are one string, so prose that outgrows its line does not need a
`+`, and `\\"` is a quote inside one. A *keyword's* quoted argument — a
`matching`, a `strformat` — is one literal, since a run there could not be told
from the argument followed by a description.

A `struct` declares a shape: a record with named, typed, constrained fields, which
a port may be typed with and a value may be made into. A shape a file declares
outranks a serialisation tag of the same name — what the file says about the
name is what the file means by it — and it may hold, and be held by, another
shape. `A..B` bounds a number, a duration or an instant, and the *length* of a
string, a byte string or a list; either end may be left off (`1..`, `..200`).
A shape holding `bytes` anywhere in it cannot go through `| json`, which has
nothing to carry them in; `| packb` can.

ONE VALUE: everything here is a stream, which is the right default for dataflow
      — but some of what moves through a flow is one value, and `let` gives it
      a name. `let code = http.status_code` reads one value of that stream and
      binds it, and the name then stands *where an expression does*:
      `if code >= 200 and code < 300 { .. }`, `strformat("%d", code)`,
      `code == other`. It is also a stream of one wherever a SOURCE goes, which
      is the other direction: `let image = page.body` then
      `image | chunk 65536 -> upload.parts` cuts that one value into 64 KiB
      pieces. A `let` is lazy — nothing is read until the name is — so it may
      be written where it reads best rather than where the value is first
      needed, and one nothing reads is reported. An empty stream binds nothing,
      which `if not code` is how to ask about. A value is read, never written.

      Reading a stream where a value belongs *takes* a value off it. Two places
      that read one stream for a value take turns on the one view of it, so they
      see two different values rather than two copies of the first: reading the
      first value and ignoring the rest is exactly the mistake nobody finds out
      about. Which of them gets which value is not defined; `after` is how a flow
      that cares says so. A stream the language can *prove* carries one value is
      the exception, and is shared rather than taken: a port that did not say
      `stream`, a header, a status, or a pipeline that reduced with `| collect`,
      `| count` or `| first 1`. Those promise one value, so a second arriving
      ends the flow with `invalid_argument` rather than passing unnoticed.

      `advance V` rebinds a `let` value to the *next* value of the same stream,
      which is how a flow reads several values of one stream one at a time and
      knows which is which: `let word = words`, use it, `advance word`, use it
      again. The guarantee is positional rather than an ordering — the *k*th
      binding of a name is the *k*th value of its stream however the flow is
      scheduled — so it holds without a barrier. Statements written above an
      `advance` keep the value they were resolved against, which is what makes
      the name read top to bottom. Advancing past the end binds nothing.

      Several names take one value apart: `let name, age = user` by field, and
      `let first, second = pair` by position. They are the same statement, and
      which one is meant is a question about the value rather than about the
      text: each name is looked up as a field, and as a position where there is
      no such field. So `let name, age = match("name={name} age={age:int}", line)`
      reads what a pattern named. A part is not a value of a stream of its own,
      so `advance` on one says so rather than binding the next whole value.

SOURCE is a port (in-port, X.out-port), a node, a loop variable, a `let` value,
a header alias, a literal, `status SUBJECT`, `N.id`, `zip(SOURCE, ...)`, or any
of those with `.field` / `[i]`.
`zip(a, b, ...)` reads several streams in step and gives one stream of tuples,
read as `it[0]`, `it[1]`, or taken apart by `for x, y in zip(a, b)`. A source
that ends *well* contributes a null to every tuple after it, so the longer
stream is still read to its end; one that ends with an *error* ends the whole
iteration with that status. It stops when every source has, and it is a stream
like any other — `wait`, `drain`, `| first n`, `| drop n`, `| count` all work
on one.
DEST is an out-port, X.in-port, or a node.
SUBJECT is a call, a node, a port, or a named wait/drain.
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
STAGES: first N | last N | drop N | truncate N | batch N | chunk N |
        group EXPR | then SOURCE | where EXPR | map EXPR | join "sep" |
        strformat "fmt" | mime "text/*" | collect | count | distinct | text |
        json | packb | log [LEVEL] [EXPR] | logf [LEVEL] "fmt" [ARG, ...]
      chunk N cuts each value into pieces of at most N *bytes* — the sizes
      people write are byte counts, because they are about a frame or a buffer.
      Text stops at a character boundary rather than splitting one. A value
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
      the language itself writes — duration("1m30s"), time("2026-08-11T09:14:22Z")
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
for every reader at once, and `nodes` blocks keep a step's traffic off the wire.
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


def request(payload: Mapping[str, Any]) -> dict[str, Any]:
    """Ask the language one question about one document.

    The Python adapter over the one service every Flow frontend is a frontend
    of — the same `{"method": .., "source": ..}` request `a11-flow serve` and
    `a11 flow serve` answer, and the same envelope back. A capability added
    there is available here without anything being added here.

    ```python
    from a11 import flow

    problems = flow.request({"method": "check", "source": "flow t { }"})
    schema = flow.request({"method": "schema", "source": src, "struct": "Source"})
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
    "request",
]
