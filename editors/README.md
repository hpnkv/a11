# Editor support for A11 Flow

The [Flow language](../doc/docs/guides/flow.md) describes a composition of A11
actions. Its files are `.flow`, and — because a flow is meant to travel as text —
most of the ones people read are inside a string literal in some other language.

| Editor | Where | `.flow` files | Flows in string literals |
| --- | --- | --- | --- |
| Sublime Text | [`sublime-text/`](sublime-text) | yes | — |
| IntelliJ IDEs | [`../intellij-plugin/`](../intellij-plugin) | yes | yes, automatically |
| VSCode | [`../vscode-plugin/`](../vscode-plugin) | yes | coloured, and checked |

[`pygments/`](pygments) is not an editor: it is the lexer that colours a fenced
flow on a documentation page, and it is here because it is generated the same way
the Sublime and VSCode grammars are. A11's own site uses it (see
[`doc/hooks/flow_highlighting.py`](../doc/hooks/flow_highlighting.py)), and so can
anything else built on Pygments — MkDocs, Sphinx, `pygmentize`.

## IntelliJ

The language is part of the **A11 Chat** plugin in
[`intellij-plugin/`](../intellij-plugin), so it arrives with the rest of A11's
IDE support and works in every JetBrains IDE — it uses only platform API.

A flow inside a string literal is highlighted where it is written, the way SQL is
in a Python string. Nothing has to be configured: a string whose first real word
is `flow` followed by a name and a `{` is treated as one.

```python
program = flow.loads("""
    flow shout {                          # highlighted from here
      in  words:   string stream
      out loudest: string

      say = call text-upper(text: words)
      say.upper | first 1 -> loudest
    }
""")
```

For a fragment that cannot say so itself, the platform's own markers work,
because the language is registered like any other:

```python
# language=A11Flow
fragment = "shout.upper | first 1 -> loudest"
```

and `@Language("A11Flow")` does the same for a Java or Kotlin host. Alt+Enter →
*Inject language or reference* → **A11 Flow** works too.

## VSCode

The language is part of the **A11 Flow & Chat** extension in
[`vscode-plugin/`](../vscode-plugin), which is a language client over
`a11-flow serve --protocol lsp`: diagnostics with their quick fixes, semantic
highlighting, formatting, completion, hover, symbols and go-to-declaration.

A flow inside a string literal is coloured by [`vscode/`](vscode) — an *injection*
grammar, applied to the string scopes of Python, TypeScript, JavaScript, Java,
Kotlin, Go and C++ — and checked by asking the language about the fragment as text.
The rule for what counts as one is the same as the JetBrains plugin's, so the same
string is recognised in both.

What VSCode has no equivalent of is the injector itself: there is no way to declare
that a range of a document *is* another language and have every feature follow. So
that one capability is reached differently in each editor rather than pretended to
be the same, and the extension's README says which parts are shared and which are
not, and why.

## None of these knows the language

A highlighter used to be a second copy of the language's vocabulary, and a second
copy drifts. None of these is a copy any more.

The **Sublime** grammar, the **Pygments** lexer and both **VSCode** grammars are
**generated**. Each is a static file that a highlighter loads and cannot call out
of, so `cpp/a11/flow/generate.cc` writes all four from the language's own word
tables:

```sh
a11 flow syntax --generate                     # write all of them
a11 flow syntax --target vscode --generate     # or just one
a11 flow syntax                                # are they current? (CI gates on this)
```

The list of targets comes from the C++ as well, so a target added there is a target
the check covers rather than one a list somewhere has to be told about.

Editing it by hand is pointless: the next generation overwrites it, and the check
fails until somebody notices.

The **IntelliJ** plugin and the **VSCode** extension *ask*. One runs
`a11-flow serve --protocol json` per IDE, the other
`a11-flow serve --protocol lsp` per window, and in both the answers are the lexer,
the inspections, the formatter and the completion — so a stage added to the grammar
is a stage they colour, offer and check with no change in either. Their own code is
platform wiring and nothing else; with no binary for the platform each colours what
it can and says so once.

Both also *read the project*: `a11-flow scan` finds the `ActionSchema` declarations
in a workspace's own Python, C++ and TypeScript, so hovering an action somebody
wrote this afternoon shows what it does and go-to-declaration lands on it. That is
world knowledge rather than language knowledge, and it arrives the same way the
rest does — as data, from one implementation.

That is the whole reason for the `a11-flow` binary: an editor that can start a
process needs no language knowledge, and one that cannot gets a generated file.
[`a11/flow/tests/test_editor_support.py`](../a11/flow/tests/test_editor_support.py)
holds both properties in place — that the generated file is current, and that no
word list has crept back into the plugin.
