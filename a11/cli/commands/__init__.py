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

"""Registry of ``a11`` subcommands.

Every entry in `COMMANDS` becomes an ``a11 <name>`` subcommand. To add a
command, define a [Command][a11.cli.app.Command] in a sibling module and append
it here.
"""

from __future__ import annotations

from a11.cli.app import Command
from a11.cli.commands.account import (
    IDENTITY_COMMAND,
    LOGIN_COMMAND,
    LOGOUT_COMMAND,
    REGISTER_COMMAND,
    WHOAMI_COMMAND,
)
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
    IDENTITY_COMMAND,
    LOGIN_COMMAND,
    LOGOUT_COMMAND,
    REGISTER_COMMAND,
    SERVE_COMMAND,
    WHOAMI_COMMAND,
]

__all__ = ["COMMANDS"]
