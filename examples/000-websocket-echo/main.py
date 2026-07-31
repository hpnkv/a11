import asyncio
from typing import Sequence

from absl import app as absl_app
from absl import logging

import a11
from a11.data import types
from a11.net.websocket_wire_stream import (
    WebSocketClientOptions,
    WebSocketWireStream,
)
from a11.service.session import SessionWithRecv
from a11.status import Status

from server import make_websocket_server


def make_wire_message_with_text(text: str):
    return types.WireMessage(
        node_fragments=[
            types.NodeFragment(
                id="test",
                data=types.Chunk(
                    metadata=types.ChunkMetadata(mimetype="text/plain"),
                    data=text.encode("utf-8"),
                ),
            )
        ]
    )


async def main(_argv: Sequence[str]):
    server = make_websocket_server()
    session = SessionWithRecv()
    try:
        client_options = WebSocketClientOptions()
        client_options.headers = {"hello": "world"}
        stream = WebSocketWireStream.connect(
            f"ws://127.0.0.1:{server.port}/ws",
            websocket_options=client_options,
        )
        await session.add_stream(stream, mode="start")

        try:
            while True:
                try:
                    message = await asyncio.to_thread(input, "> ")
                except EOFError:
                    # Ctrl-D / closed stdin: drain and shut down like "exit"
                    # rather than leaving the session open on ``done.wait()``.
                    session.half_close()
                    break

                if message.casefold() == "exit":
                    session.half_close()
                    break

                session.send(make_wire_message_with_text(message))
                message = await session.receive()
                if message is None:
                    break

                logging.info("[client] Echo: %s", message.debug_string())

        except Exception as exc:
            if session.get_status().is_ok():
                session.abort(Status.from_exception(exc))

        finally:
            await session.done.wait()

        session.get_status().raise_if_not_ok()
    finally:
        server.stop()


def sync_main(argv: Sequence[str]):
    logging.use_absl_handler()
    logging.set_verbosity(logging.INFO)

    a11.observability.configure_langfuse_from_env()

    return asyncio.run(main(argv))


if __name__ == "__main__":
    absl_app.run(sync_main)
