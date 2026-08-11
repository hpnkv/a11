# A11 Flow for Sublime Text

Syntax highlighting for the [Flow language](../../doc/docs/guides/flow.md):
`.flow` files describing a composition of A11 actions.

## Install

Copy this directory into Sublime Text's `Packages` directory, as a folder named
`A11 Flow`:

| Platform | Path |
| --- | --- |
| macOS | `~/Library/Application Support/Sublime Text/Packages/A11 Flow/` |
| Linux | `~/.config/sublime-text/Packages/A11 Flow/` |
| Windows | `%APPDATA%\Sublime Text\Packages\A11 Flow\` |

```sh
# macOS, from the repository root
cp -R editors/sublime-text \
  "$HOME/Library/Application Support/Sublime Text/Packages/A11 Flow"
```

Sublime picks it up immediately: open any `.flow` file, or choose **A11 Flow**
from the syntax menu. `Cmd/Ctrl-/` comments with `#`.

## What it highlights

* `flow` names, and the ports, headers and `describe` line that declare one
* statements -- `run`, `call`, `node`, `nodes`, `skip`, `wait`, `drain`,
  `status`, `cancel`, `fail`, `for`, `repeat`, `until`, `while`, `if`, `else`
* call modifiers (`tee`, `via`, `timeout`, `after`, `with`, `id`) and `try`
* pipeline stages after a `|`, and `then`/`where` where they drop the pipe --
  `history then asked`, `hits where it.ok` -- which is a stage with an operand
  after it, and a plain name without one
* the operators `->`, `<-`, `|`, and the comparisons
* port types -- built-in names, generics such as `list[string]`, and the tags a
  serialisation registry knows a type by, like `a11.sdk.AudioBuffer`
* the built-in functions, `it`, and the canonical status codes
* strings with escapes, numbers, and durations such as `30s`, `250ms` or `500ns`

Every significant word is accepted in lower case or UPPER CASE, and *only* in
those: `For` is highlighted as a name, because that is what the compiler reads it
as. That correspondence is checked by
[`a11/flow/tests/test_editor_support.py`](../../a11/flow/tests/test_editor_support.py),
so the highlighter cannot quietly fall behind the language.

## Other editors

IntelliJ IDEs are covered by the A11 plugin in
[`intellij-plugin/`](../../intellij-plugin), which highlights `.flow` files and
also flows written inside string literals. See [`../README.md`](../README.md).

The language's tables live in `a11/flow/parser.py` and `a11/flow/plan.py`, which
is where any further highlighter should take its word lists from.
