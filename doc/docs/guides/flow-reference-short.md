# Flow: compact agent reference

Flow combines streaming tools into one action: filter intermediates before they
reach model context and fix orchestration without per-call model decisions.
Execute almost every tool operation through the host's Flow runner, including
a single read, search, edit, command, or HTTP request. A one-step Flow is the
default for small operations too. Combine related phases in one invocation;
an entry flow can compose sibling flows. Filter and bound outputs. For
surrounding content, use `context` on original file lines rather than manual
ranges. When fresh judgment
is needed, return its evidence and execute the next operation in another Flow.
Use direct calls only for discovery/guides/checks, user interaction, session
controls, completion reporting, tools the runner cannot invoke, or an explicit
user request. Never wrap the Flow runner in itself. Statements overlap; data,
`after`, and `wait all` order them. Arrival order can remain nondeterministic.
Reuse action/port contracts already in context; discover only missing contracts.
Discovery and compile-only checks are optional, not prerequisites to execution.
For ordinary tool chains, write one flow using runtime-provided tools directly.
Avoid modules, module contracts, and imports outside `std` unless building a
reusable library; do not redeclare tool contracts already supplied by the host.

Prefer specialized non-shell actions and Flow stages for file access, search,
HTTP, filtering, and transformations whenever they can solve the task. Reserve
process tools for actual program execution, such as tests and builds, or work
that available actions cannot perform. When a process is needed, prefer a
direct executable with arguments over Bash unless shell syntax is required.

Choose a small output budget before writing each Flow. Set source-side query,
path, item, and byte limits where available. Filter with `where`/`match` and
project needed fields with `map` before returning results. Combine `first N`
to cap returned items with `truncate M` to bound each text value; either alone
can still return too much. Flatten list values before limiting their entries.
Avoid returning whole files, response bodies, objects, or unbounded `collect`
results when selected evidence answers the question. Increase limits only when
the bounded result leaves a concrete gap.

Keep full data needed by downstream actions inside the Flow; truncate the
branch returned to the model. Retain identifiers, errors, and available counts
or truncation indicators so the next Flow can retrieve missing evidence.

For file/tool-output matches (tests, web fetches, external systems), prefer
`context e before N after M` on original lines/items per source; filtering first
loses neighbors. Use search hits to locate source spans. Split unary text with
`std::split(text, "\n") | flatten`. Keep N/M small; `first G` caps groups, then
`map std::join(it, "\n") | truncate C` caps each text excerpt. Successful checks
need only status/counts. Preserve complete causal diagnostics and structured
details; save full logs and expand only for a concrete gap.

Use this reference first. Consult extended guides only when your specific
concern is genuinely not answered here; do not load them as routine background.

For missing details: [guide](flow.md),
[language/API](../api/flow.md),
[checking/tooling](flow-tooling.md).

## Syntax

`e` = expression, `s` = stream, `T` = type; `[ ... ]` in tables means optional.
Examples use separate lines; tables use semicolons only to separate alternatives.
One statement per logical line; incomplete expressions, types, pipelines, and
modifier tails may continue across lines. Braces delimit scopes; `#` comments. Keywords accept
lowercase or UPPERCASE. Functions need `std::` unless imported; stages do not.
Flows join owned work; emitting an output does not finish execution.
Strings and `"""` strings decode `\0` (NUL), `\a`, `\b`, `\f`, `\n`, `\r`,
`\t`, `\v`, `\"`, `\'`, and `\\`. Other escapes yield the following character.

| Form | Meaning |
|---|---|
| `flow name [exported] { ... }` | Define a callable action; `flow { ... }` is a CLI entry with `argv`/`argc`. |
| `in x: T [stream] [required]`; `out y: T [stream]` | Unary unless `stream`; optional means absence, not a default. `required` means present, not nonempty. Port names come from contracts. |
| `describe "..."`; `header "key" as h [default "..."]` | Action description; incoming header binding. |
| `x = e`; `T x = e`; `a, b = e` | Eager initialization once per scope; infer or check/coerce type; destructure RHS once. Initializer required. |
| `x = e` again | Reassign in source order; child updates join back. Reject conflicting parallel updates. |
| `s = source \| stage` | Lazy stream alias: unused pipelines do not execute. Readers share production. Nested scopes replay outer streams; explicit `take` uses a shared cursor. A list is one value; `flatten` expands it. |
| `x = take s`; `x = try take s` | Successive reads: EOF errors / returns `{has_value:false}`. Present items have `value`, including null. Upstream errors propagate. |
| `j = run action(port: s, ...)`; `j = call action(...)` | Local handler / peer dispatch; executes even if unused. Read `j.output`; write `j.input`. Unused outputs drain; unfed inputs close immediately, fed inputs after their last writer. |
| `s \| run f(config: e)`; `s \| call m::f(...)` | Once per whole stream. Exactly one unsupplied input (optional inputs count) and one declared output. Interface required. |
| `s \| map (run f(input: it)).output` | Per-item invocation; distinct from whole-stream application. |
| `a -> b -> out`; `s -> a, b`; `s -> _` | Right-associative: forward all b, then all a. Terminal commas broadcast. Independent attachments to a shared handle interleave after its prefix. Handles are sinks, never payloads or sources. |
| `if e { ... } else if e { ... } else { ... }` | Boolean conditional scopes. |
| `if x = option { ... } else { ... }` | Evaluate once; bind the present payload only in the success branch. Fresh name; no enclosing-name conflicts. Else-if initializers are lazy. |
| `for x[, y...] in s [parallel N] { ... } [until e / while e]` | Per-item scopes, optional tuple destructuring; sequential by default. A bare list literal (`for x in [a,b,c]`) expands one level; empty → no passes. Other sources retain stream values; use `\| flatten` for named lists or list-valued pipelines. Parallel iterations may interleave writes. Trailing condition runs after the complete pass; forbidden with parallel passes. |
| `repeat [state = e] [max N] { ... } [until e / while e]` | Repeat with condition and/or literal bound. `state <- e` supplies next pass's state; ordinary reassignment also works. |
| `... after a, b`; `after a, b { ... }` | Gate activation, including arguments/initializers, on completion. Bound blocks/loops name completion. |
| `wait all` | Scope-local phase barrier: all earlier work completes before later work activates. No binding/modifiers. Keep streaming producers with consumers in the same phase. |
| `wait j`; `wait all of a, b`; `wait first of a, b` | Wait for subjects; the race yields the zero-based winning index. Explicit-subject waits are not scope-wide barriers. |
| `status j`; `outcome = wait j` | Completion status: `.ok`, `.code`, `.message`, `.details`. Unhandled failure ends the scope. |
| `try run/call ...`; `attempt = try { ... }`; `try s -> dest` | Tolerate call / block / transfer failure; inspect its status. No rollback of outputs/effects. Tolerating a call does not suppress aborted-output read errors. |
| `n = node([id])`; `close n`; `abort n [code [message]]` | Create/attach stream node; close normally / with error. `n.id` passes its identity to a tool. |
| `nodes m { ... }`; `nodes m`; `... via m` | Scoped/named node map. Local calls keep intermediates off the wire unless `tee`. |
| `cancel j`; `fail [code [message]]`; `fail status_value` | Cancel an action / fail the scope. Use conditions, data dependencies, or `after` to gate side effects. |
| `skip N s` | Drop the first N items for every reader; `drop N` affects one pipeline. |
| `log [level] e`; `logf [level] "fmt" e, ...` | Log without output ports; default `info`; levels: `debug`, `info`, `warning`, `error`, `critical`. |
| Call tail: `timeout D`, `deadline t`, `after subjects`, `via m`, `tee`, `id e`, `with "k": e`, `forward headers "pattern"` | Positive relative duration / absolute time; earlier limit wins. Deadline is eager; `after` delays its entire expression. Waits also accept timeout/deadline. |
| `mod m [exported] { ... }`; `declare mod m { ... }`; `declare flow m::f { interface }` | Modules/members default private. Contracts describe local, imported, or host-supplied implementations; structs are fully defined. No modules inside flows/structs; `std` reserved. |
| `import "dir/lib.flow" [as m]`; `from lib import f [as g], T`; `from lib import *` | Default namespace `lib`; symbol/wildcard imports also work in module/flow scopes. `from std import f` exposes standard names. Filesystem imports require approved host roots. |
| `export symbol [as alias]`; `struct T [exported] { fields }` | Explicit reexport; record type. Fields: `name: T`, `required` or `default LITERAL` (exclusive), numeric bounds `1..10`, text regex `matching "pattern"`, `one of [...]`, list `unique`. |
| `m::f`, `a11::sdk::Interaction`; `j.output`, `v.field`, `v[index]` | Static qualification / runtime projection. Registered host types need no import; wire tags retain dotted spelling. |
| `T{field:e}`; `e as T`; `[e, ...]`; `{key:e, ...other}` | Typed record, cast, list, object/spread. Strings: `"..."` or triple quotes. Literals: `true`, `false`, `null`. |
| `+`, `-`; `== != < <= > >= in`; `not`, `and`, `or` | Numeric/time arithmetic, comparison/membership, short-circuit logic. Space subtraction: `a-b` is a name. No `*` or `/` arithmetic. |

`parallel` accepts integer expressions such as `parallel workers` or
`parallel (workers + 1)`, evaluated once when the loop or stage starts. Dynamic
counts must be positive signed 32-bit integers. Dynamic loops retain parallel
safety restrictions on outer-variable updates and trailing conditions.

## Types and conversions

Prefer `list[T]` and `list(value)`: `list` is available without an import as an
alias of `std::list`. The qualified spelling remains valid.

| Types | Meaning |
|---|---|
| `int`, `number`, `bool`, `string`, `bytes`, `null` | Primitive prelude; prefer `int` (`integer` is an alias). Same-name conversions are unqualified. |
| `object`, `object[V]` | String-keyed heterogeneous / uniformly typed values; no key-type parameter. |
| `list[T]`, `std::json`, `std::any` | One list value, JSON-compatible data, unrestricted payload. |
| `std::time`, `std::duration` | Instant / interval; timezone-less input is UTC. Durations: `ns us ms s m h`, e.g. `1m30s`; numeric conversions use seconds. |
| `std::status`, `std::option[T]`, `std::stream[T]` | Status data, explicit presence, live stream capability. Prefer optional binding below. Explicit `.has_value` tests presence; absent `.value` access fails. |
| Standard-type conversions `int(v)`, `list(v)`, etc.; `v as T` | Convert/validate; existing permissive coercions may lose precision or map invalid numeric text to zero. Checked parsers below fail instead. |
| `std::text`, `std::boolean`, `std::array[T]` | Aliases of string, bool, list. Exposing a standard type also exposes its supported conversion. |

## Optional reads

Prefer optional binding. Each initializer runs once; the name exists only in its
success branch. Only EOF means absence; errors and cancellation propagate.
Null, false, zero and empty payloads remain present.

```a11flow
flow direct_name {
  in  rows: object stream

  out name: string

  if row = try take rows {
    row.name -> name
  }
}

flow stored_name {
  in  rows: object stream

  out name: string

  candidate = try take rows
  if row = candidate {
    row.name -> name
  }
}
```

A stored option may be reused. Ordinary records require an explicit validated
`std::option(...)` conversion; matching field names alone do not make an option.

Use `T settings = try take options or T{}` for a lazy default. With an option
on the left, `or` unwraps a present payload, including null, false, zero and
empty values. Only absence evaluates the fallback. This also works with stored
`std::option[T]` values. Upstream errors propagate. For ordinary values, `or`
keeps its truthiness-based short-circuit behavior.

## Ordered transfers

Arrows associate right: `later -> earlier -> result` forwards all of `earlier`,
then all of `later`. A terminal comma list broadcasts that ordered sequence.
Ordering controls consumption; separately declared producing actions still
activate concurrently unless gated.

```a11flow
flow ordered {
  in  earlier: string stream
  in  later:   string stream

  out result: string stream

  later -> earlier -> result
}

flow shared_tail {
  in  prefix: string stream
  in  left:   string stream
  in  right:  string stream

  out result: string stream

  tail = prefix -> result
  left -> tail
  right -> tail
  wait tail
}
```

The shared tail emits `prefix`, then interleaves `left` and `right`. Aliases
share the handle; they do not repeat its transfer. `wait tail` includes attached
writers and owned cleanup. Register attachments before a scope barrier: placing
`wait all` between `tail` and its attachments creates a diagnosed cycle.
Handles are destinations, never stream sources or payloads. Broadcast lists
currently accept terminal destinations only; a handle may itself broadcast.

## Functions

All names below use `std::`, except primitive/object conversions above.
Brackets mark optional arguments; `v` is a value, `s` text, `xs` a list.

| Function(s) | Result |
|---|---|
| `len(v)` | Length. |
| `lower(v)`, `upper(v)`, `trim(v)` | Text case / surrounding whitespace. |
| `contains(v,x)`, `startswith(s,p)`, `endswith(s,p)` | Membership / prefix / suffix; `p` may be a list of alternatives. |
| `replace(s,old,new)`, `slice(v,start[,stop])` | Replace all / slice; negative indices supported. |
| `split(s[,sep])`, `join(xs[,sep])` | Split on whitespace runs, omitting empties, by default / join with empty separator by default. |
| `keys(v)`, `values(v)` | Lists in sorted-key order. |
| `get(v,key[,fallback])`, `default(v,fallback)` | Lookup fallback defaults to null; `default` replaces null/empty text/list/object, not 0/false. |
| `merge(a,b,...)` | Merge records; later keys win. |
| `match("{name:word}={n:int}",s)` | Flow capture template (not regex): record or null. Holes also support number, bool, line, rest, duration, time, json. |
| `strformat(fmt,args...)` | printf-style formatting: `%s`, `%d`, `%.2f`, `%2$s`; time `%(%H:%M)s`. |
| `now()`, `seconds(duration)` | Current instant / numeric seconds. |
| `time(v)`, `duration(v)` | Permissive temporal conversion; `parse_time(s)`, `parse_duration(s)` fail on invalid text. |
| `b64encode(v)`, `b64decode(s)`, `b64urlencode(v)`, `b64urldecode(s)` | Base64 / URL-safe encoding and decoding to bytes. |
| `utf8encode(s)`, `utf8decode(bytes)` | Text ↔ UTF-8 bytes. |
| `to_chunk(v[,mimetype])`, `from_chunk(chunk)` | Host-aware encode/decode, preserving payload metadata. |
| `zip(a,b,...)`, `interleave(a,b,...)` | Lockstep tuple stream / arrival-ordered merged stream. |
| `list(v)`, `json(v)`, `any(v)`, `status(v)`, `option(v)` | Standard conversions; a scalar converted to list becomes one item. |

## Stages

Write `s | STAGE`; `it` is the current item. Parallel per-item stages restore
input order unless `unordered`; `sort` reorders deliberately. `first` ends
consumption, not producer ownership/cancellation. `collect`, `sort`, `last`,
and reductions need EOF; bound endless sources upstream. Counts for `first`,
`last`, `drop`, `truncate`, `batch`, `window`, and `chunk` are scalar integer
expressions, evaluated once per activation outside `it`.

| Stage | Effect |
|---|---|
| `map e`; `where e` | Transform / filter each item. |
| `take while e`; `take until e` | Prefix while true / until true; exclude the boundary item. |
| `first N`; `last N`; `drop N`; `distinct` | Prefix / suffix / skip / deduplicate. |
| `truncate N`; `chunk N` | Shorten each value / split text or bytes into ≤N-byte chunks. |
| `batch N`; `window N`; `group e`; `flatten` | Batches / full-width rolling windows / groups closed by e / expand lists. `batch` and `group` flush partial groups at EOF; `window` does not. |
| `context e before N after M [numbered [base B]]` | Excerpts with N preceding items; each match within M trailing items resets the tail. EOF flushes. Counts: nonnegative int32, evaluated once outside `it`. `numbered`: `{index, value}`, counting every source item from B (default 1). Active excerpts can grow; bound endless sources upstream. |
| `sort [by e] [desc]` | Collect and order; ascending by default. |
| `match "pattern"`; `mime "text/*"` | Extract matching records, dropping nonmatches / filter representations. |
| `collect`; `consume`; `count` | One list (empty → `[]`) / require exactly one item and EOF / item count (empty → 0). |
| `sum [e]`; `min [e]`; `max [e]`; `avg [e]` | Aggregate items or projections. Empty: `sum` → 0; `min`/`max`/`avg` → no item (`take` then fails). |
| `fold INITIAL as acc, e`; `scan INITIAL as acc, e` | Final / every accumulator; INITIAL is a literal, including a record. Empty `fold` yields INITIAL. |
| `join ["sep"]`; `text`; `strformat "fmt"` | Stream → one string (default separator empty) / each item → text / format each item. |
| `json`; `packb` | JSON parse/encode / MessagePack representation. |
| `then s` | Entire first stream, then s. |
| `timeout D`; `pace D` | Maximum inactivity gap / minimum inter-item interval. |
| `log [level] e`; `logf [level] "fmt" e,...` | Observe and pass items through. |
| `try STAGE [into failures]` | Per-item failure handling for suitable stages: drop failed item; route status or log. |
| `STAGE parallel N [unordered]` | Parallel per-item transforms/filters; not reductions, sorting, or stateful prefix predicates. |
| `run f(args...)`; `call f(args...)` | Whole-stream action adapter; ordinary call modifiers apply. |

## Examples

These examples collectively pack in language forms to teach breadth; do not
copy their complexity into routine tool chains. When a human will read or
maintain the Flow, favor clear names, descriptions, and logical spacing.
For mere runtime-tool combinations, prefer simplicity and token savings.

Default: combine available runtime tools; return only the small result:

```a11flow
flow extract_errors {
  in  source: string required
  in  target: string required

  out written: int

  file = run read_file(path: source)

  saved = run write_file(
    path: target,
    content: file.lines
      | where std::contains(it, "ERROR")
      | first 100
      | map std::strformat("%s\n", it)
  )

  saved.bytes_written -> written
}
```

Library features only when needed: typed modules, aliases, custom stages,
validation, parallel work, bounded outputs. This example is for a human to
read; replace the description placeholders with useful user-facing text:

```a11flow
from std import trim, len

struct Hit exported {
  name:  string required
  score: int required 0..100
}

mod text exported {
  flow clean exported {
    in  input: string stream

    out output: string stream

    input
      | map trim(it)
      | where len(it) > 0
      -> output
  }
}

export text::clean as clean

flow rank exported {
  describe "<Explain this flow's purpose to the user.>"

  in  lines: string stream "<Describe the expected line format.>"

  out best:  Hit stream "<Describe the ranked results.>"
  out total: int        "<Describe what the total measures.>"

  bad = node()
  hits = lines
    | run clean()
    | match "{name:word}={score:int}"
    | try map Hit{name: it.name, score: it.score} parallel 4 into bad

  hits | sort by it.score desc | first 3 -> best
  hits | sum it.score -> total

  bad | log warning it.message -> _
}
```

Recovery: optional input, remote calls, timing, headers, scoped nodes, status,
reassignment, and a phase barrier. The declaration describes a host-supplied
API; omit it when discovery already supplies that contract:

```a11flow
declare flow fetch_page {
  in  url: string

  out text: string
}

flow fetch_first {
  in  urls:   string stream
  in  budget: std::duration

  out text: string

  header "authorization" as token default ""

  std::duration allowance = try take budget or 30s
  std::time limit = std::now() + allowance

  nodes scratch

  bool found = false
  for url in urls | first 3 {
    page = try call fetch_page(url: url)
        via scratch
        timeout 5s
        deadline limit
        with "authorization": token

    outcome = wait page
    if outcome.ok {
      page.text | truncate 1000 -> text
      found = true
    } else {
      log warning outcome.message
    }
  } until found

  wait all

  if not found {
    fail not_found "no candidate succeeded"
  }
}
```

Check before execution with `a11 flow check FILE`.
