"""WebRTC peer-to-peer echo server.

Run with ``python -m a11.demos.echo_server_webrtc``. This is the WebRTC
counterpart of :mod:`a11.demos.echo_server`: it exposes the same ``echo``
action, but reaches clients over WebRTC data channels instead of HTTP/2 SSE.

The process starts *two* servers:

* a WebSocket **signalling** server, used only to exchange the SDP/ICE
  handshake, and
* a :class:`~a11.net.webrtc_wire_stream.WebRtcWireServer` listening under the
  identity ``echo_server_webrtc`` on the signalling service.

A client connects to the printed signalling URL and dials ``echo_server_webrtc``
(see :mod:`a11.demos.echo_client_webrtc`). Each accepted stream stripes A11
packets across several data channels transparently; the demo does not need to
know or care how many.
"""

from __future__ import annotations

import argparse
import asyncio

import a11
from absl import logging

from a11.net.signalling import (
    SignallingService,
    WebSocketSignallingServer,
    WebSocketSignallingServerOptions,
)
from a11.net.webrtc_wire_stream import WebRtcConfiguration, WebRtcWireServer

SERVER_IDENTITY = "echo_server_webrtc"

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


async def serve(host: str = "127.0.0.1", port: int = 8787) -> None:
    """Serve WebRTC echo sessions until interrupted."""

    registry = make_registry()
    service = SignallingService.create()

    signalling_options = WebSocketSignallingServerOptions()
    signalling_options.bind_address = host
    signalling_options.port = port
    signalling_server = WebSocketSignallingServer.create(
        service, signalling_options
    )

    async def on_stream(stream: a11.WebRtcWireStream) -> None:
        logging.info("Accepting WebRTC stream %s", stream.get_id())
        session = a11.Session(action_registry=registry)
        await session.add_stream(stream, mode="accept")
        logging.info("Accepted WebRTC stream %s", stream.get_id())
        await session.done.wait()
        logging.info("WebRTC stream %s closed", stream.get_id())

    # Defaults to eight data channels per connection; override max_channels to
    # cap how many a single peer may open.
    configuration = WebRtcConfiguration()
    server = WebRtcWireServer.create(
        SERVER_IDENTITY, service, on_stream, configuration
    )
    logging.info(
        "WebRTC echo server '%s' ready; signalling at ws://%s:%d",
        SERVER_IDENTITY,
        host,
        signalling_server.port,
    )
    print(
        f"echo_server_webrtc listening: connect with\n"
        f"  python -m a11.demos.echo_client_webrtc {SERVER_IDENTITY} "
        f"ws://{host}:{signalling_server.port}",
        flush=True,
    )
    try:
        await asyncio.Event().wait()
    finally:
        server.stop()
        signalling_server.stop()
        service.stop()


def main() -> None:
    logging.use_absl_handler()
    logging.set_verbosity(logging.DEBUG)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument(
        "--port",
        default=8787,
        type=int,
        help="Signalling server port (0 selects an ephemeral port)",
    )
    args = parser.parse_args()
    asyncio.run(serve(args.host, args.port))


if __name__ == "__main__":
    main()
