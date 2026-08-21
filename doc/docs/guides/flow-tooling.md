# Checking flows from a toolchain

Use `a11 flow` to check, format, inspect, and run Flow source from CI or an
editor. Its versioned machine-readable formats let integrations consume the
native language tooling directly.

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
a11 flow scan a11 cpp js                     # the actions the project declares
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
a11-flow scan src/                # the actions the project declares, with origins
a11-flow serve --protocol lsp     # a language server, over stdio
a11-flow --stdio                  # the same, spelled the way an LSP client does
a11-flow serve --protocol json    # one request per line, one answer per line
```

`serve --protocol lsp` speaks the Language Server Protocol: diagnostics with quick
fixes built from each diagnostic's own edits, semantic tokens, formatting,
completion, hover, document symbols and go-to-declaration. `--stdio` means the same
thing and is accepted because that is what an LSP client says: `vscode-languageclient`
appends it to a server's arguments on its own, and a tool that refused it would exit
before reading a byte -- which a client reports as a connection that went away
rather than as an unrecognised flag. That is a VSCode or
Neovim integration with no language knowledge of its own — a client, a few
hundred lines at most. It also takes three methods of its own. `a11flow/setContext` is how a client that
knows what actions and types are available says so for the session;
`a11flow/scan` asks it to read a project's own source for the actions *it*
declares and folds the answer into the same place; and `a11flow/relay` carries one
request of the JSON protocol below, for the questions LSP has no method for -- the
one that matters being a flow written inside a string literal, which is not a
document the server has.

`serve --protocol json` is the same capabilities with no framing to implement,
which is what a host with a pipe and no LSP client wants:

```
{"id": 1, "method": "check", "source": "flow t { }"}
{"id": 1, "ok": true, "result": {"format": "flow.diagnostics/v1", ...}}
```

The methods are `check`, `tokens`, `parse`, `plan`, `format`, `complete`,
`describe`, `symbols`, `definition`, `catalogue`, `scan`, `schema`, `shapes`,
`codes`, `vocabulary` and `syntax`; each answers with the envelope of the same name below.
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
{"method": "tokens", "source": "...", "offsets": "utf16"}
```

`"bytes"` (the default) or `"utf16"`. It applies to the whole exchange: every
offset in the answer comes back in those units, and `complete`'s inbound `offset`
is read in them too. The conversion is the language's — a frontend that did it
itself would be re-deriving something the service already knows how to do, and
would be the second implementation of it.

Two fields represent distinct coordinate systems rather than document offsets: a
diagnostic's `line` and `column` (1-based, code-point count), and a completion
proposal's `caret` (offset into the inserted proposal text).

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
      "message": "'try' lets web-fetch fail without ending the flow, and nothing here reads its status: ...",
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

The kinds describe semantic roles used by readers and editors:
`stage`, `builtin`, `type`, `status-code`, `member`, `action-name`, `node-map-name`,
`flow-name`, `declaration-keyword`, `statement-keyword`, `modifier-keyword`,
`constant`, `word-operator`, `port-name`, `identifier`, `comment`, `string`,
`number`, `duration`, `flow-operator`, `operator`, `brace`, `parenthesis`,
`bracket`, `punctuation`, `bad`. An editor maps them to its own palette and needs
no lexer of its own — the same call decides that a word after a `|` is a stage,
that one past a port's `:` is a type, and that `join` is a function only where it
is called.

`port-name` requires name resolution because neighbouring tokens cannot
distinguish a flow port from a local node. Other kinds are lexical, so an editor
can apply them before resolution completes. The IntelliJ plugin renders resolved
ports with the identifier colour and italic emphasis.

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
  "detail": "Make one HTTP request, with every part of the response ...",
  "markdown": "...\n\n**Inputs**\n\n- `url`: str *(required)* ...",
  "range": {"start": {...}, "end": {...}},
  "definition": {"start": {...}, "end": {...}}
}
```

`markdown` is the whole thing an editor shows; `summary` is the one line a
status bar wants. `definition` is there only when the thing was declared in
*this* document.

`origin` is the other half of that, and is there when the thing was declared in
another file that something read — an action a `scan` found. The two are kept
apart rather than folded into one field because they answer differently: a
definition is a range in the document that was passed in, and an origin is a path
a host has to open. A frontend that treated them as one would put the caret at
line 12 of the wrong file.

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

Where the name at one offset was bound: `{"found": true, "range": {...}, "name":
..., "kind": ...}`, or `{"found": false}` for a word that is not a name of this
document.

`found` stays about *this* document. A word that is not a name of it may still
have somewhere to go — an action a `scan` found in a project file — and that
arrives as `origin` beside a `found` of `false`:

```json
{"format": "flow.definition/v1", "found": false, "name": "split_lines",
 "kind": "external",
 "origin": {"file": "a11/demos/split_lines.py", "line": 16, "column": 26}}
```

Over LSP the adapter answers that as a `file://` location, so the two cases are
one gesture to a client.

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
{"method": "complete", "source": "...", "offset": 42,
 "context": {"actions": [...], "types": [...], "replace": false}}
```

which is merged over the snapshot — or replaces it, with `"replace": true`, for
a host that knows exactly which registry an inline flow is attached to. Over
LSP the same thing is said once per session with the `a11flow/setContext`
notification, rather than on every keystroke.

#### Actions a project declares in its own source

The snapshot is what the *SDK* registers. An action somebody wrote this afternoon
is in no snapshot, and that is the common case for anybody composing their own
actions: hovering its name said "action name", completing its ports offered
nothing, and there was nowhere to go.

`scan` reads source for `ActionSchema` declarations and answers a catalogue in
which every entry carries an **origin**:

```sh
a11-flow scan a11 cpp js --format json | jq '.actions[0]'
```

```json
{
  "name": "split_lines",
  "description": "Split one text into its non-empty lines, trimmed, one value per line.",
  "inputs": [{"name": "text", "type": "text/plain", "required": true,
              "description": "The text to split."}],
  "outputs": [{"name": "lines", "type": "text/plain", "required": true}],
  "origin": {"file": "a11/demos/split_lines.py", "line": 16, "column": 26}
}
```

`origin` is 1-based, like a diagnostic's line and column, and the path is the one
the scan was given — absolute if the root was, relative if it was — because the
host has to open the answer in the terms it asked in. It is **absent** for
everything that came from a live registry or from the embedded snapshot: a
registry knows what it holds and not where the text that put it there was
written.

Reading Python, C++ and TypeScript. It is a tolerant textual read rather than a
full compiler frontend, extracting available structural declarations:

* A schema written as a **constructor call with literal arguments** — the Python
  and TypeScript shape — comes back whole: name, description, and every port with
  its type and description. Descriptions written as adjacent literals or joined
  with a `+` are one string, and a `"""..."""` one gives back the indentation
  its source put in front of it.
* A schema **assembled statement by statement** — the C++ shape, `schema.name =
  ...; schema.outputs.emplace(...)` — comes back with its name, its description and
  its port names. A port's type and description come from the literals of
  whatever call builds it, read for what they look like, so a helper whose
  arguments run in an unexpected order gives a port with no type rather than a
  port with the wrong one. A port added by a helper the schema was *passed to* is
  not found: following that means following a call across functions.
* A name a file binds to a constant resolves, including one declared in a `.cc`
  file's sibling header — which is how nearly every C++ action names itself, so
  without it the C++ side would find almost nothing.
* A name **computed at run time** is not found, and a schema whose name cannot be
  read is dropped: nothing can look up an action with no name, so half an entry
  would be worse than none.

`scanned` says what was *not* read, so a caller can tell a half-read tree from a
small one:

```json
{"scanned": {"files_read": 245, "reached_file_limit": false, "too_large": []}}
```

Both editors run this on open and again after a save, and fold the result into
the context they send. `flow.hover/v1` and `flow.definition/v1` then carry the
origin through, which is what makes "go to declaration" leave the flow.

### `flow.vocabulary/v1`

Every word set the language gives meaning to: the stages and what each takes, the
functions, the types, the statements, the clause words, the modifiers, the port and
field modifiers, the status codes, the status fields, the constants, the operators,
the duration units, and the punctuation. What something generating a static grammar
file reads instead of keeping a list of its own.

Beside the lists, **what each word does**, under `documentation`, keyed by role and
then by word:

```sh
a11 flow vocabulary --format json | jq '.documentation.symbol["|"]'
```

```json
{
  "summary": "Puts a stream through a stage.",
  "takes": "a stream on the left, a stage on the right",
  "detail": "The stages chain, and each one reads what the one before it produced ...",
  "example": "page.text | truncate 2000 -> brief.pages"
}
```

Every word of every set has one, which
`FlowVocabulary.EveryWordOfTheLanguageIsDocumented` pins: a form added to the
grammar without reference text fails CI rather than reaching a reader. That is
what a hover and the popup beside a completion list are both rendered from — one
answer to a question asked twice — and it is why hovering a `|` says what a pipe
does rather than `flow-operator`, which is the name of the token's kind and not an
answer.

The role names are the word-list keys, singular: `stage`, `builtin`, `statement`,
`declaration`, `clause_word`, `modifier`, `source`, `port_modifier`,
`field_modifier`, `type`, `constant`, `operator`, `status_code`, `status_field`,
`duration_unit`, `symbol`. A word genuinely in two sets — `stream` is a declaration
word and a port modifier — has one entry, reachable under either.

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

## Native language services

The language is C++: the lexer, the highlighter, the parser, the resolver, the
inspector, the formatter, the completion and the runtime. `a11 flow`, `a11-flow`,
the Python API and the IntelliJ plugin are frontends over it, and these formats are
the contract between them.

`a11.flow.loads`, `a11 flow check`, `a11 flow describe`, and the runtime use the
same native parser and resolver. The resulting graph is the source for
diagnostics, descriptions, and execution.

## Editors

An editor that can run a process needs no language knowledge at all, and there are
two worked examples.

The **IntelliJ plugin** (`intellij-plugin/`) runs one
`a11-flow serve --protocol json` per IDE and its Kotlin side is platform wiring — a
replay lexer over the token stream, an external annotator over the diagnostics, a
formatting service, a completion contributor. It carries no word lists, no parser
and no resolver.

The **VSCode extension** (`vscode-plugin/`) runs `a11-flow serve --protocol lsp`
per window and is a `vscode-languageclient` over it, which is what the protocol
half of this document exists to make possible: diagnostics, quick fixes, semantic
tokens, formatting, completion, hover, symbols and definitions arrive with no
language knowledge in the client at all.

The two differ in exactly one place, and it is a difference in the *editors* rather
than in the language. IntelliJ has a `MultiHostInjector`, so a flow inside a string
literal is a real document of the injected language and every feature works in it
for free. VSCode has no equivalent, so its extension colours fragments with the
generated injection grammar and asks the language about them as text through
`a11flow/relay`, translating offsets by where the fragment starts. Same capability,
each platform's own means — which is the useful shape for a contract like this one:
what is shared is the *answers*, and how an editor asks is its own business.

With no binary for the platform, both colour what they can and say so once.

A definition that *cannot* call out — a static grammar file loaded by a highlighter
— is **generated**:

```sh
a11 flow syntax                        # is the checked-in file current?
a11 flow syntax --generate             # write it
a11 flow syntax --target pygments      # one of them
```

There are four targets, and `a11 flow syntax` with no `--target` checks them all:

| Target | Written to | Read by |
| --- | --- | --- |
| `sublime` | `editors/sublime-text/A11 Flow.sublime-syntax` | Sublime, Zed, the TextMate family |
| `pygments` | `editors/pygments/a11flow_lexer.py` | MkDocs, Sphinx, `pygmentize` — every flow on these pages |
| `vscode` | `editors/vscode/a11flow.tmLanguage.json` | VSCode, for a `.flow` |
| `vscode-injection` | `editors/vscode/a11flow-injection.tmLanguage.json` | VSCode, for a flow inside a host language's string |

All four are written from the language's own tables, so a word the language gains
reaches them by running the generator; the check exits 1 when nobody has, which is
what CI gates on. The list of targets is the C++'s, asked for rather than restated,
so a target added there is a target the check covers.
