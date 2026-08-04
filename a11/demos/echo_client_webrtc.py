"""WebRTC echo client for :mod:`a11.demos.echo_server_webrtc`.

Run with::

    python -m a11.demos.echo_client_webrtc <server_identity> <signalling_url>

for example::

    python -m a11.demos.echo_client_webrtc echo_server_webrtc ws://127.0.0.1:8787

It connects to the WebSocket signalling server at ``signalling_url``, dials the
peer named ``server_identity`` over WebRTC, then reads lines from stdin and
prints the server's echo of each. The underlying stream stripes A11 packets
across several data channels; that is invisible here -- it behaves as one
ordered, reliable stream.
"""

from __future__ import annotations

import argparse
import asyncio

import a11
from absl import logging

from a11.net.signalling import WebSocketSignallingClient
from a11.net.webrtc_wire_stream import WebRtcConfiguration, WebRtcWireStream

CLIENT_IDENTITY = "echo_client_webrtc"

# Must match the server's "echo" action schema.
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


async def call_echo(
    session: a11.Session, stream: a11.WebRtcWireStream, text: str
) -> str:
    """Invoke the remote ``echo`` action once and return its reply."""

    action = a11.Action(
        ECHO_SCHEMA,
        node_map=session.node_map,
        stream=stream,
        session=session,
    )
    await action.call()
    await action["input"].put(text, final=True)
    (await action.wait_for_dispatch()).raise_if_not_ok()
    reply = await action["output"].consume(str)
    await action.wait()
    action.get_status().raise_if_not_ok()
    return reply


async def run(server_identity: str, signalling_url: str) -> None:
    """Connect, then echo stdin lines until EOF or ``exit``."""

    signalling = await WebSocketSignallingClient.connect(
        signalling_url, CLIENT_IDENTITY
    )
    session = a11.Session()
    try:
        stream = WebRtcWireStream.create_client(
            server_identity, signalling, WebRtcConfiguration()
        )
        await session.add_stream(stream, mode="start")
        logging.info(
            "Connected to '%s' via %s", server_identity, signalling_url
        )
        try:
            while True:
                try:
                    text = await asyncio.to_thread(input, "> ")
                except EOFError:
                    break
                if text.casefold() == "exit":
                    break
                reply = await call_echo(session, stream, text)
                print(f"echo: {reply}", flush=True)
        finally:
            # Signal we are done sending. The server tears its side down when
            # this peer disconnects, so we do not block on a bidirectional
            # close; give the half-close a brief moment to flush, then exit.
            session.half_close()
            try:
                await asyncio.wait_for(session.done.wait(), timeout=2)
            except asyncio.TimeoutError:
                pass
        if session.is_done():
            session.get_status().raise_if_not_ok()
    finally:
        signalling.close()


def main() -> None:
    logging.use_absl_handler()
    logging.set_verbosity(logging.INFO)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "server_identity", help="Identity of the WebRTC peer to dial"
    )
    parser.add_argument(
        "signalling_url", help="WebSocket signalling URL, e.g. ws://host:port"
    )
    args = parser.parse_args()
    asyncio.run(run(args.server_identity, args.signalling_url))


if __name__ == "__main__":
    main()
