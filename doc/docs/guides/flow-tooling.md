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

`check` exits `0` with no errors, `1` with language errors, and `2` when the file
cannot be read. A file named `-` reads standard input.

## `a11-flow`, the standalone tool

The `a11-flow` native binary provides the same commands and is built with
`cmake --build . --target a11_flow_tool`. It links only the language library,
Abseil, and nlohmann, so editor extensions and CI images can use it without the
full A11 runtime.

```sh
a11-flow check my.flow --format json
a11-flow fmt --check *.flow
a11-flow scan src/                # the actions the project declares, with origins
a11-flow serve --protocol lsp     # a language server, over stdio
a11-flow --stdio                  # the same, spelled the way an LSP client does
a11-flow serve --protocol json    # one request per line, one answer per line
```

`serve --protocol lsp` provides diagnostics and quick fixes, semantic tokens,
formatting, completion, hover, document symbols, and go-to-declaration.
`--stdio` is an alias accepted by clients such as `vscode-languageclient`.

Three custom methods cover Flow-specific integration. `a11flow/setContext`
sets the session's available actions and types. `a11flow/scan` extracts action
declarations from project source and adds them to that context.
`a11flow/relay` carries a JSON-protocol request for features outside LSP, such
as analysing Flow embedded in a host-language string.

`serve --protocol json` exposes the same capabilities as newline-delimited JSON
for hosts that do not use an LSP client:

```
{"id": 1, "method": "check", "source": "flow t { }"}
{"id": 1, "ok": true, "result": {"format": "flow.diagnostics/v1", ...}}
```

The methods are `check`, `tokens`, `parse`, `plan`, `format`, `complete`,
`describe`, `symbols`, `definition`, `catalogue`, `scan`, `schema`, `shapes`,
`codes`, `vocabulary`, and `syntax`. Each returns the corresponding envelope
described below. Every method accepts a `context` containing the actions and
types available outside the document; see `flow.catalogue/v1`.
`a11 flow serve` speaks the identical protocol through the Python bindings, for a
host that already has A11 installed.

### Which units the offsets are in

Offsets are **byte offsets** by default. JVM and JavaScript editor APIs usually
index strings in UTF-16 code units, where `§` occupies one unit and two UTF-8
bytes.

ASCII has identical byte and UTF-16 offsets, so test integrations with non-ASCII
text. Set the offset unit once per request:

```
{"method": "tokens", "source": "...", "offsets": "utf16"}
```

Use `"bytes"` (the default) or `"utf16"`. The selected unit applies to every
returned offset and to the inbound `offset` for `complete`. The service performs
the conversion.

Two fields use their own coordinate systems: a diagnostic's `line` and `column`
are 1-based code-point counts, and a completion proposal's `caret` is an offset
into the inserted proposal text.

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

The envelope remains usable without the source text:

* **A range carries offsets, lines, and columns.** Lines and columns are 1-based;
  ranges are half-open.
* **A fix contains complete edits.** Frontends apply them without deriving a
  repair. Fixes are present only when one safe edit is available.
* **`counts` supports build gates** without traversing the diagnostics list.
* **`flow` is absent, not empty,** when the text did not get far enough to name
  one.

### `flow.codes/v1`

The published table of every code the language can produce, its family, its
default severity and one line on what it means. A toolchain may match on a code:
codes are stable, and the wording of a message is not. The table is generated
from the C++ source of truth into `testdata/flow/codes.json`, which every
language reads.

```sh
a11 flow codes --format json | jq '.codes[] | select(.family == "unused")'
```

### `flow.tokens/v1`

`a11 flow highlight --format json` returns one entry per token with its semantic
role.

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

`lexical` contains the token's lexical type (`word`, `->`, `{`), while `kind`
contains its semantic role. Clients that tokenize locally can use both fields.

Tokens tile the source: every offset in the file is covered by exactly one of
them, comments included, so a client can colour a whole file from one response.
Columns count characters, not bytes.

### `flow.syntax/v1`

`a11 flow parse --format json` returns the parsed tree and all diagnostics. The
parser recovers after errors so editors can continue to highlight, format, and
check incomplete source.

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

* **`kind` identifies the node**, in kebab case. The remaining fields depend on
  that kind: `pipe` has a `pipeline` and `targets`, `call` has an
  `action`, `args` and `modifiers`, `for-each` has a `variable` and a `body`,
  `block` has a `body` and whether it is `tolerant`.
* **`at` identifies the node's first token**, not the full construct. Nesting it
  avoids a conflict with the `start` field used by `repeat`.
* **A duration is `{"$duration": seconds}`.** `250ms` and `0.25` are different
  things, and a reader should not have to guess which one a bare number was.
* **An unreadable statement becomes an `error` node** containing the expected
  syntax, so consumers can distinguish it from an omitted subtree.

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

`edits` contains one edit trimmed to the changed range, which limits cursor and
fold disruption. A file with an **error** is returned unchanged with
`changed: false` and an explanatory diagnostic.

#### What the formatter decides, and what it does not

It decides indentation (two spaces a level), the spaces between tokens, how far a
continued line is indented, how many blank lines are allowed and where, the columns
of a run of `in`/`out` or `header` declarations, trailing whitespace, and the
newline at the end of the file.

It does **not** change line breaks. Existing breaks are retained and indented;
new breaks are not introduced.

Two invariants, tested over every flow in the repository:

* **Idempotent.** Formatting formatted text changes nothing.
* **The program does not change.** Except for required block-body line breaks,
  formatted text has the same tokens and parse tree. Tests enforce this
  invariant.

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

`markdown` contains the full editor display; `summary` contains one line for a
status bar. `definition` is present only for declarations in the current
document.

`origin` identifies declarations found by `scan` in another file. A `definition`
is a range in the current document; an `origin` contains a path the host must
open.

The language service performs name resolution before returning hover results, so
editors do not reproduce this logic.

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

Shapes are the Flow representation; schemas are the external representation.
The `bytes`, `time`, and `duration` types become strings with an encoding or
format plus `x-a11-type`, preserving round trips. `x-a11-order` preserves field
declaration order.

### `flow.catalogue/v1`

The catalogue lists available actions and types with descriptions, ports, and
fields.

The language library cannot import a runtime registry, so context is supplied as
data. A snapshot generated
from the live registries
(`scripts/generate_flow_catalogue.py` → `testdata/flow/catalogue.json`) is
embedded, so the standalone tool can complete `make_http_request` without
additional configuration. A frontend with a live registry sends its own:

```json
{"method": "complete", "source": "...", "offset": 42,
 "context": {"actions": [...], "types": [...], "replace": false}}
```

The context merges with the snapshot, or replaces it when `"replace": true`.
Over LSP, send it once per session with `a11flow/setContext`.

#### Actions a project declares in its own source

The snapshot covers SDK actions. Use `scan` to add project-defined actions so
hover, completion, and navigation include them.

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

`origin` is 1-based, like a diagnostic's line and column. Its path retains the
form supplied to the scan: an absolute root produces absolute paths, and a
relative root produces relative paths. Entries from a live registry or the
embedded snapshot omit `origin` because those sources provide no file location.

`scan` performs a tolerant textual read of Python, C++, and TypeScript to extract
structural declarations:

* A schema written as a **constructor call with literal arguments** — the Python
  and TypeScript shape — comes back whole: name, description, and every port with
  its type and description. Descriptions written as adjacent literals or joined
  with a `+` are one string, and a `"""..."""` one gives back the indentation
  its source put in front of it.
* A schema **assembled statement by statement** — the C++ shape, `schema.name =
  ...; schema.outputs.emplace(...)` — comes back with its name, its description and
  its port names. Port types and descriptions come from literals in the
  construction call. If their positions are ambiguous, the field is omitted.
  The scan does not follow schemas passed through helper calls.
* Constants resolve within a file and from a `.cc` file's sibling header.
* Names computed at run time do not resolve. Schemas without a readable name are
  omitted.

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

`FlowVocabulary.EveryWordOfTheLanguageIsDocumented` requires documentation for
every entry. Hover and completion render this shared text, so hovering `|`
describes the pipe operation instead of showing only its `flow-operator` token
kind.

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

| Severity | Description |
| --- | --- |
| `error` | The flow does not compile. |
| `warning` | It compiles and does something other than what it says. |
| `weak-warning` | The flow compiles, but part of it has no effect. |
| `information` | Non-blocking information. |

Editors can expose each diagnostic **family** as one switchable inspection:
`syntax`, `form`, `name`, `sequence`, `barrier`, and `unused`. The middle segment
of each code contains its family, as in `flow.unused.header`.

## In Python

Python exposes the same shapes as values:

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

Editors can delegate language analysis to the service. The repository contains
two integrations.

The **IntelliJ plugin** (`intellij-plugin/`) runs one
`a11-flow serve --protocol json` process per IDE. Kotlin adapts the returned
tokens, diagnostics, formatting, and completions to IntelliJ APIs. It contains no
Flow word lists, parser, or resolver.

The **VS Code extension** (`vscode-plugin/`) runs
`a11-flow serve --protocol lsp` per window through `vscode-languageclient`. The
server supplies diagnostics, quick fixes, semantic tokens, formatting,
completion, hover, symbols, and definitions.

Embedded Flow uses editor-specific integration. IntelliJ exposes a string
literal as an injected-language document through `MultiHostInjector`. VS Code
uses the generated injection grammar and sends fragment text through
`a11flow/relay`, translating offsets from the fragment to the host document.

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

All four files are generated from the language tables. The check exits `1` when
any generated file is stale. It reads the target list from C++, so new targets
are included automatically.
