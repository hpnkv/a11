# Copyright 2026 The A11 Authors.

"""SSE over HTTP/1.1, where the outbound direction needs its own connection.

An HTTP/2 SSE stream is two streams on one connection. HTTP/1.1 has no
multiplexing and an A11 client connection carries a single request, so the
never-ending event stream occupies the connect connection for good and the
outbound direction has to go somewhere else. There are two somewheres, and both
are exercised here:

* a **streamed request body** -- one extra connection for every outbound
  message, which is what a non-multiplexed connection picks whenever the server
  advertises it;
* **one POST per message** -- one connection per message, the fallback for a
  server that will not carry a long-lived request body.

Regression cover for both: forcing HTTP/1.1 used to connect and then never
answer, because the POST was queued behind the event stream on the one
connection it was allowed.
"""

from __future__ import annotations

import asyncio
import contextlib

import pytest

import a11
from a11 import net, timing


async def _shut_down(service: a11.Service, server) -> None:
    """Stop the listener and let go of the remainder.

    A session finishes when both peers have half-closed, so the server side is
    still waiting on a client that has gone; aborting is what a real shutdown
    does with that remainder rather than paying the drain timeout per test.
    """
    server.stop()
    service.abort(
        a11.Status(code=a11.StatusCode.CANCELLED, message="test over")
    )
    with contextlib.suppress(Exception):
        await asyncio.wait_for(
            service.aclose(timeout=timing.Duration.seconds(2)), timeout=5
        )


_SHOUT = a11.ActionSchema(
    name="shout",
    description="Upper-case the input.",
    inputs={
        "text": a11.ActionPortSchema(
            name="text", type="text/plain", unary=True, required=True
        )
    },
    outputs={
        "output": a11.ActionPortSchema(
            name="output", type="text/plain", unary=True
        )
    },
)


async def _shout(action: a11.Action) -> None:
    await action["output"].finalize((await action["text"].consume(str)).upper())


def _registry() -> a11.ActionRegistry:
    registry = a11.ActionRegistry()
    registry.register(_SHOUT.name, _SHOUT, _shout)
    return registry


def _http11(options: net.HttpSseOptions) -> net.HttpSseOptions:
    """Pin the connection to HTTP/1.1, both directions, no negotiation."""
    options.http2_options.enable_http1 = True
    options.http2_options.enable_h2c = False
    options.http2_options.enable_h2 = False
    return options


async def _call(session, stream, text: str) -> str:
    call = (
        a11
        .Action(_SHOUT)
        .bind_node_map(session.node_map)
        .bind_session(session)
        .bind_stream(stream)
    )
    await call.call()
    await call["text"].finalize(text)
    result = await call["output"].consume(str)
    await call.wait(timing.Duration.seconds(10))
    return result


async def _round_trip(server_options: net.HttpSseOptions) -> tuple[str, object]:
    """Serve, call once over HTTP/1.1, and report the delivery mode used."""
    service = a11.Service(action_registry=_registry())
    server = net.HttpSseServer.create(
        "127.0.0.1", 0, service.accept, server_options
    )
    try:
        stream = net.HttpSseClientWireStream.create(
            f"http://127.0.0.1:{server.port}",
            _http11(net.HttpSseOptions()),
        )
        client = a11.Session(action_registry=a11.ActionRegistry())
        await asyncio.wait_for(client.add_stream(stream, mode="start"), 15)
        delivery = stream.outbound_delivery
        result = await asyncio.wait_for(_call(client, stream, "hi"), 15)
        stream.half_close()
        await asyncio.wait_for(stream.drain_outgoing_messages(), 15)
        return result, delivery
    finally:
        await _shut_down(service, server)


@pytest.mark.asyncio
async def test_http11_prefers_a_streamed_body_and_round_trips() -> None:
    """The ordinary case: one extra connection carries all of the outbound."""
    result, delivery = await _round_trip(_http11(net.HttpSseOptions()))

    assert result == "HI"
    # Not what was asked for -- the option still defaults to POST -- but what a
    # connection that cannot multiplex is worth doing instead.
    assert delivery == net.SseOutboundDelivery.STREAM


@pytest.mark.asyncio
async def test_http11_falls_back_to_one_post_per_message() -> None:
    """A server refusing a streamed body works too, a connection per message."""
    options = _http11(net.HttpSseOptions())
    options.accept_streamed_outbound = False

    result, delivery = await _round_trip(options)

    assert result == "HI"
    assert delivery == net.SseOutboundDelivery.POST


@pytest.mark.asyncio
async def test_http11_carries_many_messages_over_a_streamed_body() -> None:
    """Several calls in a row, so the one upload connection is really reused."""
    service = a11.Service(action_registry=_registry())
    server = net.HttpSseServer.create(
        "127.0.0.1", 0, service.accept, _http11(net.HttpSseOptions())
    )
    try:
        stream = net.HttpSseClientWireStream.create(
            f"http://127.0.0.1:{server.port}", _http11(net.HttpSseOptions())
        )
        client = a11.Session(action_registry=a11.ActionRegistry())
        await asyncio.wait_for(client.add_stream(stream, mode="start"), 15)
        for index in range(5):
            assert (
                await asyncio.wait_for(_call(client, stream, f"m{index}"), 15)
                == f"M{index}"
            )
        stream.half_close()
        await asyncio.wait_for(stream.drain_outgoing_messages(), 15)
    finally:
        await _shut_down(service, server)


@pytest.mark.asyncio
async def test_http11_carries_many_messages_over_posts() -> None:
    """The same, with a connection per message rather than one for all."""
    server_options = _http11(net.HttpSseOptions())
    server_options.accept_streamed_outbound = False
    service = a11.Service(action_registry=_registry())
    server = net.HttpSseServer.create(
        "127.0.0.1", 0, service.accept, server_options
    )
    try:
        stream = net.HttpSseClientWireStream.create(
            f"http://127.0.0.1:{server.port}", _http11(net.HttpSseOptions())
        )
        client = a11.Session(action_registry=a11.ActionRegistry())
        await asyncio.wait_for(client.add_stream(stream, mode="start"), 15)
        assert stream.outbound_delivery == net.SseOutboundDelivery.POST
        for index in range(5):
            assert (
                await asyncio.wait_for(_call(client, stream, f"p{index}"), 15)
                == f"P{index}"
            )
        stream.half_close()
        await asyncio.wait_for(stream.drain_outgoing_messages(), 15)
    finally:
        await _shut_down(service, server)


@pytest.mark.asyncio
async def test_http2_still_chooses_post_delivery() -> None:
    """The multiplexed case is untouched: nothing here changes HTTP/2.

    Only the mode is asserted, not a round trip. Concurrent POST delivery over
    HTTP/2 has a pre-existing ordering flake of its own (~1 call in 20 ends
    CANCELLED), and reproducing it here would say nothing about this change.
    """
    options = net.HttpSseOptions()
    options.http2_options.enable_http1 = False
    options.http2_options.enable_h2c = True
    options.http2_options.enable_h2 = False

    service = a11.Service(action_registry=_registry())
    server = net.HttpSseServer.create("127.0.0.1", 0, service.accept, options)
    try:
        stream = net.HttpSseClientWireStream.create(
            f"http://127.0.0.1:{server.port}", options
        )
        client = a11.Session(action_registry=a11.ActionRegistry())
        await asyncio.wait_for(client.add_stream(stream, mode="start"), 15)
        assert stream.outbound_delivery == net.SseOutboundDelivery.POST
        stream.half_close()
        await asyncio.wait_for(stream.drain_outgoing_messages(), 15)
    finally:
        await _shut_down(service, server)
