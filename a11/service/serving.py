# Copyright 2026 The A11 Authors.

"""Binding a `Service` to listeners, and taking them down again.

The application-level counterpart to
[a11.service.service][a11.service.service]: a service knows how to serve a
stream, a listener knows how to produce one, and this is the sentence that says
which listeners a given service is exposed on.

Examples:
    A gateway on WebSocket and SSE at once, shut down together:

    ```python
    async with serving(
        gateway.service,
        websocket(ws_options),
        http_sse("127.0.0.1", 8080),
    ) as listeners:
        await stop.wait()
    ```

    A service with no listener at all -- the embedded case:

    ```python
    async with serving(service) as _:
        await service.accept(server_stream)
    ```
"""

from __future__ import annotations

import contextlib
from collections.abc import AsyncIterator, Callable
from typing import Any

from absl import logging

from a11 import net, timing
from a11.service.service import Service

#: Something that binds a service to a transport and returns the live listener.
#: The listener must expose ``stop()``; every A11 server does.
Listener = Callable[[Service], Any]


def websocket(options: net.WebSocketServerOptions) -> Listener:
    """A WebSocket listener bound to the service."""

    def bind(service: Service):
        return net.WebSocketWireServer.create(service.accept, options)

    return bind


def http_sse(
    bind_address: str,
    port: int,
    options: net.HttpSseOptions | None = None,
) -> Listener:
    """An HTTP SSE listener bound to the service."""

    def bind(service: Service):
        return net.HttpSseServer.create(
            bind_address, port, service.accept, options or net.HttpSseOptions()
        )

    return bind


def webrtc(
    signalling: net.SignallingTransport,
    configuration: net.WebRtcConfiguration | None = None,
    stream_options: net.WireStreamOptions | None = None,
) -> Listener:
    """A WebRTC listener bound to the service.

    Unlike the HTTP listeners this one does not open a port: peers find it
    through ``signalling``, and the server listens as whatever identity that
    transport registered under. Pass a
    [`WebSocketSignallingClient`][a11.net.signalling.WebSocketSignallingClient]
    connected to a signalling server to be reachable through it, or an endpoint
    of an in-process
    [`SignallingService`][a11.net.signalling.SignallingService] to be reachable
    within this process.

    The server closes the transport when it is stopped, which `serving` does on
    the way out along with every other listener.
    """

    def bind(service: Service):
        return net.WebRtcWireServer.create(
            signalling,
            service.accept,
            configuration or net.WebRtcConfiguration(),
            stream_options or net.WireStreamOptions(),
        )

    return bind


@contextlib.asynccontextmanager
async def serving(
    service: Service,
    *listeners: Listener,
    drain_timeout: timing.Duration | None = None,
) -> AsyncIterator[list[Any]]:
    """Bind ``listeners`` to ``service``, yield them, then shut everything down.

    Teardown is deliberately ordered: listeners are stopped first, in reverse, so
    that no new connection can arrive while the service is draining the ones it
    already has.

    Args:
        service: The service to expose.
        *listeners: Listener factories, e.g. `websocket`. None is valid -- a
            service with no listener still serves streams handed to it directly.
        drain_timeout: How long to wait for live sessions on the way out.

    Yields:
        The live listeners, in the order given.
    """
    started: list[Any] = []
    try:
        for listener in listeners:
            started.append(listener(service))
        yield started
    finally:
        for live in reversed(started):
            try:
                live.stop()
            except Exception:  # noqa: BLE001 - shutdown is best effort
                logging.debug("a listener failed to stop", exc_info=True)
        await service.aclose(timeout=drain_timeout)


__all__ = ["Listener", "http_sse", "serving", "webrtc", "websocket"]
