# Copyright 2026 The A11 Authors.

"""The gateway's liveness action, shared by the server and its clients.

Both sides need this schema -- the gateway to serve it, a client to probe with it
-- and neither should have to import the other's module to get it. It lives on
its own so a client can probe without dragging in the shell and audio SDKs that
`a11.gateway.app` pulls in.

A ping is how a client tells "a gateway is listening here" apart from "something
is listening here". The distinction matters because anything at all can hold the
port, and a peer that completes a TCP handshake but does not speak A11 would
otherwise be joined and then hang on the first real call.
"""

from __future__ import annotations

import logging
from typing import cast

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


async def ping(action: actions.Action) -> None:
    """Echo the single value on ``input`` back on ``output``."""
    stream_str = "<no stream>"
    if action.get_stream():
        stream_str = str(action.get_stream().get_id())

    logging.info(f"[{stream_str}] running ping on stream {stream_str}")
    async with action["output"] as output_node:
        ping_value = cast(str, await action["input"].consume(str))
        await output_node.put_final(ping_value)

    logging.info(f"[{stream_str}] ping complete")


__all__ = ["PING_ACTION", "PING_SCHEMA", "ping"]
