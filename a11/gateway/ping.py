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

"""The liveness action, now a builtin every A11 peer answers.

A ping is how a client tells "an A11 peer is listening here" apart from
"something is listening here". Anything can hold the port, so the probe must
complete before the client joins the peer.

``__ping`` is a runtime builtin, so
:meth:`a11.client.connection.GatewayConnection.probe` works with every A11
service. See ``cpp/a11/actions/builtins.h``.

The name and the schema are unchanged, because four languages' clients probe
with them. This module stays as the place they are spelled in Python.
"""

from __future__ import annotations

from a11 import actions

#: Reserved name of the liveness action.
PING_ACTION = "__ping"

PING_SCHEMA = actions.ActionSchema(
    name=PING_ACTION,
    description=(
        "Ping the server to check if it is alive. Requires a single value on"
        " the port `input`, which it returns as a single value on the port"
        " `output`."
    ),
    inputs={
        "input": actions.ActionPortSchema(
            name="input",
            description="Ping input value",
            type="text/plain",
            typeinfo=str,
        ),
    },
    outputs={
        "output": actions.ActionPortSchema(
            name="output",
            description="Pong response value",
            type="text/plain",
            typeinfo=str,
        ),
    },
)


__all__ = ["PING_ACTION", "PING_SCHEMA"]
