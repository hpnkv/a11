# Copyright 2026 The A11 Authors.

"""The liveness action, now a builtin every A11 peer answers.

A ping is how a client tells "an A11 peer is listening here" apart from
"something is listening here". The distinction matters because anything at all
can hold the port, and a peer that completes a TCP handshake but does not speak
A11 would otherwise be joined and then hang on the first real call.

It used to be registered by `a11.gateway.app` and by nothing else, which meant
:meth:`a11.client.connection.GatewayConnection.probe` failed against every A11
service that was not *this* gateway -- an `a11 serve`, an IDE plugin, a demo. It
is now one of the actions a registry answers for whether or not anybody
installed it (see `cpp/a11/actions/builtins.h`), so the probe works against any
peer.

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
