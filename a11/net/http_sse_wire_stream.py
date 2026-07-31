"""Native HTTP/2 Server-Sent Events wire streams.

An HTTP SSE wire stream carries A11 [WireMessage][a11.data.types.WireMessage]
traffic
over an HTTP/2 Server-Sent-Events channel -- a firewall-friendly alternative to
WebSockets when only ordinary HTTP is available. See
[a11.net.wire_stream][a11.net.wire_stream]
for the transport-agnostic interface these implement.
"""

from a11 import _native
from a11._native_protocol import attach_protocol

from .http2 import Http2Client, Http2Options, Http2Server
from .wire_stream import WireStream, WireStreamOptions

DEFAULT_CONNECT_ENDPOINT = _native.DEFAULT_SSE_CONNECT_ENDPOINT
DEFAULT_MESSAGE_ENDPOINT = _native.DEFAULT_SSE_MESSAGE_ENDPOINT
HTTP_HEADER_PREFIX = _native.SSE_HTTP_HEADER_PREFIX
STREAM_ID_HEADER = _native.SSE_STREAM_ID_HEADER

from a11._native import HttpSseOptions
from a11._native import HttpSseWireStream
from a11._native import HttpSseClientWireStream
from a11._native import HttpSseServerWireStream
from a11._native import HttpSseServer
from a11._native import HttpSseWireStreamServer


class _HttpSseServerProtocol:
    """Context-manager protocol for the SSE server (``stop`` on exit)."""

    def __enter__(self) -> "HttpSseServer":
        return self

    def __exit__(self, exc_type, exc, traceback) -> None:
        del exc_type, exc, traceback
        self.stop()


attach_protocol(HttpSseServer, _HttpSseServerProtocol)

for _class in (
    HttpSseOptions,
    HttpSseWireStream,
    HttpSseClientWireStream,
    HttpSseServerWireStream,
    HttpSseServer,
):
    _class.__module__ = __name__


__all__ = [
    "DEFAULT_CONNECT_ENDPOINT",
    "DEFAULT_MESSAGE_ENDPOINT",
    "HTTP_HEADER_PREFIX",
    "Http2Client",
    "Http2Options",
    "Http2Server",
    "HttpSseClientWireStream",
    "HttpSseOptions",
    "HttpSseServer",
    "HttpSseServerWireStream",
    "HttpSseWireStream",
    "HttpSseWireStreamServer",
    "STREAM_ID_HEADER",
    "WireStream",
    "WireStreamOptions",
]
