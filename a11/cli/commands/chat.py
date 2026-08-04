# Copyright 2026 The A11 Authors.

"""``a11 chat [claude|gemini|ollama] [model]`` — an interactive LLM chat.

Thin wrapper: it parses the backend/model arguments and hands off to
[a11.cli.chat_ui.run_chat][a11.cli.chat_ui.run_chat], where the actual
conversation loop lives.
"""

from __future__ import annotations

import argparse

from a11.cli.app import Command
from a11.cli.backends import DEFAULT_PROVIDER, PROVIDERS


def _configure(parser: argparse.ArgumentParser) -> None:
    parser.add_argument(
        "backend",
        nargs="?",
        default=DEFAULT_PROVIDER,
        choices=sorted(PROVIDERS),
        help=f"LLM provider to chat with (default: {DEFAULT_PROVIDER}).",
    )
    parser.add_argument(
        "model",
        nargs="?",
        default=None,
        help="Model id (defaults to the backend's default model).",
    )
    parser.add_argument(
        "-v",
        "--verbose",
        action="store_true",
        help="Log raw interaction events as they stream in.",
    )


async def _run(args: argparse.Namespace) -> int:
    # Imported lazily so `a11 --help` never pulls in prompt_toolkit/rich work.
    from a11.cli.chat_ui import run_chat

    return await run_chat(args.backend, args.model, verbose=args.verbose)


CHAT_COMMAND = Command(
    name="chat",
    help="Chat interactively with an LLM backend.",
    description=(
        "Start an interactive chat with an LLM backend. Switch backends "
        "mid-session with /model, and see /help for the full command list."
    ),
    configure=_configure,
    run=_run,
)

__all__ = ["CHAT_COMMAND"]
