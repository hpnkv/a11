import asyncio

from absl import logging

from a11.data import types
from a11.net.websocket_wire_stream import (
    WebSocketServerOptions,
    WebSocketWireServer,
)
from a11.net.wire_stream import WireStream
from a11.service.session import Session


def _make_server_callbacks():
    async def on_session_stream_message(
        message: types.WireMessage | None, stream: WireStream, session: Session
    ):
        if message is None:
            logging.info(
                "[server] client half-closed stream %s.", stream.get_id()
            )
            if session.is_closed():
                logging.log_first_n(
                    logging.INFO, "[server] client half-closed the session.", 1
                )
                session.half_close()

            stream.half_close()
            return

        logging.info("[server] Received message: %s", message.debug_string())
        session.send(message)

    async def on_session_stream_done(stream: WireStream, session: Session):
        session_status = session.get_status()
        stream_status = stream.get_status()
        if stream_status.is_ok():
            logging.info(
                "[server] Stream %s closed successfully.", stream.get_id()
            )
        else:
            logging.error(
                "[server] Stream %s closed with an error: %s.",
                stream.get_id(),
                stream_status,
            )

        if session_status.is_ok():
            logging.log_first_n(
                logging.INFO,
                "[server] Session %s closed.",
                1,
                session.get_id(),
            )
        else:
            logging.log_first_n(
                logging.ERROR,
                "[server] Session %s closed with an error: %s.",
                1,
                session.get_id(),
                session_status,
            )

    return on_session_stream_message, on_session_stream_done


async def _accept_stream(stream: WireStream) -> None:
    on_stream_message, on_stream_done = _make_server_callbacks()
    session = Session(
        on_stream_message=on_stream_message,
        on_stream_done=on_stream_done,
    )
    await session.add_stream(stream, mode="accept")
    await session.done.wait()


def make_websocket_server() -> WebSocketWireServer:
    options = WebSocketServerOptions()
    options.path = "/ws"
    return WebSocketWireServer.create(_accept_stream, options)
