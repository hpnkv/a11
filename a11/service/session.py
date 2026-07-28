"""Native sessions with asyncio-shaped completion and receive methods."""

from __future__ import annotations

from collections.abc import Awaitable, Callable, Mapping

from a11 import _native, timing
from a11._native_options import install_native_options
from a11.data import types
from a11.net.wire_stream import WireStream
from a11.service._native_session import Session, SessionWithRecv

OnSessionStreamMessage = Callable[
    [types.WireMessage | None, WireStream, "Session"], Awaitable[None]
]
OnSessionStreamDone = Callable[[WireStream, "Session"], Awaitable[None]]

SESSION_STATUS_HEADER = _native.SESSION_STATUS_HEADER
MAX_SINGLE_MESSAGE_SIZE = _native.SESSION_MAX_SINGLE_MESSAGE_SIZE

SessionOptions = install_native_options(
    _native.SessionOptions,
    {
        "max_buffered_messages_total": (int, 256),
        "max_buffered_messages_per_stream": (int, 32),
        "max_concurrent_root_actions": (int, 32),
        "max_concurrent_nested_actions": (int, 128),
        "max_single_message_size": (int, MAX_SINGLE_MESSAGE_SIZE),
        "max_buffered_bytes_total": (int, 32 * 1024 * 1024),
        "max_buffered_bytes_per_stream": (int, 4 * 1024 * 1024),
        "no_stream_timeout": (timing.Duration, timing.Duration.seconds(30)),
        "deadline": (timing.Time | None, timing.infinite_future()),
    },
)
SessionOptions.__module__ = __name__


def normalise_headers(
    headers: Mapping[str, bytes] | None,
) -> dict[str, bytes]:
    """Validate and case-normalise Session headers in native code."""

    return _native.normalize_session_headers(headers)


__all__ = [
    "MAX_SINGLE_MESSAGE_SIZE",
    "OnSessionStreamDone",
    "OnSessionStreamMessage",
    "SESSION_STATUS_HEADER",
    "Session",
    "SessionOptions",
    "SessionWithRecv",
    "normalise_headers",
]
