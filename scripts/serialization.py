import asyncio
from typing import Sequence

from absl import app
from absl import logging

from a11.data import types
from a11.net.websocket_wire_stream import (
    WebSocketClientOptions,
    WebSocketServerOptions,
    WebSocketWireServer,
    WebSocketWireStream,
)
from a11.net.wire_stream import WireStreamWithRecv


async def main(_args: Sequence[str]):
    accepted = asyncio.get_running_loop().create_future()

    async def on_stream(stream: WebSocketWireStream) -> None:
        receiver = WireStreamWithRecv(stream)
        await receiver.accept()
        accepted.set_result(receiver)

    server_options = WebSocketServerOptions()
    server_options.path = "/stream"
    server = WebSocketWireServer.create(on_stream, server_options)
    try:
        client_options = WebSocketClientOptions()
        client_options.headers = {"x-client": "request"}
        client_stream = WireStreamWithRecv(
            WebSocketWireStream.connect(
                f"ws://127.0.0.1:{server.port}/stream",
                websocket_options=client_options,
            )
        )
        await client_stream.start()
        server_stream = await accepted

        logging.info("[client] sending message")
        client_stream.send(
            types.WireMessage(
                node_fragments=[types.NodeFragment(data=types.Chunk())]
            )
        )
        message = await server_stream.receive()
        logging.info("[server] message: %s", message)
        server_stream.send(message)
        logging.info("[client] message: %s", await client_stream.receive())

        client_stream.half_close({"x-client-half-close-success": b"true"})
        server_stream.half_close({"x-server-half-close-success": b"true"})
        await asyncio.gather(
            client_stream.drain_outgoing_messages(),
            server_stream.drain_outgoing_messages(),
        )
    finally:
        server.stop()


def sync_main(argv: Sequence[str]):
    asyncio.run(main(argv))


if __name__ == "__main__":
    app.run(sync_main)
