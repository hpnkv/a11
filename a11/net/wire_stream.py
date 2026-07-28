"""Async Python protocols attached to A11's native wire streams."""

from __future__ import annotations

from collections.abc import Awaitable, Callable
from typing import Self

from a11 import _native, timing
from a11._native_options import install_native_options
from a11.data import types

OnMessage = Callable[[types.WireMessage | None], Awaitable[None]]
OnDone = Callable[[], Awaitable[None]]

MAX_SINGLE_MESSAGE_SIZE = _native.WIRE_STREAM_MAX_SINGLE_MESSAGE_SIZE
ABORT_STATUS_HEADER = _native.WIRE_STREAM_ABORT_STATUS_HEADER

WireStreamOptions = install_native_options(
    _native.WireStreamOptions,
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
WireStream = _native.WireStream
WireStreamWithRecv = _native.WireStreamWithRecv


async def _aenter(stream: WireStream) -> Self:
    return stream


async def _aexit(stream: WireStream, exc_type, exc, traceback) -> None:
    del exc_type, exc, traceback
    await stream.drain_outgoing_messages()


WireStream.__aenter__ = _aenter
WireStream.__aexit__ = _aexit

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
