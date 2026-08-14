# Editor support for A11 Flow

The [Flow language](../doc/docs/guides/flow.md) describes a composition of A11
actions. Its files are `.flow`, and — because a flow is meant to travel as text —
most of the ones people read are inside a string literal in some other language.
Both editors here handle both.

| Editor | Where | `.flow` files | Flows in string literals |
| --- | --- | --- | --- |
| Sublime Text | [`sublime-text/`](sublime-text) | yes | — |
| IntelliJ IDEs | [`../intellij-plugin/`](../intellij-plugin) | yes | yes, automatically |

[`pygments/`](pygments) is not an editor: it is the lexer that colours a fenced
flow on a documentation page, and it is here because it is generated the same way
the Sublime grammar is. A11's own site uses it (see
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

## Neither of these knows the language

A highlighter used to be a second copy of the language's vocabulary, and a second
copy drifts. Neither of these is a copy any more.

The **Sublime** grammar and the **Pygments** lexer are **generated**. Each is a
static file that a highlighter loads and cannot call out of, so
`cpp/a11/flow/generate.cc` writes both from the language's own word tables:

```sh
a11 flow syntax --target sublime --generate    # write it
a11 flow syntax --target pygments --generate
a11 flow syntax --target pygments             # is it current? (CI gates on this)
```

Editing it by hand is pointless: the next generation overwrites it, and the check
fails until somebody notices.

The **IntelliJ** plugin *asks*. It runs one `a11-flow serve --protocol json` per IDE
and the answers are its lexer, its inspections, its formatter and its completion —
so a stage added to the grammar is a stage it colours, offers and checks with no
plugin change. Its Kotlin side is platform wiring and nothing else; with no binary
for the platform it colours what it can and says so once.

That is the whole reason for the `a11-flow` binary: an editor that can start a
process needs no language knowledge, and one that cannot gets a generated file.
[`a11/flow/tests/test_editor_support.py`](../a11/flow/tests/test_editor_support.py)
holds both properties in place — that the generated file is current, and that no
word list has crept back into the plugin.
