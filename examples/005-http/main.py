"""HTTP as separate streams: the two Actions, and the flows in `http.flow`.

    python examples/005-http/main.py            # against a local server
    python examples/005-http/main.py --live     # and against the real web

The example demonstrates four HTTP Action properties:

* **The status arrives before the body.** `only-if-json` branches on the content
  type while the body is still in flight and skips unwanted response bodies.
* **Trailers are a port.** They cannot be sent with the headers -- a checksum is
  not known until the body has been produced -- so a client that flattens a
  response into one object either drops them or makes you wait for everything.
* **A pushed response is its own stream.** The server sends a resource nobody
  asked for; `pushes` carries the head, and each record names a node the body
  was streamed into. That is HTTP/2's own model -- interleaved frames with
  stream ids -- written in A11's.
* **Connections are shared while they are in use and closed when they are not.**
  Two requests to one peer travel over one socket, and nothing is kept open with
  no work on it.

The local server is a real `Http2Server`, so this is a real HTTP/2 conversation
over a loopback socket -- not a stub.
"""

from __future__ import annotations

import argparse
import asyncio
import contextlib
import pathlib

import a11
from a11 import flow, net
from a11.sdk.http import actions, client

_HERE = pathlib.Path(__file__).parent


@contextlib.contextmanager
def demo_server():
    """A small server with something to say on every port.

    Built inside the running loop: the native server captures the ambient
    asyncio loop when it is created, and one made before the loop exists has
    nowhere to dispatch its handler.
    """

    async def handler(request, response):
        path = request.path
        if path == "/page":
            response.send_response(
                200,
                [("content-type", "text/html"), ("x-served-by", "example")],
                b"<h1>A page</h1>",
            )
        elif path == "/report":
            # A body whose checksum is only known once it has been produced --
            # which is what the trailer section is for.
            response.send_headers(
                200,
                [("content-type", "text/csv"), ("trailer", "x-rows, x-digest")],
            )
            rows = 0
            for line in (b"id,name\n", b"1,alpha\n", b"2,beta\n", b"3,gamma\n"):
                response.write(line)
                rows += 1
            response.finish_with_trailers([
                ("x-rows", str(rows)),
                ("x-digest", "d41d8c"),
            ])
        elif path == "/feed":
            response.send_headers(200, [("content-type", "text/event-stream")])
            for index in range(3):
                response.write(
                    f'event: tick\ndata: {{"n": {index}}}\n'
                    f"id: {index}\n\n".encode()
                )
            response.finish()
        elif path == "/rows.ndjson":
            response.send_response(
                200,
                [("content-type", "application/x-ndjson")],
                b'{"name": "alpha"}\n{"name": "beta"}\n',
            )
        elif path == "/with-assets":
            # A resource the client did not ask for, sent alongside one it did.
            with contextlib.suppress(Exception):
                pushed = response.push_promise("GET", "/style.css")
                pushed.send_response(
                    200,
                    [("content-type", "text/css")],
                    b"h1 { color: rebeccapurple }",
                )
            response.send_response(
                200,
                [("content-type", "text/html")],
                b'<link rel=stylesheet href="/style.css"><h1>Styled</h1>',
            )
        elif path == "/upload":
            response.send_response(
                200,
                [("content-type", "text/plain")],
                f"took {len(request.body)} bytes by {request.method}".encode(),
            )
        elif path == "/slow":
            # Answers its headers at once and its body later, so a caller can
            # have a request genuinely in flight while it makes another.
            response.send_headers(200, [("content-type", "text/plain")])
            await asyncio.sleep(0.2)
            response.write(b"eventually")
            response.finish()
        elif path == "/moved":
            response.send_response(302, [("location", "/page")], b"")
        else:
            response.send_response(404, [("content-type", "text/plain")], b"no")

    server = net.Http2Server.create("127.0.0.1", 0, handler)
    try:
        yield server
    finally:
        server.stop()


def url(server, path: str) -> str:
    return f"http://127.0.0.1:{server.port}{path}"


async def status_before_body(server) -> None:
    """Act on the status while the body is still arriving."""
    print("\n--- the status arrives before the body ---")
    async with await client.request(url(server, "/report")) as response:
        print(f"  status  {await response.status()}")
        headers = await response.headers()
        print(f"  type    {headers['content-type']}")
        # The decision to read the body at all is made here, with the body still
        # in flight. This is the whole reason these are different ports.
        if not headers["content-type"].startswith("text/csv"):
            print("  not a report; not reading it")
            return
        rows = 0
        async for chunk in response.aiter_bytes():
            rows += chunk.count(b"\n")
        print(f"  rows    {rows} (counted as they arrived)")
        # And the trailers, which could not have been sent any earlier.
        trailers = await response.trailers()
        print(
            f"  trailer x-rows={trailers.get('x-rows')} "
            f"x-digest={trailers.get('x-digest')}"
        )


async def repeated_header_fields(server) -> None:
    """A joined header map is convenient; the field list is the truth."""
    print("\n--- headers twice, on purpose ---")
    async with await client.request(url(server, "/page")) as response:
        headers = await response.headers()
        print(f"  map     content-type={headers['content-type']}")
        fields = [pair async for pair in response.aiter_fields()]
        print(f"  fields  {fields}")
        await response.read()


async def pushed_resources(server) -> None:
    """A response the server sent without being asked."""
    print("\n--- server push, on a port of its own ---")
    async with await client.request(
        url(server, "/with-assets"), options={"accept_pushes": True}
    ) as response:
        print(f"  asked for  {len(await response.read())} bytes of HTML")
        async for record, body in response.pushes():
            pushed = b"".join([
                chunk.data
                async for chunk in body.iter_chunks()
                if not chunk.is_null()
            ])
            print(
                f"  pushed     {record['path']} "
                f"({record['headers']['content-type']}, {len(pushed)} bytes)"
            )


async def streaming_decodes(server) -> None:
    """web-fetch decodes by content type, so a caller does not have to."""
    print("\n--- web-fetch: the body in the shape you want it ---")
    async with await client.fetch(url(server, "/rows.ndjson")) as response:
        rows = [row async for row in response.aiter_items()]
        print(f"  ndjson  {rows}")
    async with await client.fetch(url(server, "/feed")) as response:
        events = [event async for event in response.aiter_items()]
        print(f"  sse     {[event['json']['n'] for event in events]}")
    async with await client.fetch(url(server, "/nope")) as response:
        # A 4xx is data, not an exception: the error document is the point.
        print(
            f"  404     ok={await response.ok()} body={await response.text()!r}"
        )


async def streamed_upload(server) -> None:
    """A body sent as it is assembled, with no content-length."""
    print("\n--- a streamed request body ---")
    async with await client.request(
        url(server, "/upload"),
        method="POST",
        body=[b"part-one ", b"part-two ", b"part-three"],
        options={"request_body": "stream"},
    ) as response:
        print(f"  server saw: {(await response.read()).decode()}")


async def shared_connections(server) -> None:
    """One socket while there is work on it, and none when there is not."""
    print("\n--- connections are shared, never hoarded ---")
    # /slow answers its headers now and its body later, so this request is still
    # genuinely in flight below.
    slow = await client.request(url(server, "/slow"))
    await slow.status()
    joined = await client.request(url(server, "/page"))
    await joined.read()
    # HTTP/2 multiplexing completes this request on the same socket while the
    # first request remains open.
    print(
        f"  joined a connection in use: {(await joined.connection())['reused']}"
    )
    await joined.drain()
    await slow.read()
    await slow.drain()
    # Both leases are gone, so the connection is closed rather than parked: the
    # next request dials, and nothing was holding a socket open for nobody.
    after = await client.request(url(server, "/page"))
    await after.read()
    print(
        f"  after work ended, reused:   {(await after.connection())['reused']}"
    )
    await after.drain()


async def run_the_flows(server) -> None:
    """The same actions, composed as text rather than called from Python."""
    print("\n--- the flows in http.flow ---")
    registry = a11.ActionRegistry()
    actions.register(registry)
    program = flow.loads((_HERE / "http.flow").read_text(), "http.flow")
    program.register_all(registry)

    page = await program["read-a-page"].invoke(
        {"url": url(server, "/page")}, registry=registry
    )
    print(f"  read-a-page    status={page['code']} text={page['text']!r}")

    json_only = await program["only-if-json"].invoke(
        {"url": url(server, "/page")}, registry=registry
    )
    print(
        f"  only-if-json   declined={json_only['declined']!r} "
        f"(never read the body)"
    )

    inspected = await program["inspect-a-url"].invoke(
        {"url": url(server, "/moved")}, registry=registry
    )
    print(
        f"  inspect-a-url  code={inspected['code']} "
        f"hops={[hop['location'] for hop in inspected['hops']]} "
        f"size={inspected['size']} "
        f"carried={inspected['carried']['http_version']}"
    )

    sent = await program["send-a-stream"].invoke(
        {"url": url(server, "/upload"), "parts": ["a", "b", "c"]},
        registry=registry,
    )
    print(f"  send-a-stream  {sent['answer']!r}")


async def against_the_real_web() -> None:
    """The same actions, unchanged, against a server nobody here controls."""
    print("\n--- and the actual internet ---")
    async with await client.fetch("https://example.com/") as response:
        text = await response.text()
        print(
            f"  example.com  status={await response.status()} bytes={len(text)}"
        )
    async with await client.request("https://example.com/") as response:
        connection = await response.connection()
        print(
            f"  carried over HTTP/{connection['http_version']} "
            f"tls={connection['secure']}"
        )
        await response.read()


async def main(live: bool) -> None:
    with demo_server() as server:
        await status_before_body(server)
        await repeated_header_fields(server)
        await pushed_resources(server)
        await streaming_decodes(server)
        await streamed_upload(server)
        await shared_connections(server)
        await run_the_flows(server)
    if live:
        await against_the_real_web()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--live",
        action="store_true",
        help="also fetch https://example.com, which needs a network.",
    )
    asyncio.run(main(parser.parse_args().live))
