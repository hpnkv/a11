"""HTTP/2 SSE server for the browser-client echo guide.

Run with ``python -m a11.demos.echo_server``. The service exposes the A11 SSE
endpoints under ``/demos/echo`` and executes one action named ``echo``.
"""

from __future__ import annotations

import argparse
import asyncio

import a11
from absl import logging

ECHO_SCHEMA = a11.ActionSchema(
    name="echo",
    description="Return the supplied text unchanged.",
    inputs={
        "input": a11.ActionPortSchema(
            name="input", type="text/plain", typeinfo=str, required=True
        )
    },
    outputs={
        "output": a11.ActionPortSchema(
            name="output", type="text/plain", typeinfo=str, required=True
        )
    },
)


async def echo(action: a11.Action) -> None:
    """Copy the final input value to the action's output node."""

    value = await action["input"].consume(str)
    await action["output"].put(value, final=True)


def make_registry() -> a11.ActionRegistry:
    """Return the action registry shared by all accepted sessions."""

    registry = a11.ActionRegistry()
    registry.register("echo", ECHO_SCHEMA, echo)
    return registry


async def serve(host: str = "127.0.0.1", port: int = 80) -> None:
    """Serve echo sessions until interrupted."""

    registry = make_registry()

    async def accept(stream: a11.HttpSseServerWireStream) -> None:
        async def close_after_peer(
            message: a11.WireMessage | None,
            _stream: a11.WireStream,
            session: a11.Session,
        ) -> None:
            if message is None:
                session.half_close()

        session = a11.Session(
            action_registry=registry, on_stream_message=close_after_peer
        )
        await session.add_stream(stream, mode="accept")
        await session.done.wait()

    options = a11.HttpSseOptions()
    options.connect_endpoint = "/demos/echo/connect"
    options.message_endpoint = "/demos/echo/streams/{id}/message"
    server = a11.HttpSseServer.create(host, port, accept, options)
    logging.info(
        "Echo server listening at http://%s:%d/demos/echo", host, server.port
    )
    try:
        await asyncio.Event().wait()
    finally:
        server.stop()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", default=80, type=int)
    args = parser.parse_args()
    asyncio.run(serve(args.host, args.port))


if __name__ == "__main__":
    main()
