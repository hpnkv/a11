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

    logging.info("Running echo action %s", action.get_id())
    value = await action["input"].consume(str)
    await action["output"].put(value, final=True)
    logging.info("Completed echo action %s", action.get_id())


def make_registry() -> a11.ActionRegistry:
    """Return the action registry shared by all accepted sessions."""

    registry = a11.ActionRegistry()
    registry.register("echo", ECHO_SCHEMA, echo)
    return registry


async def serve(
    host: str = "127.0.0.1",
    port: int = 80,
    certificate: str = "",
    private_key: str = "",
) -> None:
    """Serve echo sessions until interrupted."""

    registry = make_registry()

    async def accept(stream: a11.HttpSseServerWireStream) -> None:
        logging.info("Accepting SSE stream %s", stream.get_id())
        session = a11.Session(action_registry=registry)
        await session.add_stream(stream, mode="accept")
        logging.info("Accepted SSE stream %s", stream.get_id())
        await session.done.wait()

    options = a11.HttpSseOptions()
    options.connect_endpoint = "/demos/echo/connect"
    options.message_endpoint = "/demos/echo/streams/{id}/message"
    options.cors_allow_origin = "*"
    options.cors_allow_methods = "*"
    options.cors_allow_headers = "*"
    options.cors_expose_headers = "x-a11-stream-id"
    if certificate:
        http2_options = options.http2_options
        tls_options = http2_options.tls
        tls_options.enabled = True
        tls_options.certificate_pem_file = certificate
        tls_options.key_pem_file = private_key
        http2_options.tls = tls_options
        options.http2_options = http2_options
    server = a11.HttpSseServer.create(host, port, accept, options)
    scheme = "https" if certificate else "http"
    logging.info(
        "Echo server listening at %s://%s:%d/demos/echo",
        scheme,
        host,
        server.port,
    )
    try:
        await asyncio.Event().wait()
    finally:
        server.stop()


def main() -> None:
    logging.use_absl_handler()
    logging.set_verbosity(logging.DEBUG)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", default=80, type=int)
    parser.add_argument("--certificate", default="", help="TLS certificate PEM")
    parser.add_argument("--private-key", default="", help="TLS private-key PEM")
    args = parser.parse_args()
    if bool(args.certificate) != bool(args.private_key):
        parser.error(
            "--certificate and --private-key must be supplied together"
        )
    asyncio.run(serve(args.host, args.port, args.certificate, args.private_key))


if __name__ == "__main__":
    main()
