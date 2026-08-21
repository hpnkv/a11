# Copyright 2026 The A11 Authors.

"""Registry of ``a11`` subcommands.

Every entry in `COMMANDS` becomes an ``a11 <name>`` subcommand. To add a
command, define a [Command][a11.cli.app.Command] in a sibling module and append
it here.
"""

from __future__ import annotations

from a11.cli.app import Command
from a11.cli.commands.chat import CHAT_COMMAND
from a11.cli.commands.discover import DISCOVER_COMMAND
from a11.cli.commands.flow import FLOW_COMMAND
from a11.cli.commands.gateway import GATEWAY_COMMAND
from a11.cli.commands.serve import SERVE_COMMAND

COMMANDS: list[Command] = [
    CHAT_COMMAND,
    DISCOVER_COMMAND,
    FLOW_COMMAND,
    GATEWAY_COMMAND,
    SERVE_COMMAND,
]

__all__ = ["COMMANDS"]
