"""Python-facing nghttp2 client, server, and streaming primitives.

These are A11's HTTP/2 building blocks -- an `Http2Client`,
`Http2Server`, and the request/response body streams -- that underpin the
WebSocket and HTTP SSE wire-stream transports. Most agents use them only
indirectly through
[a11.net.websocket_wire_stream][a11.net.websocket_wire_stream] or
[a11.net.http_sse_wire_stream][a11.net.http_sse_wire_stream].
"""

from __future__ import annotations

from collections.abc import Awaitable, Callable, Mapping, Sequence
from typing import TypeAlias

from a11 import _native
from a11._native_protocol import attach_protocol

HttpHeaders: TypeAlias = Mapping[str, str] | Sequence[tuple[str, str]]
HttpRequestHandler: TypeAlias = Callable[
    ["HttpRequest", "Http2ResponseWriter"], Awaitable[None]
]

from a11._native import HttpRequest
from a11._native import HttpResponseHead
from a11._native import HttpResponse
from a11._native import HttpProtocolPreference
from a11._native import Http2TlsOptions
from a11._native import Http2Options
from a11._native import Http2RequestBodyStream
from a11._native import Http2ResponseStream
from a11._native import Http2DuplexStream
from a11._native import Http2ResponseWriter
from a11._native import Http2Server
from a11._native import Http2Client

get_http_header = _native.get_http_header
validate_http_headers = _native.validate_http_headers


class _Http2ServerProtocol:
    """Context-manager protocol for the server (``stop`` on exit)."""

    def __enter__(self) -> "Http2Server":
        return self

    def __exit__(self, exc_type, exc, traceback) -> None:
        del exc_type, exc, traceback
        self.stop()


class _Http2ClientProtocol:
    """Async context-manager protocol for the client (``close`` on exit)."""

    async def __aenter__(self) -> "Http2Client":
        return self

    async def __aexit__(self, exc_type, exc, traceback) -> None:
        del exc_type, exc, traceback
        self.close()


class _Http2ByteStreamProtocol:
    """Async-iteration protocol yielding body bytes until the stream ends."""

    def __aiter__(self):
        return self

    async def __anext__(self) -> bytes:
        data = await self.read()
        if data is None:
            raise StopAsyncIteration
        return data


attach_protocol(Http2Server, _Http2ServerProtocol)
attach_protocol(Http2Client, _Http2ClientProtocol)
for _stream_type in (
    Http2RequestBodyStream,
    Http2ResponseStream,
    Http2DuplexStream,
):
    attach_protocol(_stream_type, _Http2ByteStreamProtocol)

for _class in (
    HttpRequest,
    HttpResponseHead,
    HttpResponse,
    HttpProtocolPreference,
    Http2TlsOptions,
    Http2Options,
    Http2RequestBodyStream,
    Http2ResponseStream,
    Http2DuplexStream,
    Http2ResponseWriter,
    Http2Server,
    Http2Client,
):
    _class.__module__ = __name__


__all__ = [
    "Http2Client",
    "Http2DuplexStream",
    "Http2Options",
    "Http2RequestBodyStream",
    "Http2ResponseStream",
    "Http2ResponseWriter",
    "Http2Server",
    "Http2TlsOptions",
    "HttpHeaders",
    "HttpProtocolPreference",
    "HttpRequest",
    "HttpRequestHandler",
    "HttpResponse",
    "HttpResponseHead",
    "get_http_header",
    "validate_http_headers",
]
