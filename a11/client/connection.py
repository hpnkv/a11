# Copyright 2026 The A11 Authors.

"""Connecting to a gateway, and deciding which gateway to connect to.

A client has three situations to tell apart, and the difference matters:

* it was given an explicit endpoint, which must work or the command must fail;
* it was given nothing and a gateway is already running, which it should join;
* it was given nothing and there is none, in which case it runs one itself, in
  process, over an in-memory stream pair.

The middle case needs a *probe*, not a connect. Anything at all can be listening
on 8011, and a TCP handshake with something that is not an A11 gateway succeeds
just as readily as one with a gateway -- after which the first real call hangs
until its deadline. So the probe completes a ``__ping`` round trip, which proves
both liveness and that the peer speaks A11.
"""

from __future__ import annotations

import asyncio
import contextlib
from collections.abc import AsyncIterator, Sequence

from absl import logging

import a11
from a11 import net, timing
from a11.gateway.config import DEFAULT_GATEWAY_URL
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
    """How this client speaks WebSocket: RFC 6455 over HTTP/1.1, always.

    Two decisions live here, both of which have cost real debugging time.

    **The protocol.** A gateway serves RFC 6455 over HTTP/1.1 and turns HTTP/2
    off, and a reverse proxy in front of one cannot carry a WebSocket over
    HTTP/2 at all -- nginx does not implement RFC 8441's extended CONNECT, so
    it answers the handshake with a bare `400` before any credential is read,
    which presents as a rejected token. `client_preference` is what decides
    this; clearing `enable_h2`/`enable_h2c` is not sufficient on its own, as a
    `wss://` connection kept offering h2 with both false.

    **The deadline.** It bounds the handshake and must not land on
    `http2_options.deadline`, which aborts the whole stream: a WebSocket is one
    long-lived HTTP request, so a deadline there hangs the session up
    mid-conversation rather than timing out a connect.
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
    ) -> GatewayConnection:
        """Open a session to the gateway at ``url``.

        Does not verify that the peer is a gateway; call `probe` for that.

        Raises:
            StatusException: When the transport cannot be established.
        """
        # The connect deadline bounds the *handshake*, and only that. It must not
        # become `WireStreamOptions.deadline`, which is the absolute time the
        # stream is aborted at: a chat session outlives its handshake by hours,
        # and a stream deadline of "now + 10s" makes every turn after the first
        # ten seconds fail with "This endpoint has already terminated".
        handshake_deadline = timing.now() + (timeout or CONNECT_TIMEOUT)
        options = net.WireStreamOptions()  # no deadline: the stream lives on
        websocket_options = websocket_client_options(handshake_deadline)
        stream = net.WebSocketWireStream.connect(
            url, options, websocket_options=websocket_options
        )
        session = Session(action_registry=registry or a11.ActionRegistry())
        await session.add_stream(stream, mode="start")
        return cls(session, stream, url=url)

    async def probe(
        self, *, timeout: timing.Duration | None = None
    ) -> None:
        """Verify the peer is a live A11 gateway, via a ``__ping`` round trip.

        Raises:
            StatusException: When the peer is not a gateway.
            TimeoutError: When it does not answer in time.
        """
        bound = (timeout or PROBE_CALL_TIMEOUT).float_seconds()
        # The *whole* round trip is bounded, not just the terminal wait. A peer
        # that accepts the connection and then says nothing -- which is what
        # something-that-is-not-a-gateway looks like -- leaves the read of
        # `output` pending forever, and a deadline on the call does not surface
        # until that read returns.
        await asyncio.wait_for(self._ping(), timeout=bound)

    async def _ping(self) -> None:
        call = self.action(PING_ACTION, PING_SCHEMA)
        await call.call()
        await call["input"].finalize("probe")
        await call["output"].consume(str)
        await call.wait(PROBE_CALL_TIMEOUT)

    async def aclose(self) -> None:
        """Finish sending, let the queue go out, then close the transport.

        Half-close, then drain, then abort, and the order is forced at each
        step. `drain_outgoing_messages` answers FAILED_PRECONDITION before a
        half-close, because there is otherwise no point at which the outgoing
        queue is final; and the abort has to come after the drain or it
        discards what was queued.

        The abort is what actually closes the socket, and leaving it out was a
        real bug rather than tidiness. A half-close says only "I have finished
        sending" -- the connection stays up, and a peer has no way to tell that
        from a caller still waiting for an answer. Against the exchange relay
        that meant a session per call which nothing ever ended, each holding a
        WebRTC leg to the agent, until the agent was swamped and answered
        nobody.

        `Status()` is OK, so this is a graceful close and not a failure
        reported to the peer: a terminal status that is OK takes the
        transport's ordinary close path, where a non-OK one would fail the
        connection.

        Every step is best effort. A peer that has already gone is not a
        problem worth reporting on the way out.
        """
        with contextlib.suppress(Exception):
            self.stream.half_close()
        with contextlib.suppress(Exception):
            await self.stream.drain_outgoing_messages()
        with contextlib.suppress(Exception):
            self.stream.abort(Status())

    def action(self, name: str, schema: a11.ActionSchema | None = None):
        """Build a call on this connection's session and stream.

        The one way to address the peer: bound to this session's node map so its
        replies route back, and to this stream so the call goes out on it.
        """
        resolved = schema if schema is not None else self._registry.get_schema(
            name
        )
        return (
            a11.Action(resolved)
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
) -> AsyncIterator[GatewayConnection]:
    """Yield a connection to a gateway, starting one only if it must.

    Args:
        url: An explicit endpoint. When given it is the *only* candidate: if it
            cannot be reached the error propagates, because an endpoint the user
            named turning silently into a local process is the worst outcome
            available -- their tools would run somewhere they did not choose.
        registry: Registry the gateway's reverse-dispatched tool calls run
            against.
        allow_embedded: Whether to fall back to an in-process gateway when
            nothing answers at the default endpoint.

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

    # Imported here so a client that never needs an embedded gateway does not
    # pay for the gateway's imports.
    from a11.gateway.embedded import embedded_gateway

    async with embedded_gateway(registry=registry) as connection:
        yield connection


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
        # Something is listening but it is not an A11 gateway (or not a healthy
        # one). Treat it as absent rather than failing the command: the user did
        # not ask for *this* endpoint specifically.
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
