# Copyright 2026 The A11 Authors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Start a chat with a selected LLM backend and model.

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
from a11.status import Status, StatusCode


def _configure(parser: argparse.ArgumentParser) -> None:
    parser.add_argument(
        "backend",
        nargs="?",
        default=None,
        help=(
            "LLM provider, or the first word of a coding task when it is not"
            f" one of the providers (default: {DEFAULT_PROVIDER})."
        ),
    )
    parser.add_argument(
        "model",
        nargs="?",
        default=None,
        help="Model id (defaults to the backend's default model).",
    )
    parser.add_argument(
        "task_words",
        nargs="*",
        help=argparse.SUPPRESS,
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
        "--log-level",
        choices=("debug", "info", "warning", "error", "critical"),
        default=None,
        help="Enable A11 runtime logs at this level (off by default).",
    )
    parser.add_argument(
        "--no-shell-tools",
        action="store_true",
        help=(
            "Deprecated compatibility option; omit persistent shell actions."
        ),
    )
    parser.add_argument("--task", help="Run an initial coding task.")
    parser.add_argument(
        "--cwd",
        default=".",
        help="Workspace directory (default: current directory).",
    )
    parser.add_argument(
        "--add-dir",
        action="append",
        default=[],
        help="Add an accessible workspace root; repeatable.",
    )
    parser.add_argument(
        "--approval",
        choices=("ask", "suggest", "auto"),
        default="ask",
        help="Permission policy (default: ask).",
    )
    parser.add_argument(
        "--sandbox",
        choices=("read-only", "workspace-write"),
        default="workspace-write",
        help="A11 native sandbox filesystem mode.",
    )
    parser.add_argument(
        "--plan",
        action="store_true",
        help="Plan without writing files or running effectful actions.",
    )
    parser.add_argument(
        "--resume",
        nargs="?",
        const="",
        metavar="SESSION",
        help="Resume a named session, or the most recent session.",
    )
    parser.add_argument(
        "--max-turns", type=int, default=50, help="Maximum user/model turns."
    )
    parser.add_argument(
        "--timeout",
        type=int,
        default=600,
        metavar="SECONDS",
        help="Deadline for each model turn (default: 600).",
    )
    parser.add_argument(
        "--non-interactive",
        action="store_true",
        help=(
            "Run the task once and exit; requires --task and non-ask approval."
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
    from a11 import logging as a11_logging

    if args.log_level:
        a11_logging.enable(args.log_level)
    else:
        a11_logging.disable()

    # Flags take precedence over the positionals; fall back to the default.
    positional_task = None
    positional_backend = args.backend
    positional_model = args.model
    if (
        positional_backend
        and normalize_provider_name(positional_backend) not in PROVIDER_CHOICES
    ):
        positional_task = " ".join(
            part
            for part in (
                positional_backend,
                positional_model,
                *args.task_words,
            )
            if part
        )
        positional_backend = None
        positional_model = None
    elif args.task_words:
        positional_task = " ".join(args.task_words)
    backend = normalize_provider_name(
        args.provider or positional_backend or DEFAULT_PROVIDER
    )
    model = args.model_flag or positional_model
    task = args.task or positional_task
    if args.non_interactive and (not task or args.approval == "ask"):
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message=(
                "--non-interactive requires a task and --approval suggest"
                " or auto."
            ),
        ).to_exception()
    if args.max_turns < 1 or args.timeout < 1:
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message="--max-turns and --timeout must both be positive.",
        ).to_exception()

    return await run_chat(
        backend,
        model,
        provider_explicit=bool(args.provider or positional_backend),
        verbose=args.verbose,
        gateway=args.gateway,
        shell_tools=not args.no_shell_tools,
        coding=True,
        cwd=args.cwd,
        add_dirs=tuple(args.add_dir),
        approval="suggest" if args.plan else args.approval,
        sandbox="read-only"
        if args.plan or args.no_shell_tools
        else args.sandbox,
        task=task,
        resume=args.resume,
        max_turns=args.max_turns,
        timeout_seconds=args.timeout,
        non_interactive=args.non_interactive,
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
