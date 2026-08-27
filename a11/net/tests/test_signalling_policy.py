# Copyright 2026 The A11 Authors.

"""Admission, interception and federation on the WebSocket signalling server.

A signalling server that anyone may register any identity on is fine in a
process and not fine on the internet, so the server grew four hooks: who may
register, what happens to each message, where to look when the recipient is
elsewhere, and who left. They are the whole of A11's opinion about policy --
the decisions stay with whoever deploys it.

The tests use A11's own signalling client, because the point of the hooks is
that an ordinary client reaches a policed server without knowing it is policed.
"""

from __future__ import annotations

import asyncio

import pytest

from a11 import net, timing
from a11.status import Status, StatusCode, StatusException

pytestmark = pytest.mark.asyncio

CONNECT_TIMEOUT = timing.Duration.seconds(5)


def server_options(**kwargs) -> net.WebSocketSignallingServerOptions:
    options = net.WebSocketSignallingServerOptions()
    options.bind_address = "127.0.0.1"
    options.port = 0
    options.path_prefix = "/ice/"
    for name, value in kwargs.items():
        setattr(options, name, value)
    return options


def client_options(
    headers: dict[str, str] | None = None,
) -> net.WebSocketSignallingClientOptions:
    # The shipped factory, not a hand-rolled copy of it: every test below then
    # connects the way a real host does.
    options = net.signalling.client_options()
    options.deadline = timing.now() + CONNECT_TIMEOUT
    if headers:
        options.headers = headers
    return options


async def connect(
    port: int,
    identity: str,
    *,
    headers: dict[str, str] | None = None,
    on_message=None,
):
    """Connect A11's signalling client to a server on ``port``."""
    return await net.WebSocketSignallingClient.connect(
        f"ws://127.0.0.1:{port}/ice",
        identity,
        on_message,
        client_options(headers),
    )


async def test_admission_sees_the_identity_headers_and_query():
    seen: list[net.SignallingAdmission] = []

    async def admit(admission):
        seen.append(admission)

    service = net.SignallingService.create()
    server = net.WebSocketSignallingServer.create(
        service, server_options(on_admit=admit)
    )
    try:
        client = await connect(
            server.port, "host-a", headers={"x-a11-claim": "a-claim-token"}
        )
        client.close()
    finally:
        server.stop()

    assert len(seen) == 1
    assert seen[0].identity == "host-a"
    assert seen[0].path.startswith("/ice/host-a")
    assert ("x-a11-claim", "a-claim-token") in seen[0].headers


async def test_a_refused_admission_stops_the_connection():
    async def admit(admission):
        raise Status(
            code=StatusCode.PERMISSION_DENIED,
            message=f"{admission.identity} may not register here",
        ).to_exception()

    service = net.SignallingService.create()
    server = net.WebSocketSignallingServer.create(
        service, server_options(on_admit=admit)
    )
    try:
        with pytest.raises(Exception):
            await connect(server.port, "intruder")
        # And nothing was registered, so a refusal leaves no trace to clean up.
        assert "intruder" not in service.identities()
    finally:
        server.stop()


async def test_headers_are_how_a_client_authenticates():
    async def admit(admission):
        presented = dict(admission.headers)
        if presented.get("authorization") != "Bearer good-key":
            raise Status(
                code=StatusCode.UNAUTHENTICATED, message="bad key"
            ).to_exception()

    service = net.SignallingService.create()
    server = net.WebSocketSignallingServer.create(
        service, server_options(on_admit=admit)
    )
    try:
        client = await connect(
            server.port,
            "authenticated",
            headers={"authorization": "Bearer good-key"},
        )
        assert client.connected()
        client.close()

        with pytest.raises(Exception):
            await connect(
                server.port,
                "impostor",
                headers={"authorization": "Bearer wrong-key"},
            )
    finally:
        server.stop()


async def test_departure_is_reported():
    departed: list[str] = []

    service = net.SignallingService.create()
    server = net.WebSocketSignallingServer.create(
        service, server_options(on_departed=departed.append)
    )
    try:
        client = await connect(server.port, "transient")
        client.close()
        for _ in range(100):
            if departed:
                break
            await asyncio.sleep(0.02)
    finally:
        server.stop()

    assert departed == ["transient"]


async def test_a_message_hook_can_rewrite_what_is_routed():
    def rewrite(message):
        message.description = message.description.replace("ORIGINAL", "REWROTE")

    service = net.SignallingService.create()
    server = net.WebSocketSignallingServer.create(
        service, server_options(on_message=rewrite)
    )
    received: asyncio.Queue = asyncio.Queue()

    async def on_message(message):
        await received.put(message)

    try:
        receiver = await connect(server.port, "receiver", on_message=on_message)
        sender = await connect(server.port, "sender")

        outgoing = net.SignallingMessage(
            type=net.SignallingMessageType.DESCRIPTION,
            recipient="receiver",
            description="v=0 ORIGINAL",
            description_type="offer",
        )
        sender.send(outgoing)

        arrived = await asyncio.wait_for(received.get(), timeout=5)
        assert arrived.description == "v=0 REWROTE"
        assert arrived.sender == "sender"

        sender.close()
        receiver.close()
    finally:
        server.stop()


async def test_a_refused_message_is_reported_and_the_connection_survives():
    def refuse(message):
        if message.description_type == "offer":
            raise Status(
                code=StatusCode.RESOURCE_EXHAUSTED,
                message="too many offers",
            ).to_exception()

    service = net.SignallingService.create()
    server = net.WebSocketSignallingServer.create(
        service, server_options(on_message=refuse)
    )
    errors: asyncio.Queue = asyncio.Queue()

    async def on_message(message):
        await errors.put(message)

    try:
        await connect(server.port, "receiver")
        sender = await connect(server.port, "sender", on_message=on_message)

        sender.send(
            net.SignallingMessage(
                type=net.SignallingMessageType.DESCRIPTION,
                recipient="receiver",
                description="v=0 blocked",
                description_type="offer",
            )
        )

        report = await asyncio.wait_for(errors.get(), timeout=5)
        assert report.type == net.SignallingMessageType.ERROR
        assert report.error.code == StatusCode.RESOURCE_EXHAUSTED
        # Refusing one message must not cost the sender its socket -- rate
        # limiting a busy host should slow it, not disconnect it.
        assert sender.connected()
        sender.close()
    finally:
        server.stop()


async def test_an_absent_recipient_is_reported_not_fatal():
    service = net.SignallingService.create()
    server = net.WebSocketSignallingServer.create(service, server_options())
    errors: asyncio.Queue = asyncio.Queue()

    async def on_message(message):
        await errors.put(message)

    try:
        sender = await connect(server.port, "sender", on_message=on_message)
        sender.send(
            net.SignallingMessage(
                type=net.SignallingMessageType.CANDIDATE,
                recipient="nobody-is-here",
                candidate="candidate:1 1 udp 1 127.0.0.1 9 typ host",
                mid="0",
            )
        )

        report = await asyncio.wait_for(errors.get(), timeout=5)
        assert report.type == net.SignallingMessageType.ERROR
        assert report.error.code == StatusCode.NOT_FOUND
        # Addressing a peer that has gone is an ordinary thing to do during a
        # reconnect, and it must not tear down the sender.
        assert sender.connected()
        sender.close()
    finally:
        server.stop()


async def test_an_unroutable_message_can_be_carried_elsewhere():
    """Two servers, one fabric: on_unroutable out, deliver in."""
    left = net.SignallingService.create()
    right = net.SignallingService.create()

    def forward(message):
        # In a deployment this is a publish; here the other service is simply
        # in the same process.
        right.deliver(message)

    left_server = net.WebSocketSignallingServer.create(
        left, server_options(on_unroutable=forward)
    )
    right_server = net.WebSocketSignallingServer.create(right, server_options())
    received: asyncio.Queue = asyncio.Queue()

    async def on_message(message):
        await received.put(message)

    try:
        sender = await connect(left_server.port, "sender")
        receiver = await connect(
            right_server.port, "far-away", on_message=on_message
        )

        sender.send(
            net.SignallingMessage(
                type=net.SignallingMessageType.DESCRIPTION,
                recipient="far-away",
                description="v=0 across",
                description_type="offer",
            )
        )

        arrived = await asyncio.wait_for(received.get(), timeout=5)
        assert arrived.description == "v=0 across"
        assert arrived.sender == "sender"

        sender.close()
        receiver.close()
    finally:
        right_server.stop()
        left_server.stop()


async def test_deliver_reports_an_unknown_recipient():
    service = net.SignallingService.create()
    with pytest.raises(StatusException) as caught:
        service.deliver(
            net.SignallingMessage(
                type=net.SignallingMessageType.CANDIDATE,
                sender="somewhere",
                recipient="not-here",
                candidate="candidate:1 1 udp 1 127.0.0.1 9 typ host",
                mid="0",
            )
        )
    assert caught.value.status.code == StatusCode.NOT_FOUND


async def test_replace_existing_lets_a_restarted_host_take_its_name_back():
    service = net.SignallingService.create()
    server = net.WebSocketSignallingServer.create(
        service, server_options(replace_existing=True)
    )
    try:
        first = await connect(server.port, "restarting")
        assert first.connected()

        # The first connection is still open, as it would be if its host had
        # crashed without closing the socket. Without replace_existing this is
        # ALREADY_EXISTS and the identity is unusable until somebody notices.
        second = await connect(server.port, "restarting")
        assert second.connected()

        received: asyncio.Queue = asyncio.Queue()

        async def on_message(message):
            await received.put(message)

        second.set_on_message(on_message)
        other = await connect(server.port, "caller")
        other.send(
            net.SignallingMessage(
                type=net.SignallingMessageType.DESCRIPTION,
                recipient="restarting",
                description="v=0 for the new one",
                description_type="offer",
            )
        )
        arrived = await asyncio.wait_for(received.get(), timeout=5)
        assert arrived.description == "v=0 for the new one"

        other.close()
        second.close()
    finally:
        server.stop()


async def test_without_replace_existing_a_second_registration_is_refused():
    service = net.SignallingService.create()
    server = net.WebSocketSignallingServer.create(service, server_options())
    try:
        first = await connect(server.port, "sole")
        assert first.connected()

        with pytest.raises(Exception):
            await connect(server.port, "sole")

        first.close()
    finally:
        server.stop()


async def test_the_connect_deadline_does_not_expire_the_registration():
    """A handshake deadline must not become the socket's lifetime.

    It did: the deadline was folded into the HTTP/2 stream's own deadline, and
    a WebSocket is one long-lived HTTP/2 request, so a host asking to connect
    within N seconds got a signalling socket that hung up N seconds after it
    opened. Hosted agents went unreachable a moment after appearing.
    """
    received: asyncio.Queue = asyncio.Queue()

    async def on_message(message):
        await received.put(message)

    service = net.SignallingService.create()
    server = net.WebSocketSignallingServer.create(service, server_options())
    try:
        options = net.WebSocketSignallingClientOptions()
        options.http2_options.enable_h2 = False
        options.http2_options.enable_h2c = False
        # The connection must remain usable after this deadline passes.
        options.deadline = timing.now() + timing.Duration.seconds(1)
        host = await net.WebSocketSignallingClient.connect(
            f"ws://127.0.0.1:{server.port}/ice",
            "long-lived",
            on_message,
            options,
        )
        peer = await connect(server.port, "peer")

        await asyncio.sleep(2.0)

        assert host.connected()
        assert "long-lived" in service.identities()
        peer.send(
            net.SignallingMessage(
                type=net.SignallingMessageType.DESCRIPTION,
                recipient="long-lived",
                description="v=0 well after the deadline",
                description_type="offer",
            )
        )
        arrived = await asyncio.wait_for(received.get(), timeout=5)
        assert arrived.description == "v=0 well after the deadline"

        peer.close()
        host.close()
    finally:
        server.stop()


async def test_client_options_ask_for_http_1_1():
    """Use HTTP/1.1 for WebSockets routed through a reverse proxy.

    nginx and its peers do not implement RFC 8441's extended CONNECT, so a
    signalling client that prefers HTTP/2 receives a bare `400` when the server
    sits behind an ingress.
    """
    options = net.signalling.client_options()

    # The explicit preference prevents the client from offering h2.
    assert (
        options.http2_options.client_preference
        == net.HttpProtocolPreference.HTTP11
    )
    assert not options.http2_options.enable_h2
    assert not options.http2_options.enable_h2c
