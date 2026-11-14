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

"""Connect to an explicit gateway or discover the local default.

An explicit endpoint must connect successfully. With no endpoint, a ``__ping``
round trip checks whether an A11 gateway occupies the default address; the
client starts a local WebSocket gateway when none answers.
"""

from __future__ import annotations

import asyncio
import contextlib
from collections.abc import AsyncIterator, Sequence

from absl import logging

import a11
from a11 import net, timing
from a11.gateway.config import DEFAULT_GATEWAY_URL
from a11.gateway.config import GatewayConfig
from a11.gateway.ping import PING_ACTION, PING_SCHEMA
from a11.service.session import Session
from a11.status import Status, StatusCode, StatusException

#: How long to wait for the transport to come up during a probe. Short: this
#: runs on the fallback path, where the common answer is "nothing is there".
PROBE_CONNECT_TIMEOUT = timing.Duration.milliseconds(750)
#: How long to wait for the ``__ping`` round trip to complete.
PROBE_CALL_TIMEOUT = timing.Duration.milliseconds(1500)
#: How long to wait for a connection the caller asked for explicitly.
CONNECT_TIMEOUT = timing.Duration.seconds(10)


def websocket_client_options(
    handshake_deadline: "timing.Time",
) -> "net.WebSocketClientOptions":
    """Use RFC 6455 over HTTP/1.1 with a handshake-only deadline.

    ``client_preference`` selects HTTP/1.1 even for ``wss://``. The deadline
    applies to the handshake; an HTTP request deadline would end the active
    WebSocket session.
    """
    options = net.WebSocketClientOptions()
    options.http2_options.client_preference = net.HttpProtocolPreference.HTTP11
    options.http2_options.enable_h2 = False
    options.http2_options.enable_h2c = False
    options.handshake_deadline = handshake_deadline
    return options


class GatewayConnection:
    """One client session on one stream to a gateway.

    Owns the `Session` the caller's actions are dispatched through, and the
    stream they travel on. Both are needed to build a call, which is why they
    travel together rather than as two loose arguments.
    """

    def __init__(
        self,
        session: Session,
        stream: net.WireStream,
        *,
        url: str = "",
        embedded: bool = False,
    ) -> None:
        self.session = session
        self.stream = stream
        #: Where this connection goes, for messages. Empty when in-process.
        self.url = url
        #: Whether the gateway is running inside this process.
        self.embedded = embedded
        self._registry = session.action_registry

    @property
    def description(self) -> str:
        """How to describe this connection to a person."""
        return "in-process gateway" if self.embedded else self.url

    @classmethod
    async def connect(
        cls,
        url: str,
        *,
        registry: a11.ActionRegistry | None = None,
        timeout: timing.Duration | None = None,
        headers: list[tuple[str, str]] | None = None,
    ) -> GatewayConnection:
        """Open a session to the gateway at ``url``.

        Does not verify that the peer is a gateway; call `probe` for that.

        Raises:
            StatusException: When the transport cannot be established.
        """
        # The connect deadline bounds only the handshake. It must not
        # become `WireStreamOptions.deadline`, which is the absolute time the
        # stream is aborted at: a chat session outlives its handshake by hours,
        # and a stream deadline of "now + 10s" makes every turn after the first
        # ten seconds fail with "This endpoint has already terminated".
        handshake_deadline = timing.now() + (timeout or CONNECT_TIMEOUT)
        options = net.WireStreamOptions()  # no deadline: the stream lives on
        websocket_options = websocket_client_options(handshake_deadline)
        websocket_options.headers = headers or []
        stream = net.WebSocketWireStream.connect(
            url, options, websocket_options=websocket_options
        )
        session = Session(action_registry=registry or a11.ActionRegistry())
        await session.add_stream(stream, mode="start")
        return cls(session, stream, url=url)

    async def probe(self, *, timeout: timing.Duration | None = None) -> None:
        """Verify the peer is a live A11 gateway, via a ``__ping`` round trip.

        Raises:
            StatusException: When the peer is not a gateway.
            TimeoutError: When it does not answer in time.
        """
        bound = (timeout or PROBE_CALL_TIMEOUT).float_seconds()
        # Bound the full ping, including the output read from a silent peer.
        await asyncio.wait_for(self._ping(), timeout=bound)

    async def _ping(self) -> None:
        call = self.action(PING_ACTION, PING_SCHEMA)
        await call.call()
        await call["input"].finalize("probe")
        await call["output"].consume(str)
        await call.wait(PROBE_CALL_TIMEOUT)

    async def aclose(self) -> None:
        """Half-close, drain queued messages, then close the transport.

        Draining requires a half-close; aborting before the drain discards
        queued messages. The final ``CANCELLED`` abort closes the socket and
        tells a relay to release the session. Each step is best effort.
        """
        with contextlib.suppress(Exception):
            self.stream.half_close()
        with contextlib.suppress(Exception):
            await self.stream.drain_outgoing_messages()
        with contextlib.suppress(Exception):
            self.stream.abort(
                Status(
                    code=StatusCode.CANCELLED,
                    message="The caller has closed this connection.",
                )
            )

    def action(self, name: str, schema: a11.ActionSchema | None = None):
        """Build a call on this connection's session and stream.

        Binds the call to this session's node map and outgoing stream.
        """
        resolved = (
            schema if schema is not None else self._registry.get_schema(name)
        )
        return (
            a11
            .Action(resolved)
            .bind_node_map(self.session.node_map)
            .bind_session(self.session)
            .bind_stream(self.stream)
        )


@contextlib.asynccontextmanager
async def open_gateway(
    url: str | None,
    *,
    registry: a11.ActionRegistry | None = None,
    allow_embedded: bool = True,
    local_config: GatewayConfig | None = None,
) -> AsyncIterator[GatewayConnection]:
    """Yield a connection to a gateway, starting one only if it must.

    Args:
        url: An explicit endpoint. When given it is the *only* candidate: if it
            cannot be reached, its error propagates without starting a local
            gateway.
        registry: Registry the gateway's reverse-dispatched tool calls run
            against.
        allow_embedded: Whether to start a local WebSocket gateway when
            nothing answers at the default endpoint. The legacy name remains
            for API compatibility.
        local_config: Tool groups and coding policy for a gateway started here.

    Yields:
        The connection, closed on exit.

    Raises:
        StatusException: When ``url`` was given and is unreachable, or when
            nothing answers and ``allow_embedded`` is false.
    """
    if url:
        connection = await GatewayConnection.connect(url, registry=registry)
        try:
            await connection.probe()
        except Exception:
            await connection.aclose()
            raise
        try:
            yield connection
        finally:
            await connection.aclose()
        return

    reachable = await _probe_default(registry)
    if reachable is not None:
        try:
            yield reachable
        finally:
            await reachable.aclose()
        return

    if not allow_embedded:
        raise Status(
            code=StatusCode.UNAVAILABLE,
            message=(
                f"No A11 gateway answered at {DEFAULT_GATEWAY_URL}. Start one"
                " with `a11 gateway start` or pass --gateway."
            ),
        ).to_exception()

    from a11.gateway import app as gateway_app

    gateway = gateway_app.init_app(local_config)
    from a11.net import http as net_http

    endpoint = net_http.parse_url(DEFAULT_GATEWAY_URL)
    options = net.WebSocketServerOptions()
    options.path = endpoint.path
    options.bind_address = endpoint.host
    options.port = endpoint.port
    options.http2_options.enable_h2 = False
    options.http2_options.enable_h2c = False
    server = net.WebSocketWireServer.create(gateway.handle_stream, options)
    local_url = f"ws://{endpoint.host}:{server.port}{endpoint.path}"
    logging.info("started a local WebSocket gateway at %s", local_url)
    try:
        connection = await GatewayConnection.connect(
            local_url, registry=registry
        )
        await connection.probe()
        try:
            yield connection
        finally:
            await connection.aclose()
    finally:
        server.stop()


async def _probe_default(
    registry: a11.ActionRegistry | None,
) -> GatewayConnection | None:
    """A live connection to the default endpoint, or None if there is none."""
    try:
        connection = await GatewayConnection.connect(
            DEFAULT_GATEWAY_URL,
            registry=registry,
            timeout=PROBE_CONNECT_TIMEOUT,
        )
    except StatusException as exc:
        logging.info(
            "no gateway at %s: %s", DEFAULT_GATEWAY_URL, exc.status.message
        )
        return None
    except Exception as exc:  # noqa: BLE001 - transport-specific failures
        logging.info("no gateway at %s: %s", DEFAULT_GATEWAY_URL, exc)
        return None

    try:
        await connection.probe()
    except Exception as exc:  # noqa: BLE001 - anything can hold the port
        # An unresponsive default endpoint does not prevent another connection.
        logging.info(
            "something is listening on %s but did not answer a ping: %s",
            DEFAULT_GATEWAY_URL,
            exc,
        )
        await connection.aclose()
        return None
    return connection


__all__ = [
    "CONNECT_TIMEOUT",
    "DEFAULT_GATEWAY_URL",
    "PROBE_CALL_TIMEOUT",
    "PROBE_CONNECT_TIMEOUT",
    "GatewayConnection",
    "open_gateway",
    "websocket_client_options",
]
