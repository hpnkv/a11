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

"""Run a gateway in this process over an in-memory stream pair.

``a11 chat`` uses this gateway when no remote gateway is available. It uses the
same gateway, session, tool bridge, and LLM handler as a remote connection.
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

#: Grace period for the server task to stop after the client connection closes.
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
        # Bound server shutdown after the client stream closes.
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
