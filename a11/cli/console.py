# Copyright 2026 The A11 Authors.

"""One console for the CLI, and one honest answer to "can we be fancy here?".

Every module used to build its own `rich.console.Console`, which is fine until a
command has to be *scriptable*. The rule this module exists to enforce: rich
output is a shortcut to the same information, never the only way to get it. A
table when someone is watching, `key=value` lines when something is parsing, and
the same exit code either way.

Richness is off when stdout is not a terminal, when ``NO_COLOR`` is set, when
``TERM`` is ``dumb``, or when the user passed ``--plain``.
"""

from __future__ import annotations

import os
from collections.abc import Mapping, Sequence

from rich.console import Console
from rich.table import Table

_PLAIN_OVERRIDE = False


def set_plain(plain: bool) -> None:
    """Force plain output, as ``--plain`` does."""
    global _PLAIN_OVERRIDE
    _PLAIN_OVERRIDE = plain


def console(**kwargs) -> Console:
    """A console for CLI output."""
    return Console(**kwargs)


def is_rich(target: Console | None = None) -> bool:
    """Whether decorated output is appropriate.

    Args:
        target: The console to judge, or ``None`` to make a fresh one.

    Returns:
        False when output is redirected, colour is refused, the terminal is
        ``dumb``, or ``--plain`` was passed.
    """
    if _PLAIN_OVERRIDE:
        return False
    if os.environ.get("NO_COLOR"):
        return False
    if os.environ.get("TERM", "") == "dumb":
        return False
    resolved = target if target is not None else Console()
    return bool(resolved.is_terminal)


def add_plain_flag(parser) -> None:
    """Add ``--plain`` to ``parser``."""
    parser.add_argument(
        "--plain",
        action="store_true",
        help=(
            "Plain, parseable output: key=value lines instead of tables. Implied"
            " when stdout is not a terminal."
        ),
    )


def print_fields(
    fields: Mapping[str, object],
    *,
    title: str = "",
    target: Console | None = None,
) -> None:
    """Print a mapping as a table when rich, as ``key=value`` lines when not.

    The plain form is the contract: field names are stable and a value never
    contains a newline, so ``a11 gateway status | grep pid`` works.
    """
    out = target if target is not None else console()
    if not is_rich(out):
        for key, value in fields.items():
            out.print(f"{key}={_plain(value)}", markup=False, highlight=False)
        return
    table = Table(title=title or None, show_header=False, box=None)
    table.add_column(style="dim")
    table.add_column()
    for key, value in fields.items():
        table.add_row(key, _plain(value))
    out.print(table)


def print_rows(
    headers: Sequence[str],
    rows: Sequence[Sequence[object]],
    *,
    title: str = "",
    target: Console | None = None,
) -> None:
    """Print tabular data as a table when rich, tab-separated when not."""
    out = target if target is not None else console()
    if not is_rich(out):
        out.print("\t".join(headers), markup=False, highlight=False)
        for row in rows:
            out.print(
                "\t".join(_plain(cell) for cell in row),
                markup=False,
                highlight=False,
            )
        return
    table = Table(title=title or None)
    for header in headers:
        table.add_column(header)
    for row in rows:
        table.add_row(*[_plain(cell) for cell in row])
    out.print(table)


def _plain(value: object) -> str:
    """A single-line string for a field value."""
    if value is None:
        return ""
    if isinstance(value, bool):
        return "true" if value else "false"
    return str(value).replace("\n", " ")


__all__ = [
    "add_plain_flag",
    "console",
    "is_rich",
    "print_fields",
    "print_rows",
    "set_plain",
]
