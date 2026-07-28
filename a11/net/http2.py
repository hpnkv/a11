"""Python-facing nghttp2 client, server, and streaming primitives."""

from __future__ import annotations

from collections.abc import Awaitable, Callable, Mapping, Sequence
from typing import TypeAlias

from a11 import _native

HttpHeaders: TypeAlias = Mapping[str, str] | Sequence[tuple[str, str]]
HttpRequestHandler: TypeAlias = Callable[
    ["HttpRequest", "Http2ResponseWriter"], Awaitable[None]
]

HttpRequest = _native.HttpRequest
HttpResponseHead = _native.HttpResponseHead
HttpResponse = _native.HttpResponse
Http2TlsOptions = _native.Http2TlsOptions
Http2Options = _native.Http2Options
Http2RequestBodyStream = _native.Http2RequestBodyStream
Http2ResponseStream = _native.Http2ResponseStream
Http2DuplexStream = _native.Http2DuplexStream
Http2ResponseWriter = _native.Http2ResponseWriter
Http2Server = _native.Http2Server
Http2Client = _native.Http2Client

get_http_header = _native.get_http_header
validate_http_headers = _native.validate_http_headers


def _server_enter(server: Http2Server) -> Http2Server:
    return server


def _server_exit(server: Http2Server, exc_type, exc, traceback) -> None:
    del exc_type, exc, traceback
    server.stop()


async def _client_aenter(client: Http2Client) -> Http2Client:
    return client


async def _client_aexit(client: Http2Client, exc_type, exc, traceback) -> None:
    del exc_type, exc, traceback
    client.close()


def _stream_aiter(stream):
    return stream


async def _stream_anext(stream) -> bytes:
    data = await stream.read()
    if data is None:
        raise StopAsyncIteration
    return data


Http2Server.__enter__ = _server_enter
Http2Server.__exit__ = _server_exit
Http2Client.__aenter__ = _client_aenter
Http2Client.__aexit__ = _client_aexit
for _stream_type in (
    Http2RequestBodyStream,
    Http2ResponseStream,
    Http2DuplexStream,
):
    _stream_type.__aiter__ = _stream_aiter
    _stream_type.__anext__ = _stream_anext

for _class in (
    HttpRequest,
    HttpResponseHead,
    HttpResponse,
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
    "HttpRequest",
    "HttpRequestHandler",
    "HttpResponse",
    "HttpResponseHead",
    "get_http_header",
    "validate_http_headers",
]
