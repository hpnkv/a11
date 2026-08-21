"""Native HTTP Server-Sent Events wire streams.

An HTTP SSE wire stream carries A11 [WireMessage][a11.data.types.WireMessage]
traffic over an HTTP Server-Sent-Events channel -- a firewall-friendly
alternative to WebSockets when only ordinary HTTP is available. See
[a11.net.wire_stream][a11.net.wire_stream]
for the transport-agnostic interface these implement.

Over HTTP/2 the event stream and the outbound direction share one connection.
HTTP/1.1 cannot multiplex and an A11 client connection carries a single request,
so outbound messages need a connection of their own: one for all of them when
the server accepts a streamed request body (which is what a non-multiplexed
connection prefers), or one per message otherwise. `SseOutboundDelivery` names
the two, and `HttpSseClientWireStream.outbound_delivery` reports which one is
in use.
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
from a11._native import SseOutboundDelivery
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
    SseOutboundDelivery,
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
    "SseOutboundDelivery",
    "WireStream",
    "WireStreamOptions",
]
