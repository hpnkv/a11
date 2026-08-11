# Editor support for A11 Flow

The [Flow language](../doc/docs/guides/flow.md) describes a composition of A11
actions. Its files are `.flow`, and — because a flow is meant to travel as text —
most of the ones people read are inside a string literal in some other language.
Both editors here handle both.

| Editor | Where | `.flow` files | Flows in string literals |
| --- | --- | --- | --- |
| Sublime Text | [`sublime-text/`](sublime-text) | yes | — |
| IntelliJ IDEs | [`../intellij-plugin/`](../intellij-plugin) | yes | yes, automatically |

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

## Keeping them honest

A highlighter is a second copy of the language's vocabulary, and a second copy
drifts. [`a11/flow/tests/test_editor_support.py`](../a11/flow/tests/test_editor_support.py)
reads the word lists back out of both definitions and checks them against the
parser's own tables, so adding a stage or a status code fails the Python test
suite until every editor has been told about it. The IntelliJ lexer has its own
tests in
[`FlowLexerTest.kt`](../intellij-plugin/src/test/kotlin/dev/curiositystack/a11/clion/flow/FlowLexerTest.kt),
which check it against the rules in `a11/flow/lexer.py`.
