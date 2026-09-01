# Copyright 2026 The A11 Authors.

"""``a11 chat [claude|claude_code|gemini|ollama|vllm] [model]`` — a chat.

Thin wrapper: it parses the backend/model arguments and hands off to
[a11.cli.chat_ui.run_chat][a11.cli.chat_ui.run_chat], where the actual
conversation loop lives.
"""

from __future__ import annotations

import argparse

from a11.cli.app import Command
from a11.cli.backends import (
    DEFAULT_PROVIDER,
    PROVIDER_CHOICES,
    normalize_provider_name,
)
from a11.cli.voice import DEFAULT_VOICE_MODEL, VOICE_MODELS


def _configure(parser: argparse.ArgumentParser) -> None:
    parser.add_argument(
        "backend",
        nargs="?",
        default=None,
        choices=PROVIDER_CHOICES,
        help=f"LLM provider to chat with (default: {DEFAULT_PROVIDER}).",
    )
    parser.add_argument(
        "model",
        nargs="?",
        default=None,
        help="Model id (defaults to the backend's default model).",
    )
    parser.add_argument(
        "--provider",
        choices=PROVIDER_CHOICES,
        default=None,
        help="LLM provider (overrides the positional backend).",
    )
    parser.add_argument(
        "--model",
        dest="model_flag",
        metavar="MODEL",
        default=None,
        help="Model id (overrides the positional model).",
    )
    parser.add_argument(
        "--header",
        action="append",
        nargs=2,
        metavar=("KEY", "VALUE"),
        default=None,
        dest="headers",
        help=(
            "Set a header on every interact_with_* call; repeatable. Overrides"
            " the defaults for the same key (e.g. --header x-a11-llm-base-url"
            " http://host:11434)."
        ),
    )
    parser.add_argument(
        "--gateway",
        metavar="URL",
        default=None,
        help=(
            "A11 gateway to use, e.g. ws://127.0.0.1:8011/a11. When given it"
            " must be reachable or the command fails. When omitted, an"
            " already-running gateway at ws://127.0.0.1:8011/a11 is used, and"
            " otherwise one is started in this process."
        ),
    )
    parser.add_argument(
        "-v",
        "--verbose",
        action="store_true",
        help="Log raw interaction events as they stream in.",
    )
    parser.add_argument(
        "--no-shell-tools",
        action="store_true",
        help=(
            "Disable the shell tools (and their system prompt) that let the"
            " model run commands in your environment."
        ),
    )
    parser.add_argument(
        "--no-voice",
        action="store_true",
        help="Disable local speech recognition and model downloads.",
    )
    parser.add_argument(
        "--voice-model",
        choices=tuple(VOICE_MODELS),
        default=DEFAULT_VOICE_MODEL,
        help=(
            "Local whisper.cpp model used for voice input "
            f"(default: {DEFAULT_VOICE_MODEL})."
        ),
    )


async def _run(args: argparse.Namespace) -> int:
    # Imported lazily so `a11 --help` never pulls in prompt_toolkit/rich work.
    from a11.cli.chat_ui import run_chat

    # Flags take precedence over the positionals; fall back to the default.
    backend = normalize_provider_name(
        args.provider or args.backend or DEFAULT_PROVIDER
    )
    model = args.model_flag or args.model

    return await run_chat(
        backend,
        model,
        verbose=args.verbose,
        gateway=args.gateway,
        shell_tools=not args.no_shell_tools,
        voice=not args.no_voice,
        voice_model=args.voice_model,
        extra_headers=[(key, value) for key, value in args.headers or []],
    )


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
