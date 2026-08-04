"""The Python-facing protocol for A11's native wire streams.

A `WireStream` is A11's transport abstraction: a bidirectional,
message-oriented channel that carries
[WireMessage][a11.data.types.WireMessage] values between
two endpoints. Delivery is **unordered** -- the transport makes no promise that
messages arrive in the order they were sent -- with a single guarantee that
matters: it is **synchronised on closure**, so a reader observes every delivered
message before the stream reports done, and a half-close marker follows messages
already queued by that endpoint. `half_close` queues that transition;
`drain_outgoing_messages` is the explicit delivery barrier. When you need
ordering, impose it above the transport (an
[AsyncNode][a11.nodes.async_node.AsyncNode] /
[ChunkStore][a11.stores.chunk_store.ChunkStore] log *is* ordered, by sequence
number). Everything above it -- ``AsyncNode``
mirroring, [Session][a11.service.session.Session] multiplexing, remote action
dispatch -- is written against this one interface, so the concrete transport is
a pluggable detail. A11 ships several implementations (in-process,
WebSocket/HTTP2, HTTP SSE, WebRTC), and `WireStream` is a deliberate
**extension point**: implement it to carry A11 traffic over a transport of your
own.

The classes exported here are the native ``a11._native`` wire-stream types; this
module attaches the async context-manager protocol (draining outgoing messages
on exit) via [attach_protocol][a11._native_protocol.attach_protocol].
"""

from __future__ import annotations

from collections.abc import Awaitable, Callable
from typing import Self

from a11 import _native, timing
from a11._native_options import install_native_options
from a11._native_protocol import attach_protocol
from a11.data import types

OnMessage = Callable[[types.WireMessage | None], Awaitable[None]]
OnDone = Callable[[], Awaitable[None]]

MAX_SINGLE_MESSAGE_SIZE = _native.WIRE_STREAM_MAX_SINGLE_MESSAGE_SIZE
ABORT_STATUS_HEADER = _native.WIRE_STREAM_ABORT_STATUS_HEADER

from a11._native import WireStreamOptions

install_native_options(
    WireStreamOptions,
    {
        "max_buffered_incoming_messages": (int, 100),
        "max_single_message_size": (int, MAX_SINGLE_MESSAGE_SIZE),
        "max_buffered_incoming_bytes": (int, 32 * 1024 * 1024),
        "message_timeout_millis": (
            timing.Duration | float | int | None,
            timing.infinite_duration(),
        ),
        "deadline": (timing.Time | None, timing.infinite_future()),
    },
)
from a11._native import WireStream
from a11._native import WireStreamWithRecv


class _WireStreamProtocol:
    """Async context-manager protocol shared by every wire stream.

    Entering yields the stream; a clean exit calls
    `drain_outgoing_messages`. Request `half_close` inside the block first:
    draining is a synchronization step after half-close, not an implicit close.
    Use `abort` for a failed exchange.
    """

    async def __aenter__(self) -> Self:
        return self

    async def __aexit__(self, exc_type, exc, traceback) -> None:
        del exc_type, exc, traceback
        await self.drain_outgoing_messages()


attach_protocol(WireStream, _WireStreamProtocol)

for _class in (WireStream, WireStreamOptions, WireStreamWithRecv):
    _class.__module__ = __name__


__all__ = [
    "ABORT_STATUS_HEADER",
    "MAX_SINGLE_MESSAGE_SIZE",
    "OnDone",
    "OnMessage",
    "WireStream",
    "WireStreamOptions",
    "WireStreamWithRecv",
]
