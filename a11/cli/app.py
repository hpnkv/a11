# Copyright 2026 The A11 Authors.

"""Top-level argument parsing and command dispatch for the ``a11`` CLI.

The CLI is intentionally thin: `build_parser` assembles an
`argparse` parser from a registry of `Command` objects, and
`main` parses ``argv``, then runs the selected command's coroutine on a
fresh event loop. Commands are async by default so they compose naturally with
A11's asyncio-based runtime.

Adding a command is a two-step affair: write a module under
[a11.cli.commands][a11.cli.commands] that builds a `Command`, then list it in
[a11.cli.commands.COMMANDS][a11.cli.commands.COMMANDS]. No changes here are
required.
"""

from __future__ import annotations

import argparse
import asyncio
import dataclasses
from typing import Awaitable, Callable, Sequence

_PROG = "a11"
_DESCRIPTION = "A11 — concurrent action and streaming runtime."


@dataclasses.dataclass(frozen=True)
class Command:
    """A single ``a11 <name>`` subcommand.

    Attributes:
        name: The subcommand token, e.g. ``"chat"``.
        help: One-line summary shown in ``a11 --help``.
        run: Coroutine invoked with the parsed
        [argparse.Namespace][argparse.Namespace].
            Returns a process exit code (0 for success).
        configure: Optional hook that receives the subcommand's
            [argparse.ArgumentParser][argparse.ArgumentParser] to declare its
            arguments.
        description: Longer help shown in ``a11 <name> --help`` (defaults to
            ``help``).
    """

    name: str
    help: str
    run: Callable[[argparse.Namespace], Awaitable[int]]
    configure: Callable[[argparse.ArgumentParser], None] | None = None
    description: str | None = None


def build_parser(commands: Sequence[Command]) -> argparse.ArgumentParser:
    """Build the top-level parser wired to ``commands``."""
    parser = argparse.ArgumentParser(prog=_PROG, description=_DESCRIPTION)
    parser.set_defaults(_command=None)

    subparsers = parser.add_subparsers(
        title="commands", metavar="<command>", dest="command"
    )
    for command in commands:
        sub = subparsers.add_parser(
            command.name,
            help=command.help,
            description=command.description or command.help,
        )
        if command.configure is not None:
            command.configure(sub)
        sub.set_defaults(_command=command)

    return parser


def main(argv: Sequence[str] | None = None) -> int:
    """Parse ``argv`` and dispatch to the selected command.

    Returns the command's exit code. Intended as the ``a11`` console entry
    point, so it also serves as ``python -m a11.cli``.
    """
    # Imported here (not at module top) so the command registry — and any
    # provider probing it does — is only built when the CLI actually runs.
    from a11.cli.commands import COMMANDS
    from a11 import observability

    otlp_configured = observability.configure_langfuse_from_env()

    if not otlp_configured:
        observability.configure_otel_from_env()

    parser = build_parser(COMMANDS)
    args = parser.parse_args(argv)

    command: Command | None = getattr(args, "_command", None)
    if command is None:
        parser.print_help()
        return 2

    try:
        return asyncio.run(command.run(args))
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    raise SystemExit(main())
