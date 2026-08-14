# Checking flows from a toolchain

A flow is text, which means the things you want to do to text apply to it: check
it in CI, annotate a pull request with what is wrong, highlight it in an editor,
format it before committing. `a11 flow` is the one command all of that goes
through, and its machine-readable output is a documented contract — so a new
editor or a new CI step is a consumer of a format, not another implementation of
the language.

```sh
a11 flow check my.flow                       # for a person
a11 flow check my.flow --format json         # for a tool
a11 flow check my.flow --format sarif        # for a code scanner
a11 flow fmt my.flow -i                      # format it in place
a11 flow fmt --check my.flow                 # for CI, or a pre-commit hook
a11 flow parse my.flow --format json         # the syntax tree, and what broke
a11 flow describe my.flow --format json      # the resolved plan
a11 flow highlight my.flow --format json     # what each token means
a11 flow complete my.flow --line 7 --column 12   # what may be written there
a11 flow run my.flow --input question=why    # run it here, print its ports
a11 flow codes                               # every code, and what it means
a11 flow syntax --generate                   # write the editor definitions
a11 flow serve                               # answer requests on stdin
```

`check` exits `0` when nothing is an error, `1` when something is, and `2` when a
file could not be read — so it drops into a pre-commit hook or a CI job with no
wrapper. A file named `-` reads standard input.

## `a11-flow`, the standalone tool

Everything above is also a small native binary, `a11-flow`, built by
`cmake --build . --target a11_flow_tool`. It is the same library behind the same
commands, and the point of it is what it does **not** link: no Python, no OpenSSL,
no libuv, no audio. An editor extension can bundle it per platform, and a CI image
can carry it without carrying A11.

```sh
a11-flow check my.flow --format json
a11-flow fmt --check *.flow
a11-flow serve --protocol lsp     # a language server, over stdio
a11-flow serve --protocol json    # one request per line, one answer per line
```

`serve --protocol lsp` speaks the Language Server Protocol: diagnostics with quick
fixes built from each diagnostic's own edits, semantic tokens, formatting,
completion, hover, document symbols and go-to-declaration. That is a VSCode or
Neovim integration with no language knowledge of its own — a client, a few
hundred lines at most. It also takes one notification of its own,
`a11flow/setContext`, which is how a client that knows what actions and types
are available says so for the session.

`serve --protocol json` is the same capabilities with no framing to implement,
which is what a host with a pipe and no LSP client wants:

```
{"id": 1, "method": "check", "source": "flow t { }"}
{"id": 1, "ok": true, "result": {"format": "flow.diagnostics/v1", ...}}
```

The methods are `check`, `tokens`, `parse`, `plan`, `format`, `complete`,
`describe`, `symbols`, `definition`, `catalogue`, `schema`, `shapes`, `codes`,
`vocabulary` and `syntax`; each answers with the envelope of the same name below.
Every one of them accepts a `context`, which is what the language knows of the
world outside the document — see `flow.catalogue/v1`.
`a11 flow serve` speaks the identical protocol through the Python bindings, for a
host that already has A11 installed.

### Which units the offsets are in

Every offset in the formats below is a **byte** offset by default, because that is
what the language reads and what an edit is applied in. Most editor hosts do not
count that way: a JVM `CharSequence` and a JavaScript string are indexed in UTF-16
code units, so `§` is one unit and two bytes.

For ASCII the two agree, which makes this the easiest thing in the whole protocol
to get wrong — it works on every example file and then colours the first document
with prose in it a column to the left of itself. So a client says which arithmetic
it counts in, once per request:

```
{"method": "tokens", "source": "…", "offsets": "utf16"}
```

`"bytes"` (the default) or `"utf16"`. It applies to the whole exchange: every
offset in the answer comes back in those units, and `complete`'s inbound `offset`
is read in them too. The conversion is the language's — a frontend that did it
itself would be re-deriving something the service already knows how to do, and
would be the second implementation of it.

Two fields are deliberately *not* offsets into the document and never converted: a
diagnostic's `line` and `column` (1-based, and a column counts code points), and a
completion proposal's `caret`, which counts into the text that proposal inserts.

## The formats

Every envelope carries a `format` field naming it and its version. **Adding a
field is not a version change**: a reader must ignore fields it does not know, and
a version is bumped only when a field changes meaning or disappears.

### `flow.diagnostics/v1`

```json
{
  "format": "flow.diagnostics/v1",
  "source": "research.flow",
  "diagnostics": [
    {
      "code": "flow.unused.try-status",
      "severity": "weak-warning",
      "family": "unused",
      "message": "'try' lets web-fetch fail without ending the flow, and nothing here reads its status: …",
      "flow": "resilient-read",
      "range": {
        "start": {"offset": 412, "line": 19, "column": 11},
        "end":   {"offset": 415, "line": 19, "column": 14}
      },
      "fixes": [
        {"label": "Remove 'try'", "edits": [{"start": 412, "end": 416, "text": ""}]}
      ]
    }
  ],
  "counts": {"error": 0, "warning": 0, "weak-warning": 1, "information": 0}
}
```

A few decisions worth knowing, because they are what make the format usable
without the source file in hand:

* **A range carries both.** Byte offsets are what an editor edits with; lines and
  columns are what a person and a build log read. Computing one from the other
  needs the text, which a diagnostic that has travelled as JSON no longer has, so
  both travel. Lines and columns are 1-based; the range is half-open.
* **A fix is a set of edits, not advice.** A frontend applies them blind and never
  re-derives what the fix should have been. Fixes are offered only where exactly
  one edit is obviously right, so there is never a choice to make.
* **`counts` is there so a gate needs no walk** of the list: fail the build on
  `counts.error > 0`, or on warnings too if you like.
* **`flow` is absent, not empty,** when the text did not get far enough to name
  one.

### `flow.codes/v1`

The published table of every code the language can produce, its family, its
default severity and one line on what it means. A toolchain may match on a code:
codes are stable, and the wording of a message is not. The table is generated from
the C++ source of truth into `testdata/flow/codes.json`, and every language reads
it from there rather than keeping a list of its own.

```sh
a11 flow codes --format json | jq '.codes[] | select(.family == "unused")'
```

### `flow.tokens/v1`

What `a11 flow highlight --format json` gives: one entry per token with the
*meaning* of the word at that position, which is the judgement a syntax
highlighter is making.

```json
{
  "format": "flow.tokens/v1",
  "source": "research.flow",
  "tokens": [
    {"kind": "declaration-keyword", "lexical": "word",
     "start": 154, "end": 158, "line": 4, "column": 1},
    {"kind": "flow-name", "lexical": "word",
     "start": 159, "end": 167, "line": 4, "column": 6}
  ],
  "diagnostics": []
}
```

The kinds are the distinctions a reader makes rather than the ones a parser makes:
`stage`, `builtin`, `type`, `status-code`, `member`, `action-name`, `node-map-name`,
`flow-name`, `declaration-keyword`, `statement-keyword`, `modifier-keyword`,
`constant`, `word-operator`, `port-name`, `identifier`, `comment`, `string`,
`number`, `duration`, `flow-operator`, `operator`, `brace`, `parenthesis`,
`bracket`, `punctuation`, `bad`. An editor maps them to its own palette and needs
no lexer of its own — the same call decides that a word after a `|` is a stage,
that one past a port's `:` is a type, and that `join` is a function only where it
is called.

`port-name` is the one that needs **name resolution** rather than the token
stream: whether `sources` is a port of the flow or a node of its own cannot be
told by looking at neighbouring words, and it is worth telling because a port is
the flow's interface and a node is local plumbing. Every other kind is decided
lexically, which is what lets an editor's lexer run on every keystroke; this one
is a second pass applied on top. The IntelliJ plugin renders it italic in
whatever colour identifiers already are — a slant rather than a hue, because a
port is not a different *kind* of name, just one that crosses the boundary.

`lexical` is the lexer's own name for the token (`word`, `->`, `{`), beside what it
*means*. A client that only colours wants `kind`; one that has to drive a lexer of
its own — an IDE that insists on tokenising every character, and matches braces by
token type — wants both, and one call gives it both.

Tokens tile the source: every offset in the file is covered by exactly one of
them, comments included, so a client can colour a whole file from one response.
Columns count characters, not bytes.

### `flow.syntax/v1`

What `a11 flow parse --format json` gives: the flows a file declares, as the tree
the parser read, **and** everything wrong with it. Both, always — the parser
recovers rather than stopping at the first problem, so a file somebody is in the
middle of typing still has a tree to highlight, format and check.

```json
{
  "format": "flow.syntax/v1",
  "source": "research.flow",
  "flows": [
    {
      "kind": "flow",
      "at": {"start": 237, "end": 241, "line": 4, "column": 1},
      "name": "research",
      "description": "Search, read and answer.",
      "ports": [
        {
          "kind": "port",
          "at": {"start": 300, "end": 302, "line": 7, "column": 3},
          "name": "question",
          "direction": "inputs",
          "type": {"name": "string", "parameters": [], "quoted": false},
          "unary": true,
          "required": true,
          "description": "What to find out."
        }
      ],
      "headers": [],
      "body": []
    }
  ],
  "diagnostics": []
}
```

* **`kind` says what a node is**, in kebab case, and the rest of the object is
  what that kind holds: `pipe` has a `pipeline` and `targets`, `call` has an
  `action`, `args` and `modifiers`, `for-each` has a `variable` and a `body`.
* **`at` is where the node started** — the token it began at, not the extent of the
  whole construct. It is nested under its own key rather than sitting beside the
  node's fields because a `repeat` has a `start` of its own, and a format where one
  key means two things is a format somebody reads wrong exactly once.
* **A duration is `{"$duration": seconds}`.** `250ms` and `0.25` are different
  things, and a reader should not have to guess which one a bare number was.
* **A statement that could not be read is an `error` node** with what was expected
  there, so a subtree that is missing *says* it is missing rather than looking like
  something else.

The format is pinned by `testdata/flow/example.flow` and `testdata/flow/syntax.json`
— one small flow using nearly every construct, and the tree it produces.

### `flow.format/v1`

What `a11 flow fmt --format json` gives:

```json
{
  "format": "flow.format/v1",
  "source": "my.flow",
  "formatted": "flow shout {\n  in  words: string stream\n}\n",
  "changed": true,
  "edits": [{"start": 12, "end": 40, "text": "  in  words: string stream\n"}],
  "diagnostics": []
}
```

`edits` is one edit, trimmed to the part of the file that actually differs, so an
editor applying it does not move the cursor or lose a fold over a file that only
changed at the bottom. A file with an **error** in it is returned exactly as it
was, `changed` is false, and `diagnostics` says why: half-formatting a file
somebody is in the middle of typing is how a formatter loses their work.

#### What the formatter decides, and what it does not

It decides indentation (two spaces a level), the spaces between tokens, how far a
continued line is indented, how many blank lines are allowed and where, the columns
of a run of `in`/`out` or `header` declarations, trailing whitespace, and the
newline at the end of the file.

It does **not** decide where the lines break. Whether a pipeline is written across
four lines or one, and whether a list literal is split a value per line, is a
judgement about what belongs together — which values go with which, which stage is
the interesting one — and it stays the author's. A break that is written is kept and
indented properly; a break that is not written is not invented.

Two invariants, tested over every flow in the repository:

* **Idempotent.** Formatting formatted text changes nothing.
* **The program does not change.** The formatted text lexes to the same tokens
  (line breaks aside, since a block body has to go on its own line) and parses to
  the same tree. Whitespace is all it may touch, and the test says so rather than
  the documentation promising it.

### `flow.plan/v1`

What `a11 flow describe --format json` gives: the resolved plan — each flow's
ports with their types, its headers, its node maps, and the steps the runtime will
run. This is what a caller sees of a flow, so it is also what you diff when you
want to know whether a change to a flow changed its interface.

### `flow.completions/v1`

What `a11 flow complete` and the `complete` method give: what may be written at one
offset, in the order it should be offered, with the partial word at the caret and
where it starts.

```json
{
  "format": "flow.completions/v1",
  "prefix": "trun",
  "prefix_start": 42,
  "proposals": [
    {"name": "truncate", "kind": "stage", "insert": "truncate", "tail": " n"},
    {"name": "question", "kind": "port", "insert": "question: ",
     "tail": " (required)", "type": "string"}
  ]
}
```

`insert` is what taking it writes, which is not always the name: a function takes
its parentheses (`len()`, with `caret` at 4), and an argument takes the colon that
has to follow it. The list is **unfiltered** — every frontend filters and sorts by
its own rules, and one that filtered twice would drop what a fuzzy matcher would
have kept.

After a `|` only a stage is offered; past a port's `:` only a type; after `x.` only
what `x` actually has; after a `->` only somewhere writable. Those are facts about
the grammar and the names in scope, and they are decided in one place.

### `flow.hover/v1`

What the `describe` method gives: what is at one offset, and where it came from.

```json
{
  "format": "flow.hover/v1",
  "found": true,
  "text": "make_http_request",
  "kind": "external",
  "summary": "`make_http_request` — an action",
  "detail": "Make one HTTP request, with every part of the response …",
  "markdown": "…\n\n**Inputs**\n\n- `url`: str *(required)* …",
  "range": {"start": {…}, "end": {…}},
  "definition": {"start": {…}, "end": {…}}
}
```

`markdown` is the whole thing an editor shows; `summary` is the one line a
status bar wants. `definition` is there only when the thing was declared in
*this* document — an action or a registry type has nowhere in the file to go.

Deciding that the word under the caret is a port and not a stage is name
resolution, so it happens in the language rather than in each editor. This used
to live in the LSP adapter, which is how things end up in adapters: it was small
when it was written.

### `flow.symbols/v1`

What the `symbols` method gives: what a document declares, nested as it is
written — the shapes and the flows at the top, a shape's fields and a flow's
ports, node maps and bound steps under them. That is a "go to symbol" list and
an outline both. `range` is the whole construct and `selection` is the name, so
"select symbol" takes the block and "go to symbol" puts the caret on the word.

### `flow.definition/v1`

Where the name at one offset was bound: `{"found": true, "range": {…}, "name":
…, "kind": …}`, or `{"found": false}` for a word that is not a name of this
document.

### `flow.schema/v1`

Both directions of the shape/schema translation. `schema` takes a document and a
`struct` and gives the JSON Schema (draft 2020-12) it describes; `shapes` takes a
schema and gives back **Flow source** — text that can be pasted into a file,
read and checked in.

Neither direction is the real one: a shape is what the language reads and a
schema is what the world outside it reads. The three types JSON has no word for
— `bytes`, `time`, `duration` — go out as strings with the encoding or format
that says how to read them *and* an `x-a11-type` beside it, which is what makes
coming back lossless. Field order travels in `x-a11-order`, because a JSON
object's keys have none and a shape's fields do.

### `flow.catalogue/v1`

What the tools know about the world the language runs in: the actions that may
be called and the types that may be named, each with its description and its
ports or fields.

The language links nothing but Abseil and nlohmann, so it cannot import a
registry — what the world contains reaches it as *data*. A snapshot generated
from the live registries
(`scripts/generate_flow_catalogue.py` → `testdata/flow/catalogue.json`) is
embedded, so the standalone tool completes `make_http_request`'s ports with
nothing configured. A frontend that has a live registry sends its own:

```json
{"method": "complete", "source": "…", "offset": 42,
 "context": {"actions": [...], "types": [...], "replace": false}}
```

which is merged over the snapshot — or replaces it, with `"replace": true`, for
a host that knows exactly which registry an inline flow is attached to. Over
LSP the same thing is said once per session with the `a11flow/setContext`
notification, rather than on every keystroke.

### `flow.vocabulary/v1`

Every word set the language gives meaning to: the stages and what each takes, the
functions, the types, the statements, the modifiers, the status codes, the duration
units. What something generating a static grammar file reads instead of keeping a
list of its own.

### SARIF

`--format sarif` writes a SARIF 2.1.0 log, which is what GitHub code scanning,
Azure DevOps and most annotators already read. Every rule the log can reference is
described in it, because the rule list *is* the published code table. SARIF has
three levels, so both shades of "this does nothing" (`weak-warning`,
`information`) become `note`; the `severity` in the JSON envelope keeps the
distinction if you want it.

## Severities and families

| Severity | Means |
| --- | --- |
| `error` | The flow does not compile. |
| `warning` | It compiles and does something other than what it says. |
| `weak-warning` | It works, and part of it is doing nothing. |
| `information` | Worth knowing, never worth blocking on. |

The **family** is the grouping a reader thinks in, and an editor turns each one
into a single switchable inspection: `syntax`, `form` (a form the language does not
have, or not there), `name` (unresolvable or used as the wrong thing), `sequence`
(operations that cannot do what they appear to), `barrier` (a wait, ordering or
loop tail that cannot hold), `unused` (a status, wait or declaration nothing uses).
The middle part of every code is its family, so `flow.unused.header` needs no
lookup to place.

## In Python

The same shapes, as values rather than JSON:

```python
from a11.flow import diagnostics

index = diagnostics.LineIndex(source)          # offsets ⇄ line/column
for entry in diagnostics.known_codes():        # the published table
    print(entry.code, entry.severity, entry.summary)
```

The engine itself is on `a11._native.flow`, and every function returns one of the
envelopes above as a dict:

```python
from a11._native import flow

flow.tokenize(source)      # tokens, and anything unreadable
flow.highlight(source)     # flow.tokens/v1
flow.parse(source)         # flow.syntax/v1
flow.check(source)         # flow.diagnostics/v1 — syntax, names and findings
flow.format(source)        # flow.format/v1
flow.complete(source, 42)  # flow.completions/v1
flow.plan(source)          # flow.plan/v1, and the diagnostics with it
flow.codes()               # the published table
flow.vocabulary()          # flow.vocabulary/v1
flow.syntax("sublime")     # a generated editor definition
flow.request({"method": "check", "source": source})   # the service, relayed
```

`diagnostics.Diagnostic.from_payload` turns any diagnostic in one of those back into
an object, which is how the CLI renders text and SARIF from the same values.

## What is native and what is not

The language is C++: the lexer, the highlighter, the parser, the resolver, the
inspector, the formatter, the completion and the runtime. `a11 flow`, `a11-flow`,
the Python API and the IntelliJ plugin are frontends over it, and these formats are
the contract between them.

Nothing about the language is implemented twice. `a11.flow.loads` compiles through
the same parser and resolver `a11 flow check` reads, and hands back the graph the
native runtime walks — so what `a11 flow describe` prints, what `check` refuses and
what `run` executes cannot disagree about what a file means.

## Editors

An editor that can run a process needs no language knowledge at all, and the
IntelliJ plugin is the worked example: it runs one `a11-flow serve --protocol json`
per IDE and its Kotlin side is platform wiring — a replay lexer over the token
stream, an external annotator over the diagnostics, a formatting service, a
completion contributor. It carries no word lists, no parser and no resolver. With
no binary for the platform it colours what it can and says so once.

A definition that *cannot* call out — a static grammar file loaded by a highlighter
— is **generated**:

```sh
a11 flow syntax                        # is the checked-in file current?
a11 flow syntax --generate             # write it
a11 flow syntax --target pygments      # one of them
```

There are two targets. `editors/sublime-text/A11 Flow.sublime-syntax` is a
Sublime/TextMate-family grammar, and `editors/pygments/a11flow_lexer.py` is a
Pygments lexer — the one colouring every flow on these pages, registered by
`doc/hooks/flow_highlighting.py`. Both are written from the language's own tables,
so a word the language gains reaches them by running the generator; the check exits
1 when nobody has, which is what CI gates on.
