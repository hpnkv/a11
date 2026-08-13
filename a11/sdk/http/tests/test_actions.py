# Copyright 2026 The A11 Authors.

"""The HTTP Actions through the bindings, against a local server.

The C++ suite covers the protocol -- trailers, pushes, streamed uploads, the
redirect chain. What matters here is the binding layer and the shape a Python
caller sees: that the schemas arrive with their ports typed, that the awaitables
resolve on the event loop, and that the `Response` helpers read the ports
without leaving any of them open.
"""

from __future__ import annotations

import asyncio
import contextlib

import pytest

import a11
from a11 import net
from a11.sdk.http import actions, client

# Every await is bounded: an unread port waits forever, which is a hang rather
# than a failure, and a hung test tells you nothing.
_PATIENCE = 10


async def _bounded(awaitable):
    return await asyncio.wait_for(awaitable, _PATIENCE)


@contextlib.contextmanager
def _server():
    """A local HTTP server covering the shapes these actions decode.

    Built inside the running loop rather than in a fixture: the native server
    captures the ambient asyncio loop when it is created, and one created before
    the loop exists has nowhere to dispatch its Python handler.
    """
    seen: dict[str, object] = {}
    # Held open so a test can have a request genuinely in flight while it makes
    # another to the same peer.
    release = asyncio.Event()

    def handle(request, response):
        seen["headers"] = dict(request.headers)
        seen["method"] = request.method
        seen["body"] = request.body
        path = request.path
        if path == "/plain":
            response.send_response(
                200, [("content-type", "text/plain")], b"a plain body"
            )
        elif path == "/trailed":
            response.send_headers(
                200, [("content-type", "text/plain"), ("trailer", "x-digest")]
            )
            response.write(b"counted")
            response.finish_with_trailers([("x-digest", "7")])
        elif path == "/json":
            response.send_response(
                200,
                [("content-type", "application/json")],
                b'{"name": "a11", "count": 2}',
            )
        elif path == "/rows":
            response.send_response(
                200,
                [("content-type", "application/x-ndjson")],
                b'{"n": 1}\n{"n": 2}\n{"n": 3}\n',
            )
        elif path == "/events":
            response.send_headers(200, [("content-type", "text/event-stream")])
            response.write(b'event: tick\ndata: {"n": 1}\nid: 7\n\n')
            response.write(b"data: plain\n\n")
            response.finish()
        elif path == "/redirect":
            response.send_response(302, [("location", "/plain")], b"")
        elif path == "/missing":
            response.send_response(
                404, [("content-type", "text/plain")], b"gone"
            )
        elif path == "/echo":
            response.send_response(
                200,
                [("content-type", "text/plain")],
                f"{request.method}:{request.body.decode()}".encode(),
            )
        elif path == "/pushing":
            with contextlib.suppress(Exception):
                pushed = response.push_promise("GET", "/style.css")
                pushed.send_response(
                    200, [("content-type", "text/css")], b"body{}"
                )
            response.send_response(
                200, [("content-type", "text/html")], b"<html>"
            )
        else:
            response.send_response(404, [], b"nope")

    async def handler(request, response):
        if request.path == "/hold":
            response.send_headers(200, [("content-type", "text/plain")])
            await release.wait()
            response.write(b"held")
            response.finish()
            return
        handle(request, response)

    created = net.Http2Server.create("127.0.0.1", 0, handler)
    try:
        yield created, seen, release
    finally:
        release.set()
        created.stop()


def _url(server, path: str) -> str:
    return f"http://127.0.0.1:{server.port}{path}"


# --- Registration and schemas ------------------------------------------------


def test_register_installs_both_actions() -> None:
    registry = a11.ActionRegistry()
    actions.register(registry)
    assert registry.is_registered(actions.MAKE_HTTP_REQUEST)
    assert registry.is_registered(actions.WEB_FETCH)
    registry.get_schema(actions.MAKE_HTTP_REQUEST).validate()
    registry.get_schema(actions.WEB_FETCH).validate()


def test_register_can_serve_only_the_adapter() -> None:
    # A gateway happy to let a caller fetch a document may not want to hand out
    # streamed uploads and arbitrary methods.
    registry = a11.ActionRegistry()
    actions.register(registry, low_level=False)
    assert not registry.is_registered(actions.MAKE_HTTP_REQUEST)
    assert registry.is_registered(actions.WEB_FETCH)


def test_exported_pairs_carry_native_handlers() -> None:
    assert tuple(schema.name for schema, _ in actions.HTTP_ACTIONS) == (
        actions.MAKE_HTTP_REQUEST,
        actions.WEB_FETCH,
    )
    for schema, handler in actions.HTTP_ACTIONS:
        schema.validate()
        assert isinstance(handler, a11.actions.NativeActionHandler)
        assert handler


def test_request_declares_a_port_per_http_concern() -> None:
    outputs = actions.MAKE_HTTP_REQUEST_SCHEMA.outputs
    assert set(outputs) == {
        "status_code",
        "headers",
        "fields",
        "body",
        "trailers",
        "redirects",
        "pushes",
        "connection",
    }
    # The response body is a stream and the status is one value: the difference
    # is the whole reason they are separate ports.
    assert not outputs["body"].unary
    assert outputs["status_code"].unary


def test_ports_are_typed_for_python() -> None:
    schema = actions.MAKE_HTTP_REQUEST_SCHEMA
    assert schema.inputs["url"].typeinfo is str
    assert schema.inputs["request_body"].typeinfo is bytes
    assert schema.outputs["status_code"].typeinfo is int
    assert schema.outputs["body"].typeinfo is bytes
    assert actions.WEB_FETCH_SCHEMA.outputs["ok"].typeinfo is bool


def test_the_request_body_input_does_not_collide_with_the_body_output() -> None:
    # An input and an output of the same name would be the same node, since a
    # port's node id is derived from the action id and the name alone.
    for schema in (actions.MAKE_HTTP_REQUEST_SCHEMA, actions.WEB_FETCH_SCHEMA):
        assert not set(schema.inputs) & set(schema.outputs)


# --- make_http_request -------------------------------------------------------


@pytest.mark.asyncio
async def test_request_separates_status_headers_and_body() -> None:
    with _server() as (server, _, _release):
        async with await client.request(_url(server, "/plain")) as response:
            # The status is readable before the body, which is the point.
            assert await _bounded(response.status()) == 200
            assert (await _bounded(response.headers()))["content-type"] == (
                "text/plain"
            )
            assert await _bounded(response.read()) == b"a plain body"
            connection = await _bounded(response.connection())
            assert connection["http_version"] == "2"
            assert connection["url"] == _url(server, "/plain")


@pytest.mark.asyncio
async def test_request_delivers_trailers_after_the_body() -> None:
    with _server() as (server, _, _release):
        async with await client.request(_url(server, "/trailed")) as response:
            assert await _bounded(response.read()) == b"counted"
            # The value a checksum travels in: unknowable when the headers went.
            assert (await _bounded(response.trailers()))["x-digest"] == "7"


@pytest.mark.asyncio
async def test_request_streams_the_body_as_it_arrives() -> None:
    with _server() as (server, _, _release):
        async with await client.request(_url(server, "/plain")) as response:
            pieces = [chunk async for chunk in response.aiter_bytes()]
            assert b"".join(pieces) == b"a plain body"


@pytest.mark.asyncio
async def test_request_reports_the_redirect_chain() -> None:
    with _server() as (server, _, _release):
        async with await client.request(_url(server, "/redirect")) as response:
            assert await _bounded(response.read()) == b"a plain body"
            hops = await _bounded(response.redirects())
            assert [hop["location"] for hop in hops] == ["/plain"]


@pytest.mark.asyncio
async def test_request_delivers_an_error_response_rather_than_raising() -> None:
    with _server() as (server, _, _release):
        async with await client.request(_url(server, "/missing")) as response:
            # The server was reached and answered; that is not a failed request.
            assert await _bounded(response.status()) == 404
            assert await _bounded(response.read()) == b"gone"


@pytest.mark.asyncio
async def test_request_raises_when_there_is_no_response() -> None:
    response = await client.request("http://127.0.0.1:1/nothing")
    with pytest.raises(Exception):
        await _bounded(response.drain())


@pytest.mark.asyncio
async def test_headers_are_sent_and_framework_headers_are_not() -> None:
    with _server() as (server, seen, _release):
        async with await client.request(
            _url(server, "/plain"),
            headers={"accept": "text/plain", "authorization": "Bearer s"},
        ) as response:
            await _bounded(response.read())
        assert seen["headers"]["accept"] == "text/plain"
        assert seen["headers"]["authorization"] == "Bearer s"
        # A11's own headers are the action's business, not the peer's.
        assert not any(name.startswith("x-a11-") for name in seen["headers"]), (
            seen["headers"]
        )


@pytest.mark.asyncio
async def test_fields_keeps_repeated_headers_apart() -> None:
    with _server() as (server, _, _release):
        async with await client.request(_url(server, "/plain")) as response:
            fields = [pair async for pair in response.aiter_fields()]
        names = [name for name, _ in fields]
        assert "content-type" in names


@pytest.mark.asyncio
async def test_request_sends_a_buffered_body_with_a_content_length() -> None:
    with _server() as (server, seen, _release):
        async with await client.request(
            _url(server, "/echo"), method="POST", body=[b"one-", b"two"]
        ) as response:
            assert await _bounded(response.read()) == b"POST:one-two"
        assert seen["headers"]["content-length"] == str(len(b"one-two"))


@pytest.mark.asyncio
async def test_request_streams_a_body_without_a_content_length() -> None:
    with _server() as (server, seen, _release):
        async with await client.request(
            _url(server, "/echo"),
            method="POST",
            body=[b"alpha", b"beta"],
            options={"request_body": "stream"},
        ) as response:
            assert await _bounded(response.read()) == b"POST:alphabeta"
        # A streamed body has no length to declare when the headers go out.
        assert "content-length" not in seen["headers"]


@pytest.mark.asyncio
async def test_omit_closes_ports_without_writing_them() -> None:
    with _server() as (server, _, _release):
        async with await client.request(
            _url(server, "/plain"),
            options={"omit": ["fields", "redirects", "pushes", "connection"]},
        ) as response:
            assert await _bounded(response.read()) == b"a plain body"
            # An omitted port is closed rather than left hanging, so draining
            # everything still finishes.
            assert await _bounded(response.connection()) == {}


@pytest.mark.asyncio
async def test_request_delivers_a_pushed_response_and_its_body_node() -> None:
    with _server() as (server, _, _release):
        async with await client.request(
            _url(server, "/pushing"), options={"accept_pushes": True}
        ) as response:
            assert await _bounded(response.read()) == b"<html>"
            pushed = [pair async for pair in response.pushes()]
            assert len(pushed) == 1
            record, body = pushed[0]
            assert record["path"] == "/style.css"
            assert record["headers"]["content-type"] == "text/css"
            # A push has a head and a body; one port cannot interleave several
            # bodies, so the record names a node instead.
            chunks = [
                chunk.data
                async for chunk in body.iter_chunks()
                if not chunk.is_null()
            ]
            assert b"".join(chunks) == b"body{}"


@pytest.mark.asyncio
async def test_no_pushes_arrive_unless_they_are_asked_for() -> None:
    with _server() as (server, _, _release):
        async with await client.request(_url(server, "/pushing")) as response:
            assert await _bounded(response.read()) == b"<html>"
            assert [pair async for pair in response.pushes()] == []


@pytest.mark.asyncio
async def test_unusable_options_are_rejected() -> None:
    response = await client.request(
        "http://127.0.0.1:1/nothing", options={"request_body": "telepathy"}
    )
    with pytest.raises(Exception):
        await _bounded(response.drain())


# --- web-fetch ---------------------------------------------------------------


@pytest.mark.asyncio
async def test_fetch_hands_back_text_and_json() -> None:
    with _server() as (server, _, _release):
        async with await client.fetch(_url(server, "/json")) as response:
            assert await _bounded(response.status()) == 200
            assert await _bounded(response.ok()) is True
            assert (
                await _bounded(response.text()) == '{"name": "a11", "count": 2}'
            )
            assert (await _bounded(response.json()))["name"] == "a11"


@pytest.mark.asyncio
async def test_fetch_reports_an_error_response_as_data() -> None:
    with _server() as (server, _, _release):
        async with await client.fetch(_url(server, "/missing")) as response:
            assert await _bounded(response.ok()) is False
            # The error document is still readable, which is why it does not
            # raise: an API's failure body is usually the interesting part.
            assert await _bounded(response.text()) == "gone"


@pytest.mark.asyncio
async def test_fetch_json_is_none_for_a_page_that_is_not_json() -> None:
    with _server() as (server, _, _release):
        async with await client.fetch(_url(server, "/plain")) as response:
            assert await _bounded(response.text()) == "a plain body"
            assert await _bounded(response.json()) is None


@pytest.mark.asyncio
async def test_fetch_streams_ndjson_rows_as_items() -> None:
    with _server() as (server, _, _release):
        async with await client.fetch(_url(server, "/rows")) as response:
            rows = [row async for row in response.aiter_items()]
            assert [row["n"] for row in rows] == [1, 2, 3]


@pytest.mark.asyncio
async def test_fetch_decodes_server_sent_events_as_items() -> None:
    with _server() as (server, _, _release):
        async with await client.fetch(_url(server, "/events")) as response:
            events = [event async for event in response.aiter_items()]
        assert [event["event"] for event in events] == ["tick", "message"]
        # The data is usually JSON, so it is parsed rather than handed back for
        # the caller to parse again.
        assert events[0]["json"]["n"] == 1
        assert events[0]["id"] == "7"
        assert events[1]["data"] == "plain"
        assert "json" not in events[1]


@pytest.mark.asyncio
async def test_fetch_follows_redirects() -> None:
    with _server() as (server, _, _release):
        async with await client.fetch(_url(server, "/redirect")) as response:
            assert await _bounded(response.text()) == "a plain body"


@pytest.mark.asyncio
async def test_fetch_can_stream_bytes_without_buffering_text() -> None:
    with _server() as (server, _, _release):
        async with await client.fetch(
            _url(server, "/plain"), options={"omit": ["text", "json", "items"]}
        ) as response:
            pieces = [chunk async for chunk in response.aiter_bytes()]
            assert b"".join(pieces) == b"a plain body"


@pytest.mark.asyncio
async def test_a_second_request_joins_a_connection_already_in_use() -> None:
    # The property the pool is for: a request to a peer another request is
    # already talking to travels over that connection instead of opening one.
    # /hold keeps the first exchange genuinely in flight, so there is no race
    # over whether it still holds the connection when the second asks.
    with _server() as (server, _, release):
        held = await client.request(_url(server, "/hold"))
        # Its headers have arrived, so the connection is up and leased.
        assert await _bounded(held.status()) == 200

        joined = await client.request(_url(server, "/json"))
        assert await _bounded(joined.read()) == b'{"name": "a11", "count": 2}'
        # And it completed while the first was still open, because HTTP/2
        # multiplexes -- which is what makes sharing worth doing.
        assert (await _bounded(joined.connection()))["reused"] is True

        release.set()
        assert await _bounded(held.read()) == b"held"
        await _bounded(held.drain())
        await _bounded(joined.drain())


@pytest.mark.asyncio
async def test_a_connection_is_not_kept_after_the_work_on_it_ends() -> None:
    # Nothing idles: the connection lives exactly as long as the requests on it,
    # so a later request to the same peer dials afresh rather than inheriting a
    # socket the server may have closed in the meantime.
    with _server() as (server, _, _release):
        async with await client.request(_url(server, "/plain")) as first:
            assert await _bounded(first.read()) == b"a plain body"
            assert (await _bounded(first.connection()))["reused"] is False
        async with await client.request(_url(server, "/plain")) as second:
            assert await _bounded(second.read()) == b"a plain body"
            assert (await _bounded(second.connection()))["reused"] is False
