# Copyright 2026 The A11 Authors.

"""Choosing and reaching a gateway.

The behaviours a user notices: an explicit endpoint that must work, a running
gateway that gets joined, and nothing-there that falls back to a local one. The
awkward case is the third: *something* can be listening on the port without being
a gateway, and a probe that only completes a TCP handshake would join it and then
hang on the first real call.
"""

from __future__ import annotations

import asyncio
import contextlib

import pytest

import a11
from a11 import net, timing
from a11.client.connection import (
    DEFAULT_GATEWAY_URL,
    GatewayConnection,
    open_gateway,
)
from a11.gateway import app as gateway_app
from a11.gateway import config, conversations
from a11.service.session import Session
from a11.status import Status, StatusCode, StatusException


@contextlib.contextmanager
def _serving_gateway(tmp_path, port: int = 0):
    """A real gateway on a real WebSocket, built inside the running loop."""
    settings = config.GatewayConfig(
        a11_port=port,
        conversation_store_root=tmp_path,
        shell_tools=False,
        audio_capture=False,
        speech_recognition=False,
    )
    store = conversations.ConversationStore(tmp_path)
    registry = gateway_app._make_action_registry(settings, store)
    gateway = gateway_app.A11Gateway(store, registry)

    options = net.WebSocketServerOptions()
    options.path = config.DEFAULT_PATH
    options.bind_address = settings.host
    options.port = port
    options.http2_options.enable_h2 = False
    options.http2_options.enable_h2c = False
    server = net.WebSocketWireServer.create(gateway.handle_stream, options)
    try:
        yield server
    finally:
        server.stop()


@pytest.mark.asyncio
async def test_an_explicit_gateway_is_probed_and_joined(tmp_path):
    with _serving_gateway(tmp_path) as server:
        url = f"ws://127.0.0.1:{server.port}{config.DEFAULT_PATH}"
        async with open_gateway(url) as connection:
            assert not connection.embedded
            assert connection.url == url
            assert connection.description == url
            # The probe already proved the peer speaks A11; a second one shows
            # the connection is reusable rather than single-shot.
            await connection.probe()


@pytest.mark.asyncio
async def test_an_unreachable_explicit_gateway_fails_rather_than_falling_back():
    """The worst outcome would be silently running the user's tools locally."""
    with pytest.raises(StatusException):
        async with open_gateway("ws://127.0.0.1:9/a11"):
            pass


@pytest.mark.asyncio
async def test_something_that_is_not_a_gateway_is_treated_as_absent(tmp_path):
    """A bare TCP listener answers the handshake but not a ping.

    A connect-only probe would join it and hang on the first turn, which is the
    failure this test exists to prevent.
    """
    server = await asyncio.start_server(
        lambda reader, writer: None, "127.0.0.1", 0
    )
    port = server.sockets[0].getsockname()[1]
    try:
        url = f"ws://127.0.0.1:{port}{config.DEFAULT_PATH}"
        # A short timeout, because the answer here is "no gateway" and a user
        # waiting on the fallback path should not wait long for it.
        short = timing.Duration.milliseconds(500)
        connection = None
        try:
            connection = await GatewayConnection.connect(url, timeout=short)
        except StatusException:
            # Failing to complete the handshake is an acceptable outcome, and
            # the usual one: the peer never answers it. What must not happen is
            # a *successful* probe.
            return
        with pytest.raises((StatusException, TimeoutError, asyncio.TimeoutError)):
            await connection.probe(timeout=short)
        await connection.aclose()
    finally:
        # Not wait_closed(): the native client still holds its socket open, and
        # waiting for every connection to drain would never return.
        server.close()


@pytest.mark.asyncio
async def test_no_gateway_anywhere_falls_back_to_an_embedded_one(monkeypatch):
    # Point the default endpoint at a port nothing listens on, so the fallback
    # path is taken without depending on whether the developer happens to have a
    # gateway running.
    monkeypatch.setattr(
        "a11.client.connection.DEFAULT_GATEWAY_URL", "ws://127.0.0.1:9/a11"
    )
    async with open_gateway(None) as connection:
        assert connection.embedded
        assert connection.description == "in-process gateway"
        # The embedded gateway is a real gateway: it answers a ping.
        await connection.probe()


@pytest.mark.asyncio
async def test_the_fallback_can_be_refused(monkeypatch):
    monkeypatch.setattr(
        "a11.client.connection.DEFAULT_GATEWAY_URL", "ws://127.0.0.1:9/a11"
    )
    with pytest.raises(StatusException) as caught:
        async with open_gateway(None, allow_embedded=False):
            pass
    assert caught.value.status.code == StatusCode.UNAVAILABLE
    # The message says what to do about it.
    assert "a11 gateway start" in caught.value.status.message


def test_the_default_endpoint_matches_the_gateways_own_default():
    # The plugin's setting, the gateway's bind address and the client's fallback
    # all have to be the same string, or "no gateway" becomes a lie.
    assert DEFAULT_GATEWAY_URL == config.GatewayConfig().url
    assert DEFAULT_GATEWAY_URL == "ws://127.0.0.1:8011/a11"


@pytest.mark.asyncio
async def test_aclose_closes_the_transport_not_only_the_sending_half():
    """A hang-up the peer can see.

    A half-close alone says "I have finished sending" and leaves the
    connection up, which a server cannot distinguish from a caller still
    waiting for its answer. The exchange relay held a session open per call for
    exactly that reason. So `aclose` closes the transport, and the peer's
    stream reaches a terminal state.
    """
    accepted: asyncio.Queue = asyncio.Queue()
    finished = asyncio.Event()

    async def on_stream(stream):
        await accepted.put(stream)
        await stream.accept(lambda message: None, finished.set)

    options = net.WebSocketServerOptions()
    options.bind_address = "127.0.0.1"
    options.port = 0
    server = net.WebSocketWireServer.create(on_stream, options)
    try:
        connection = await GatewayConnection.connect(
            f"ws://127.0.0.1:{server.port}/a11",
            timeout=timing.Duration.seconds(5),
        )
        server_side = await asyncio.wait_for(accepted.get(), timeout=5)

        await connection.aclose()

        # The server sees end-of-input either way; what the abort adds is that
        # the transport itself goes, which is what a relay needs in order to
        # stop holding resources for a caller that has left.
        await asyncio.wait_for(finished.wait(), timeout=5)
        for _ in range(50):
            if server_side.get_status().code != StatusCode.OK:
                break
            await asyncio.sleep(0.1)
        assert server_side.get_status().code != StatusCode.OK, (
            "the peer's stream never reached a terminal state"
        )
    finally:
        server.stop()


@pytest.mark.asyncio
async def test_aclose_aborts_with_a_status_the_transport_accepts():
    """`WireStream.abort` refuses an OK status, and `aclose` passed one.

    Inside a `contextlib.suppress`, so the INVALID_ARGUMENT went nowhere: the
    abort never happened, the socket stayed open, and the exchange relay went
    on holding a session and a WebRTC leg for a caller that had gone -- which
    is the leak the abort was added to prevent.

    Asserted on the status handed to the transport rather than on what the
    peer sees, because the transport is the thing that refused it.
    """
    first, second = net.InProcessWireStream.create_pair()
    with pytest.raises(StatusException) as refused:
        first.abort(Status())
    assert refused.value.status.code == StatusCode.INVALID_ARGUMENT
    del second

    aborted: list[Status] = []

    class _Recording:
        def half_close(self):
            return None

        async def drain_outgoing_messages(self):
            return None

        def abort(self, status):
            if status.is_ok():
                raise Status(
                    code=StatusCode.INVALID_ARGUMENT,
                    message="Abort status must be non-OK",
                ).to_exception()
            aborted.append(status)

    connection = GatewayConnection(
        Session(action_registry=a11.ActionRegistry()), _Recording()
    )

    await connection.aclose()

    assert len(aborted) == 1, "aclose must abort, and with a status that lands"
    assert not aborted[0].is_ok()
