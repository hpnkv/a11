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

"""Command names, help text, and deferred implementation loaders.

Register a command's metadata here and implement its arguments and execution in
`a11.cli.commands`. Top-level help reads only this metadata.
"""

from __future__ import annotations

import argparse
import importlib
from functools import cache

from a11_cli.app import Command

_SPECIFICATIONS = (
    ("chat", "chat", "CHAT_COMMAND", "Chat interactively with an LLM backend."),
    (
        "llm-context",
        "llm_context",
        "LLM_CONTEXT_COMMAND",
        "Compress, compact, measure, or intercept LLM context.",
    ),
    (
        "discover",
        "discover",
        "DISCOVER_COMMAND",
        "Ask an A11 peer which actions it serves.",
    ),
    ("flow", "flow", "FLOW_COMMAND", "Check and describe Flow programs."),
    ("gateway", "gateway", "GATEWAY_COMMAND", "Run or manage the A11 gateway."),
    (
        "identity",
        "account",
        "IDENTITY_COMMAND",
        "Register and list hosted identities.",
    ),
    ("login", "account", "LOGIN_COMMAND", "Log in to an A11 exchange."),
    (
        "logout",
        "account",
        "LOGOUT_COMMAND",
        "Forget this machine's exchange credential.",
    ),
    (
        "register",
        "account",
        "REGISTER_COMMAND",
        "Create an account on an A11 exchange.",
    ),
    (
        "serve",
        "serve",
        "SERVE_COMMAND",
        "Serve an Action registry from a module.",
    ),
    (
        "whoami",
        "account",
        "WHOAMI_COMMAND",
        "Show the stored credential and what it may do.",
    ),
)


def _command_help(name: str) -> str:
    return next(help for token, _, _, help in _SPECIFICATIONS if token == name)


def _deferred(name: str, module: str, attribute: str, help: str) -> Command:
    @cache
    def load() -> Command:
        return getattr(
            importlib.import_module(f"a11.cli.commands.{module}"), attribute
        )

    async def run(args: argparse.Namespace) -> int:
        return await load().run(args)

    def configure(parser: argparse.ArgumentParser) -> None:
        concrete = load()
        if concrete.configure is not None:
            concrete.configure(parser)

    return Command(
        name=name, help=help, run=run, configure=configure, loader=load
    )


COMMANDS: list[Command] = [_deferred(*entry) for entry in _SPECIFICATIONS]

__all__ = ["COMMANDS"]
