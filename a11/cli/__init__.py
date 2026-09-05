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

"""The ``a11`` command-line interface.

This package hosts the ``a11`` console entry point (see ``pyproject.toml``'s
``[project.scripts]``) and its subcommands. Commands are self-registering: each
module under [a11.cli.commands][a11.cli.commands] exposes a
[Command][a11.cli.app.Command]
that [a11.cli.app][a11.cli.app] discovers and wires into the top-level argument
parser.

Provider SDKs (``anthropic``, ``google-genai``, ``openai``) are pulled in lazily
by the backends that need them so ``a11 --help`` works on a bare
``pip install a11-kit``.
"""

from a11.cli.app import Command, build_parser, main

__all__ = ["Command", "build_parser", "main"]
