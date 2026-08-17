import asyncio

import pytest

import a11
from a11 import _native
from a11.data.types import Chunk, NodeFragment, WireMessage
from a11.net.http2 import Http2Client, Http2Server
from a11.net.signalling import (
    SignallingMessage,
    SignallingMessageType,
    SignallingService,
    WebSocketSignallingClient,
    WebSocketSignallingServer,
)
from a11.net.webrtc_wire_stream import WebRtcWireServer, WebRtcWireStream
from a11.net.websocket_wire_stream import (
    WebSocketServerOptions,
    WebSocketWireServer,
    WebSocketWireStream,
)
from a11.net.wire_stream import WireStreamWithRecv


def _message(payload: bytes) -> WireMessage:
    return WireMessage(
        node_fragments=[NodeFragment(id="native", data=Chunk(data=payload))]
    )


def test_public_network_types_are_native_classes():
    names = (
        "Http2Client",
        "Http2Server",
        "HttpProtocolPreference",
        "HttpSseClientWireStream",
        "HttpSseServer",
        "SignallingService",
        "WebRtcWireServer",
        "WebRtcWireStream",
        "WebSocketSignallingClient",
        "WebSocketSignallingServer",
        "WebSocketWireServer",
        "WebSocketWireStream",
    )
    for name in names:
        assert getattr(a11, name) is getattr(_native, name)


@pytest.mark.asyncio
async def test_nghttp2_websocket_wire_stream_exchanges_chunked_messages():
    accepted_future = asyncio.get_running_loop().create_future()

    async def on_stream(stream):
        accepted = WireStreamWithRecv(stream)
        await accepted.accept()
        accepted_future.set_result(accepted)

    options = WebSocketServerOptions()
    options.path = "/wire"
    options.framing.split_size = 1024
    server = WebSocketWireServer.create(on_stream, options)
    try:
        client_options = _native.WebSocketClientOptions()
        client_options.framing.split_size = 1024
        client_options.headers = {"x-a11-test": "native"}
        raw_client = WebSocketWireStream.connect(
            f"ws://127.0.0.1:{server.port}/wire",
            websocket_options=client_options,
        )
        client = WireStreamWithRecv(raw_client)
        await asyncio.wait_for(client.start(), timeout=5)
        accepted = await asyncio.wait_for(accepted_future, timeout=5)

        message = _message(b"native-websocket" * 32_768)
        client.send(message)
        assert await asyncio.wait_for(accepted.receive(), timeout=10) == message

        accepted.send(_message(b"response"))
        assert await asyncio.wait_for(client.receive(), timeout=5) == _message(
            b"response"
        )

        client.half_close({"client": b"done"})
        accepted.half_close({"server": b"done"})
        await asyncio.wait_for(
            asyncio.gather(
                client.drain_outgoing_messages(),
                accepted.drain_outgoing_messages(),
            ),
            timeout=5,
        )
        assert await asyncio.wait_for(client.receive(), timeout=5) is None
        assert await asyncio.wait_for(accepted.receive(), timeout=5) is None
    finally:
        server.stop()


@pytest.mark.asyncio
async def test_websocket_wire_stream_exchanges_over_http1():
    """The native WebSocket transport interoperates over RFC 6455 / HTTP/1.1."""
    accepted_future = asyncio.get_running_loop().create_future()

    async def on_stream(stream):
        accepted = WireStreamWithRecv(stream)
        await accepted.accept()
        accepted_future.set_result(accepted)

    options = WebSocketServerOptions()
    options.path = "/wire"
    server = WebSocketWireServer.create(on_stream, options)
    try:
        client_options = _native.WebSocketClientOptions()
        # Force the client onto HTTP/1.1; the cleartext server sniffs and
        # accepts the RFC 6455 upgrade over an HTTP/1.1 connection.
        client_options.http2_options.client_preference = (
            a11.HttpProtocolPreference.HTTP11
        )
        raw_client = WebSocketWireStream.connect(
            f"ws://127.0.0.1:{server.port}/wire",
            websocket_options=client_options,
        )
        client = WireStreamWithRecv(raw_client)
        await asyncio.wait_for(client.start(), timeout=5)
        accepted = await asyncio.wait_for(accepted_future, timeout=5)

        message = _message(b"http1-websocket")
        client.send(message)
        assert await asyncio.wait_for(accepted.receive(), timeout=10) == message
    finally:
        server.stop()


@pytest.mark.asyncio
async def test_http2_extended_connect_exposes_duplex_body_streams():
    async def handler(request, response):
        assert request.protocol == "echo"
        assert request.body_stream is not None
        response.send_headers(200, {"x-transport": "nghttp2"})
        async for data in request.body_stream:
            response.write(b"echo:" + data)
        response.finish()

    server = Http2Server.create(handler=handler)
    client = None
    try:
        client = await asyncio.wait_for(
            Http2Client.connect("127.0.0.1", server.port), timeout=5
        )
        stream = client.extended_connect("echo", "/duplex")
        head = await asyncio.wait_for(stream.headers(), timeout=5)
        assert head.status == 200
        assert ("x-transport", "nghttp2") in head.headers

        stream.write(b"one")
        stream.write(b"two")
        stream.finish()
        # Joined, not compared chunk by chunk: a duplex body is a byte stream, and
        # HTTP/2 DATA framing is not a message boundary. Two writes issued back to
        # back may reach the peer as one frame -- which is what happens now that a
        # write is posted to the loop rather than awaited -- and a proxy could
        # re-frame them anyway. Order and bytes are the contract; chunking is not.
        assert b"".join([chunk async for chunk in stream]) == b"echo:oneecho:two"
        await asyncio.wait_for(stream.wait_done(), timeout=5)
    finally:
        if client is not None:
            client.close()
        server.stop()


@pytest.mark.asyncio
async def test_websocket_signalling_binds_network_client_and_service():
    service = SignallingService.create()
    signalling_server = WebSocketSignallingServer.create(service)
    client = None
    received = asyncio.get_running_loop().create_future()

    async def on_message(message):
        received.set_result(message)

    endpoint = service.connect("receiver", on_message)
    try:
        client = await asyncio.wait_for(
            WebSocketSignallingClient.connect(
                f"ws://127.0.0.1:{signalling_server.port}", "client"
            ),
            timeout=5,
        )
        client.send(
            SignallingMessage(
                type=SignallingMessageType.CANDIDATE,
                recipient="receiver",
                candidate="candidate:1 1 UDP 1 127.0.0.1 1234 typ host",
                mid="0",
            )
        )
        message = await asyncio.wait_for(received, timeout=5)
        assert message.sender == "client"
        assert message.recipient == "receiver"
        assert message.mid == "0"
    finally:
        endpoint.close()
        if client is not None:
            client.close()
        signalling_server.stop()
        service.stop()


@pytest.mark.asyncio
async def test_webrtc_wire_stream_exchanges_fragmented_messages():
    signalling = SignallingService.create()
    accepted_future = asyncio.get_running_loop().create_future()

    async def on_stream(stream):
        accepted = WireStreamWithRecv(stream)
        await accepted.accept()
        accepted_future.set_result(accepted)

    server = WebRtcWireServer.create("server", signalling, on_stream)
    try:
        client = WireStreamWithRecv(
            WebRtcWireStream.create_client("client", "server", signalling)
        )
        await asyncio.wait_for(client.start(), timeout=15)
        accepted = await asyncio.wait_for(accepted_future, timeout=15)

        message = _message(b"native-webrtc" * 20_000)
        client.send(message)
        assert await asyncio.wait_for(accepted.receive(), timeout=15) == message

        accepted.send(_message(b"reply"))
        assert await asyncio.wait_for(client.receive(), timeout=15) == _message(
            b"reply"
        )

        client.half_close({"client": b"done"})
        accepted.half_close({"server": b"done"})
        await asyncio.wait_for(
            asyncio.gather(
                client.drain_outgoing_messages(),
                accepted.drain_outgoing_messages(),
            ),
            timeout=15,
        )
    finally:
        server.stop()
        signalling.stop()
