# Copyright 2026 The A11 Authors.

"""A gateway inside this process, reached over an in-memory stream pair.

What `a11 chat` falls back to when no gateway is running. The point is that it is
not a *different* code path: the same `A11Gateway`, the same per-connection
session and tool bridge, the same `interact_with_llm` handler that persists
conversations -- only the transport changes, from a WebSocket to
`create_in_process_wire_stream_pair`. So a chat against an embedded gateway
exercises the same machinery as one against a remote gateway, which is why there
is only one turn loop left to maintain.
"""

from __future__ import annotations

import asyncio
import contextlib
from collections.abc import AsyncIterator

from absl import logging

import a11
from a11 import net, timing
from a11.client.connection import GatewayConnection
from a11.gateway import app
from a11.gateway.config import GatewayConfig
from a11.service.session import Session

#: How long to let the server side notice the client is gone before cancelling
#: it. Short on purpose: by the time we get here the turn has completed and the
#: gateway has already recorded it (persistence happens inside the
#: ``interact_with_llm`` call, which `run_turn` awaits), so what remains is a
#: session waiting to see whether its peer says anything else. Waiting seconds
#: for that would just make `a11 chat` slow to exit.
SHUTDOWN_GRACE = timing.Duration.milliseconds(500)


@contextlib.asynccontextmanager
async def embedded_gateway(
    config: GatewayConfig | None = None,
    *,
    registry: a11.ActionRegistry | None = None,
) -> AsyncIterator[GatewayConnection]:
    """Run a gateway in this process and yield a connection to it.

    Args:
        config: Gateway configuration. Defaults to serving everything.
        registry: Registry the gateway's reverse-dispatched tool calls run
            against, i.e. this client's own tools.

    Yields:
        A connection to the embedded gateway, torn down on exit.
    """
    gateway = app.init_app(config)
    server_stream, client_stream = net.create_in_process_wire_stream_pair()

    serving = asyncio.create_task(
        gateway.handle_stream(server_stream), name="embedded-gateway"
    )
    session = Session(action_registry=registry or a11.ActionRegistry())
    await session.add_stream(client_stream, mode="start")
    connection = GatewayConnection(session, client_stream, embedded=True)
    logging.info("started an in-process gateway")

    try:
        yield connection
    finally:
        await connection.aclose()
        # The server side ends when its stream does. Bound the wait so a wedged
        # handler cannot hang the CLI's exit, and cancel rather than leaving a
        # pending task for asyncio to complain about at shutdown.
        try:
            await asyncio.wait_for(
                asyncio.shield(serving),
                timeout=SHUTDOWN_GRACE.float_seconds(),
            )
        except (asyncio.TimeoutError, asyncio.CancelledError):
            logging.info("embedded gateway did not stop in time; cancelling")
            serving.cancel()
            with contextlib.suppress(
                asyncio.CancelledError, Exception
            ):
                await serving
        except Exception:  # noqa: BLE001 - shutdown is best effort
            logging.debug("embedded gateway ended with an error", exc_info=True)


__all__ = ["SHUTDOWN_GRACE", "embedded_gateway"]
