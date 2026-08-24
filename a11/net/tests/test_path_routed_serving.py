# Copyright 2026 The A11 Authors.

"""Serving many endpoints on one port, and knowing which one was asked for.

A11's servers matched one exact path and handed the accepted stream over with
no record of the request, so one port served one thing. That is right for an
agent and wrong for anything that fronts several -- a gateway, a proxy, a
hosting service -- which is what `path_prefix` and `request_path` are for.

The old behaviour is unchanged and asserted here too: a server that names one
path still serves exactly that path.
"""

from __future__ import annotations

import asyncio

import pytest

from a11 import net, timing
from a11.status import StatusException

pytestmark = pytest.mark.asyncio

CONNECT = timing.Duration.seconds(5)


async def dial(url: str, options: net.WebSocketClientOptions | None = None):
    """Connect and drive the handshake to completion.

    `connect` only builds the stream; the HTTP upgrade happens when it is
    started, and the server's `on_stream` runs on this same loop -- which is
    why the start is awaited rather than blocked on.
    """
    stream = net.WebSocketWireStream.connect(
        url, net.WireStreamOptions(), websocket_options=options or client_options()
    )
    await asyncio.wait_for(
        stream.start(lambda message: None, lambda: None), timeout=5
    )
    return stream


def client_options() -> net.WebSocketClientOptions:
    options = net.WebSocketClientOptions()
    options.http2_options.enable_h2 = False
    options.http2_options.enable_h2c = False
    options.handshake_deadline = timing.now() + CONNECT
    return options


async def test_a_prefix_serves_many_endpoints_on_one_port():
    accepted: asyncio.Queue = asyncio.Queue()

    async def on_stream(stream):
        await accepted.put(stream.request_path)
        # Accepting completes the handshake the client is waiting on.
        await stream.accept(lambda message: None, lambda: None)

    options = net.WebSocketServerOptions()
    options.bind_address = "127.0.0.1"
    options.port = 0
    options.path_prefix = "/ws/"
    server = net.WebSocketWireServer.create(on_stream, options)
    try:
        for identity in ("alpha", "beta"):
            await dial(f"ws://127.0.0.1:{server.port}/ws/{identity}")
            path = await asyncio.wait_for(accepted.get(), timeout=5)
            assert path == f"/ws/{identity}"
    finally:
        server.stop()


async def test_the_prefix_itself_is_not_an_endpoint():
    async def on_stream(stream):
        del stream

    options = net.WebSocketServerOptions()
    options.bind_address = "127.0.0.1"
    options.port = 0
    options.path_prefix = "/ws/"
    server = net.WebSocketWireServer.create(on_stream, options)
    try:
        # Strictly beneath, or nothing: `/ws/` names no agent.
        with pytest.raises(StatusException):
            await dial(f"ws://127.0.0.1:{server.port}/ws/")
    finally:
        server.stop()


async def test_a_query_string_is_not_part_of_the_route():
    accepted: asyncio.Queue = asyncio.Queue()

    async def on_stream(stream):
        await accepted.put(stream.request_path)
        # Accepting completes the handshake the client is waiting on.
        await stream.accept(lambda message: None, lambda: None)

    options = net.WebSocketServerOptions()
    options.bind_address = "127.0.0.1"
    options.port = 0
    options.path_prefix = "/ws/"
    server = net.WebSocketWireServer.create(on_stream, options)
    try:
        await dial(f"ws://127.0.0.1:{server.port}/ws/gamma?trace=1")
        path = await asyncio.wait_for(accepted.get(), timeout=5)
        # Routed on the path, and the query is still there to be read.
        assert path == "/ws/gamma?trace=1"
    finally:
        server.stop()


async def test_request_headers_reach_the_accepted_stream():
    accepted: asyncio.Queue = asyncio.Queue()

    async def on_stream(stream):
        await accepted.put(dict(stream.request_headers))
        await stream.accept(lambda message: None, lambda: None)

    options = net.WebSocketServerOptions()
    options.bind_address = "127.0.0.1"
    options.port = 0
    options.path_prefix = "/ws/"
    server = net.WebSocketWireServer.create(on_stream, options)
    try:
        client = client_options()
        client.headers = {"authorization": "Bearer per-connection"}
        await dial(f"ws://127.0.0.1:{server.port}/ws/delta", client)
        headers = await asyncio.wait_for(accepted.get(), timeout=5)
        # This is what lets a server authenticate a stream rather than a port.
        assert headers.get("authorization") == "Bearer per-connection"
    finally:
        server.stop()


async def test_an_exact_path_still_serves_only_that_path():
    accepted: asyncio.Queue = asyncio.Queue()

    async def on_stream(stream):
        await accepted.put(stream.request_path)
        # Accepting completes the handshake the client is waiting on.
        await stream.accept(lambda message: None, lambda: None)

    options = net.WebSocketServerOptions()
    options.bind_address = "127.0.0.1"
    options.port = 0
    options.path = "/a11"
    server = net.WebSocketWireServer.create(on_stream, options)
    try:
        await dial(f"ws://127.0.0.1:{server.port}/a11")
        assert await asyncio.wait_for(accepted.get(), timeout=5) == "/a11"

        with pytest.raises(StatusException):
            await dial(f"ws://127.0.0.1:{server.port}/a11/extra")
    finally:
        server.stop()


async def test_a_prefix_must_be_bounded_by_slashes():
    options = net.WebSocketServerOptions()
    options.path_prefix = "ws/"
    with pytest.raises(StatusException):
        options.validate()

    options.path_prefix = "/ws"
    with pytest.raises(StatusException):
        options.validate()

    options.path_prefix = "/ws/"
    options.validate()


async def test_sse_serves_many_connect_endpoints_on_one_port():
    options = net.HttpSseOptions()
    options.connect_endpoint_prefix = "/sse/"
    options.http2_options.enable_h2 = False
    options.http2_options.enable_h2c = False

    accepted: asyncio.Queue = asyncio.Queue()

    async def on_connect(stream):
        await accepted.put(stream.request_path)
        await stream.accept(lambda message: None, lambda: None)

    server = net.HttpSseServer.create("127.0.0.1", 0, on_connect, options)
    try:
        client_options_ = net.HttpSseOptions()
        client_options_.connect_endpoint = "/sse/epsilon"
        client_options_.http2_options.enable_h2 = False
        client_options_.http2_options.enable_h2c = False
        stream = net.HttpSseClientWireStream.create(
            f"http://127.0.0.1:{server.port}", client_options_
        )
        await asyncio.wait_for(
            stream.start(lambda message: None, lambda: None), timeout=5
        )
        path = await asyncio.wait_for(accepted.get(), timeout=5)
        assert path == "/sse/epsilon"
    finally:
        server.stop()


async def test_sse_prefix_must_be_bounded_by_slashes():
    options = net.HttpSseOptions()
    options.connect_endpoint_prefix = "/sse"
    with pytest.raises(StatusException):
        options.validate()

    options.connect_endpoint_prefix = "/sse/"
    options.validate()
