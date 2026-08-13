"""Driving the HTTP Actions from Python without wiring ports by hand.

[`make_http_request`][a11.sdk.http.actions] has eight output ports because HTTP
has eight things to say, and that is exactly what a Flow wants. A Python caller
who wants three of them does not want to open eight nodes to find out, so this
module does the wiring: `request` and `fetch` feed the inputs, run the action,
and hand back a [`Response`][a11.sdk.http.client.Response] that reads the ports
as they are asked for.

```python
from a11.sdk.http import client

response = await client.fetch("https://example.com/index.html")
print(response.status, await response.text())
```

Nothing here adds behaviour: it is the same Action either way, and a
caller that wants a port at a time can use
[a11.sdk.http.actions][a11.sdk.http.actions] directly. What it adds is that the
ports nobody asked for are drained rather than left for the garbage collector,
which is the one piece of bookkeeping the port model does not do for you.
"""

from __future__ import annotations

import json
from collections.abc import AsyncIterator, Iterable, Mapping
from typing import Any

from a11.actions import Action, ActionSchema
from a11.data import types
from a11.sdk.http.actions import (
    MAKE_HTTP_REQUEST_HANDLER,
    MAKE_HTTP_REQUEST_SCHEMA,
    WEB_FETCH_HANDLER,
    WEB_FETCH_SCHEMA,
)

#: Ports whose values a `Response` reads lazily, so `request` can return before
#: the body has arrived. Everything else is read eagerly and cached.
_STREAMING_PORTS = frozenset({"body", "fields", "items", "redirects", "pushes"})


async def _put_one(action: Action, port: str, value: Any) -> None:
    """Write one value to a unary input and end it."""
    await (await action[port].put(value, final=True))
    await action[port].drain_and_close()


def _bytes_chunk(data: bytes) -> types.Chunk:
    """A body chunk as bytes.

    Written as a chunk rather than through `put`, which would run the value
    through the serialization registry and base64 it into a JSON string. A body
    is bytes on the wire and bytes on the port.
    """
    return types.Chunk(
        data=data,
        metadata=types.ChunkMetadata(mimetype="application/octet-stream"),
    )


async def _feed_body(
    action: Action, body: bytes | Iterable[bytes] | None
) -> None:
    node = action["request_body"]
    if body is not None:
        pieces = (
            [body] if isinstance(body, (bytes, bytearray, memoryview)) else body
        )
        for piece in pieces:
            await (await node.put_chunk(_bytes_chunk(bytes(piece))))
    await node.put_null_final()
    await node.drain_and_close()


class Response:
    """What an HTTP Action produced, read port by port.

    A `Response` exists as soon as the action has been started, which is
    *before* the response has arrived: `status` and `headers` await the ports
    that carry them, and the body is only read when asked for. That is the
    behaviour the ports give for free, and it is why `status` is worth awaiting
    on its own: whether to read a body at all can be decided before it lands.
    """

    def __init__(self, action: Action, *, ports: Iterable[str]) -> None:
        self._action = action
        self._ports = frozenset(ports)
        self._cache: dict[str, Any] = {}
        self._read: set[str] = set()

    @property
    def action(self) -> Action:
        """The running action, for anything this wrapper does not cover."""
        return self._action

    async def _unary(self, port: str, fallback: Any = None) -> Any:
        """Reads a port's single value, remembering it."""
        if port in self._cache:
            return self._cache[port]
        if port not in self._ports:
            return fallback
        self._read.add(port)
        value = fallback
        async for chunk in self._action[port].iter_chunks():
            if chunk.is_null():
                continue
            value = json.loads(chunk.data)
            break
        # Read to the end even after taking the value: a port left part-read
        # holds its writer open.
        async for _ in self._action[port].iter_chunks():
            pass
        self._cache[port] = value
        return value

    async def _values(self, port: str) -> AsyncIterator[Any]:
        if port not in self._ports:
            return
        self._read.add(port)
        async for chunk in self._action[port].iter_chunks():
            if not chunk.is_null():
                yield json.loads(chunk.data)

    # --- Shared by both actions ---------------------------------------------

    async def status(self) -> int | None:
        """The response status code."""
        return await self._unary("status_code")

    async def headers(self) -> dict[str, str]:
        """Response header fields, lower-cased, repeats joined."""
        return await self._unary("headers", {}) or {}

    async def aiter_bytes(self) -> AsyncIterator[bytes]:
        """The response body, chunk by chunk, as it arrives."""
        if "body" not in self._ports:
            return
        self._read.add("body")
        async for chunk in self._action["body"].iter_chunks():
            if not chunk.is_null() and chunk.data:
                yield chunk.data

    async def read(self) -> bytes:
        """The whole response body."""
        if "body_bytes" in self._cache:
            return self._cache["body_bytes"]
        body = b"".join([chunk async for chunk in self.aiter_bytes()])
        self._cache["body_bytes"] = body
        return body

    # --- web-fetch ----------------------------------------------------------

    async def ok(self) -> bool:
        """Whether the status is below 400. `web-fetch` only."""
        value = await self._unary("ok")
        if value is None:
            status = await self.status()
            return status is not None and status < 400
        return bool(value)

    async def text(self) -> str:
        """The body as text.

        Uses `web-fetch`'s own `text` port where there is one, and decodes the
        bytes otherwise, so this works for either action.
        """
        if "text" in self._ports:
            return await self._unary("text", "") or ""
        return (await self.read()).decode("utf-8", errors="replace")

    async def json(self) -> Any:
        """The body parsed as JSON, or None when it is not JSON."""
        if "json" in self._ports:
            return await self._unary("json")
        try:
            return json.loads(await self.read())
        except ValueError:
            return None

    async def aiter_items(self) -> AsyncIterator[Any]:
        """The decoded items: SSE events, NDJSON values, or array elements."""
        async for value in self._values("items"):
            yield value

    # --- make_http_request --------------------------------------------------

    async def trailers(self) -> dict[str, str]:
        """The trailer section after the body; empty when there was none.

        Only meaningful once the body has been read: that is where trailers are
        on the wire, so this awaits the body first.
        """
        await self.read()
        return await self._unary("trailers", {}) or {}

    async def aiter_fields(self) -> AsyncIterator[tuple[str, str]]:
        """Every response header field in wire order, repeats intact."""
        async for pair in self._values("fields"):
            yield (pair[0], pair[1])

    async def redirects(self) -> list[dict[str, Any]]:
        """The hops that were followed, in order."""
        return [hop async for hop in self._values("redirects")]

    async def connection(self) -> dict[str, Any]:
        """How the exchange was carried: url, http_version, secure, reused."""
        return await self._unary("connection", {}) or {}

    async def pushes(self) -> AsyncIterator[tuple[dict[str, Any], Any]]:
        """Each pushed response, paired with the node carrying its body.

        A push has a head *and* a body, and one port cannot interleave several
        bodies; the record names a node instead, and this resolves it. Requires
        ``options={"accept_pushes": True}``.
        """
        node_map = self._action.get_node_map()
        async for record in self._values("pushes"):
            body = node_map.get(record["body"]) if node_map else None
            yield (record, body)

    async def drain(self) -> None:
        """Reads every port nobody asked for, and waits for the action.

        A port with an unread value keeps its writer open, so a caller that took
        the status and walked away would leave the run unfinished. `request` and
        `fetch` used as context managers do this on the way out.
        """
        for port in self._ports:
            if port in self._read or port in self._cache:
                continue
            async for _ in self._action[port].iter_chunks():
                pass
        await self._action.wait()

    async def __aenter__(self) -> Response:
        return self

    async def __aexit__(self, *_: object) -> None:
        await self.drain()


async def _start(
    schema: ActionSchema,
    handler: Any,
    url: str,
    *,
    method: str | None,
    body: bytes | Iterable[bytes] | None,
    headers: Mapping[str, str] | None,
    options: Mapping[str, Any] | None,
    action_id: str = "",
) -> Response:
    action = Action(schema, action_id, handler)
    for name, value in (headers or {}).items():
        action.set_header(name, value)
    action.run()
    await _put_one(action, "url", url)
    await _put_one(action, "method", method)
    await _put_one(action, "options", dict(options or {}))
    await _feed_body(action, body)
    ports = set(schema.outputs) - set(dict(options or {}).get("omit", ()))
    return Response(action, ports=ports)


async def request(
    url: str,
    *,
    method: str | None = None,
    body: bytes | Iterable[bytes] | None = None,
    headers: Mapping[str, str] | None = None,
    options: Mapping[str, Any] | None = None,
) -> Response:
    """Start a ``make_http_request`` and return its `Response`.

    Returns as soon as the request is under way, so `Response.status` can be
    awaited before deciding whether to read the body.

    Args:
        url: Absolute ``http`` or ``https`` URL.
        method: Request method; ``GET`` when omitted.
        body: Request body, whole or in pieces.
        headers: HTTP request headers, set as action headers.
        options: The action's ``options`` document; see its schema.

    Returns:
        The response, whose ports are read as they are asked for. Use it as an
        async context manager, or call
        [`drain`][a11.sdk.http.client.Response.drain], so the ports nobody
        wanted are not left open.
    """
    return await _start(
        MAKE_HTTP_REQUEST_SCHEMA,
        MAKE_HTTP_REQUEST_HANDLER,
        url,
        method=method,
        body=body,
        headers=headers,
        options=options,
    )


async def fetch(
    url: str,
    *,
    method: str | None = None,
    body: bytes | Iterable[bytes] | None = None,
    headers: Mapping[str, str] | None = None,
    options: Mapping[str, Any] | None = None,
) -> Response:
    """Start a ``web-fetch`` and return its `Response`.

    The `fetch()`-shaped path: a 4xx or 5xx is reported by
    [`ok`][a11.sdk.http.client.Response.ok] rather than raised, so an error
    document can still be read.

    Args: as [`request`][a11.sdk.http.client.request].

    Returns:
        The response.
    """
    return await _start(
        WEB_FETCH_SCHEMA,
        WEB_FETCH_HANDLER,
        url,
        method=method,
        body=body,
        headers=headers,
        options=options,
    )


__all__ = ["Response", "fetch", "request"]
