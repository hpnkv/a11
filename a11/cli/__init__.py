# Copyright 2026 The A11 Authors.

"""The ``a11`` command-line interface.

This package hosts the ``a11`` console entry point (see ``pyproject.toml``'s
``[project.scripts]``) and its subcommands. Commands are self-registering: each
module under [a11.cli.commands][a11.cli.commands] exposes a
[Command][a11.cli.app.Command]
that [a11.cli.app][a11.cli.app] discovers and wires into the top-level argument
parser.

Nothing here imports a specific LLM provider at module load time; provider SDKs
(``anthropic``, ``google-genai``) are pulled in lazily by the backends that
need them so ``a11 --help`` works on a bare ``pip install a11-kit``.
"""

from a11.cli.app import Command, build_parser, main

__all__ = ["Command", "build_parser", "main"]
