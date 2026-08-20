# Copyright 2026 The A11 Authors.

"""A service: sessions without a server, and a lifecycle of its own.

What matters is the seam. A service must serve a stream that arrived from
anywhere, let a connection be specialised before it starts pumping, and be
drainable -- and its Python connection hook must be able to await native work
without deadlocking the libuv loop it is called from.
"""

from __future__ import annotations

import asyncio
import contextlib

import pytest

import a11
from a11 import net, timing
from a11.service.serving import serving, websocket
from a11.status import StatusCode, StatusException

_ECHO = a11.ActionSchema(
    name="echo",
    description="Echo the input back.",
    inputs={
        "text": a11.ActionPortSchema(
            name="text", type="text/plain", unary=True, required=True
        )
    },
    outputs={"out": a11.ActionPortSchema(name="out", type="text/plain")},
)


async def _echo(action: a11.Action) -> None:
    text = await action["text"].consume(str)
    await action["out"].finalize(text)


def _registry() -> a11.ActionRegistry:
    registry = a11.ActionRegistry()
    registry.register(_ECHO.name, _ECHO, _echo)
    return registry


async def _close(stream) -> None:
    """Half-close, then drain. A stream is not a node, and the order matters:
    draining before a half-close answers FAILED_PRECONDITION."""
    stream.half_close()
    await stream.drain_outgoing_messages()


async def _finish(service, stream, serving_task) -> None:
    """Shut a connection down the way a real shutdown does.

    A session finishes when *both* peers have half-closed, so closing the client
    alone leaves the server side waiting -- for its no-stream timeout, thirty
    seconds away. Aborting the service is what a real shutdown does with the
    remainder, and it is why `embedded_gateway` gives its own server a short
    grace and then stops waiting.
    """
    await _close(stream)
    service.abort(a11.Status(code=StatusCode.CANCELLED, message="test over"))
    with contextlib.suppress(Exception):
        await asyncio.wait_for(serving_task, timeout=10)


async def _call_echo(session, stream, text: str) -> str:
    call = (
        a11.Action(_ECHO)
        .bind_node_map(session.node_map)
        .bind_session(session)
        .bind_stream(stream)
    )
    await call.call()
    await call["text"].finalize(text)
    result = await call["out"].consume(str)
    await call.wait(timing.Duration.seconds(10))
    return result


@pytest.mark.asyncio
async def test_a_service_needs_no_server_at_all():
    """An in-process stream pair is a perfectly good connection."""
    service = a11.Service(action_registry=_registry())
    server_stream, client_stream = net.create_in_process_wire_stream_pair()
    serving_task = asyncio.ensure_future(service.accept(server_stream))

    client = a11.Session(action_registry=a11.ActionRegistry())
    await client.add_stream(client_stream, mode="start")
    assert await _call_echo(client, client_stream, "hello") == "hello"
    assert service.session_count == 1
    assert len(service.session_ids()) == 1

    await _finish(service, client_stream, serving_task)


@pytest.mark.asyncio
async def test_the_connection_hook_runs_before_the_first_message():
    """The hook's whole purpose: specialise a connection without a race.

    It registers an action the client immediately calls. If the hook ran after
    the session started pumping, that call could arrive first and find nothing.
    """
    prepared: list[str] = []

    async def on_connection(session, stream) -> None:
        registry = _registry()
        session.set_action_registry(registry)
        prepared.append(session.get_id())
        # An await on native work: the hook is called from a fiber and must be
        # able to do this without deadlocking the loop it came from.
        await asyncio.sleep(0)

    service = a11.Service(on_connection=on_connection)
    server_stream, client_stream = net.create_in_process_wire_stream_pair()
    serving_task = asyncio.ensure_future(service.accept(server_stream))

    client = a11.Session(action_registry=a11.ActionRegistry())
    await client.add_stream(client_stream, mode="start")
    # The service itself was built with an *empty* registry; only the hook put
    # `echo` on this connection.
    assert await _call_echo(client, client_stream, "prepared") == "prepared"
    assert len(prepared) == 1

    await _finish(service, client_stream, serving_task)


@pytest.mark.asyncio
async def test_a_rejecting_hook_refuses_the_connection():
    async def on_connection(session, stream) -> None:
        raise a11.Status(
            code=StatusCode.PERMISSION_DENIED, message="not for you"
        ).to_exception()

    service = a11.Service(on_connection=on_connection)
    server_stream, _client_stream = net.create_in_process_wire_stream_pair()

    with pytest.raises(StatusException) as caught:
        await service.accept(server_stream)
    assert caught.value.status.code == StatusCode.PERMISSION_DENIED
    # A refused connection leaves nothing behind to drain.
    assert service.session_count == 0


@pytest.mark.asyncio
async def test_stop_accepting_refuses_new_connections_and_drain_returns():
    service = a11.Service(action_registry=_registry())
    service.stop_accepting()
    assert not service.accepting

    server_stream, _client = net.create_in_process_wire_stream_pair()
    with pytest.raises(StatusException) as caught:
        await service.accept(server_stream)
    assert caught.value.status.code == StatusCode.FAILED_PRECONDITION

    # Nothing in flight, so draining is immediate.
    await service.drain(timing.Duration.seconds(5))


@pytest.mark.asyncio
async def test_a_registry_swap_does_not_interrupt_a_live_stream():
    """The headline guarantee: reconfigure a running service, break nothing."""
    service = a11.Service(action_registry=_registry())
    server_stream, client_stream = net.create_in_process_wire_stream_pair()
    serving_task = asyncio.ensure_future(service.accept(server_stream))

    client = a11.Session(action_registry=a11.ActionRegistry())
    await client.add_stream(client_stream, mode="start")
    assert await _call_echo(client, client_stream, "before") == "before"

    replacement = _registry()
    service.set_action_registry(replacement)
    assert service.action_registry is replacement
    # The same stream keeps working across the swap.
    assert await _call_echo(client, client_stream, "after") == "after"

    await _finish(service, client_stream, serving_task)


@pytest.mark.asyncio
async def test_serving_binds_a_listener_and_stops_it_on_the_way_out():
    service = a11.Service(action_registry=_registry())
    options = net.WebSocketServerOptions()
    options.path = "/svc"
    options.bind_address = "127.0.0.1"
    options.port = 0
    options.http2_options.enable_h2 = False
    options.http2_options.enable_h2c = False

    async with serving(service, websocket(options)) as listeners:
        assert len(listeners) == 1
        port = listeners[0].port
        assert port != 0
        stream = net.WebSocketWireStream.connect(
            f"ws://127.0.0.1:{port}/svc",
            websocket_options=_client_options(),
        )
        client = a11.Session(action_registry=a11.ActionRegistry())
        await client.add_stream(stream, mode="start")
        assert await _call_echo(client, stream, "over-ws") == "over-ws"
        await _close(stream)

    # Left the block: no longer accepting, and the listener is stopped.
    assert not service.accepting


def _client_options() -> net.WebSocketClientOptions:
    options = net.WebSocketClientOptions()
    options.http2_options.enable_h2 = False
    options.http2_options.enable_h2c = False
    return options


@pytest.mark.asyncio
async def test_one_service_serves_two_listeners_at_once():
    """One service, two endpoints, one registry and one lifecycle."""
    service = a11.Service(action_registry=_registry())

    def options(path: str) -> net.WebSocketServerOptions:
        built = net.WebSocketServerOptions()
        built.path = path
        built.bind_address = "127.0.0.1"
        built.port = 0
        built.http2_options.enable_h2 = False
        built.http2_options.enable_h2c = False
        return built

    first, second = options("/one"), options("/two")
    async with serving(
        service, websocket(first), websocket(second)
    ) as listeners:
        for listener, path in zip(listeners, ("/one", "/two")):
            stream = net.WebSocketWireStream.connect(
                f"ws://127.0.0.1:{listener.port}{path}",
                websocket_options=_client_options(),
            )
            client = a11.Session(action_registry=a11.ActionRegistry())
            await client.add_stream(stream, mode="start")
            assert await _call_echo(client, stream, path) == path
            await _close(stream)


def test_service_options_are_keyword_constructible():
    options = a11.ServiceOptions(
        copy_registry_per_connection=True,
        drain_timeout=timing.Duration.seconds(2),
    )
    assert options.copy_registry_per_connection
    assert options.drain_timeout == timing.Duration.seconds(2)
    options.validate()
