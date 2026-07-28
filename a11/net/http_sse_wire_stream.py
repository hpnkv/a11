"""Native HTTP/2 Server-Sent Events wire streams."""

from a11 import _native

from .http2 import Http2Client, Http2Options, Http2Server
from .wire_stream import WireStream, WireStreamOptions

DEFAULT_CONNECT_ENDPOINT = _native.DEFAULT_SSE_CONNECT_ENDPOINT
DEFAULT_MESSAGE_ENDPOINT = _native.DEFAULT_SSE_MESSAGE_ENDPOINT
HTTP_HEADER_PREFIX = _native.SSE_HTTP_HEADER_PREFIX
STREAM_ID_HEADER = _native.SSE_STREAM_ID_HEADER

HttpSseOptions = _native.HttpSseOptions
HttpSseWireStream = _native.HttpSseWireStream
HttpSseClientWireStream = _native.HttpSseClientWireStream
HttpSseServerWireStream = _native.HttpSseServerWireStream
HttpSseServer = _native.HttpSseServer
HttpSseWireStreamServer = _native.HttpSseWireStreamServer


def _server_enter(server: HttpSseServer) -> HttpSseServer:
    return server


def _server_exit(server: HttpSseServer, exc_type, exc, traceback) -> None:
    del exc_type, exc, traceback
    server.stop()


HttpSseServer.__enter__ = _server_enter
HttpSseServer.__exit__ = _server_exit

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
