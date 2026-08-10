import argparse
import asyncio
import contextlib
import pathlib
import signal
from typing import Iterator

from absl import logging

from a11 import net
from a11.cli.app import Command
from a11.gateway import conversations


def _configure(parser: argparse.ArgumentParser) -> None:
    parser.add_argument(
        "--host",
        type=str,
        default="127.0.0.1",
        help="Host address to bind the service to.",
    )
    parser.add_argument(
        "--a11-port",
        type=int,
        default=8011,
        help="Port for the A11 protocol (Websockets).",
    )
    parser.add_argument(
        "--management-port",
        type=int,
        default=8012,
        help="Port for the management API.",
    )

    parser.add_argument(
        "--conversation-store-root",
        type=pathlib.Path,
        default=conversations.default_root(),
        help="Where to store conversation data.",
    )

    parser.add_argument(
        "--no-shell-tools",
        action="store_true",
        help="Disable shell tools.",
    )
    parser.add_argument(
        "--no-audio-capture",
        action="store_true",
        help="Disable audio capture actions.",
    )
    parser.add_argument(
        "--no-speech-recognition",
        action="store_true",
        help="Disable transcription actions.",
    )


@contextlib.contextmanager
def _stop_on_signals() -> Iterator[asyncio.Event]:
    """An event set by ``SIGINT``/``SIGTERM``, so a signal is a clean shutdown.

    Installing these matters for more than tidiness: the native runtime installs
    Abseil's failure-signal handler, which treats a plain ``SIGTERM`` as a crash
    and dumps a stack trace before the process dies with the server still
    listening. Handling the signal on the loop takes those two back, and the
    handlers are removed again on the way out so nothing outlives the command.
    """
    loop = asyncio.get_running_loop()
    stop = asyncio.Event()
    installed: list[signal.Signals] = []
    for number in (signal.SIGINT, signal.SIGTERM):
        try:
            loop.add_signal_handler(number, stop.set)
            installed.append(number)
        except NotImplementedError:
            # Not a POSIX loop; the KeyboardInterrupt path in `cli.app` remains.
            pass
    try:
        yield stop
    finally:
        for number in installed:
            loop.remove_signal_handler(number)


async def _run(args: argparse.Namespace) -> int:
    from a11.gateway import app

    options = net.WebSocketServerOptions()
    options.path = "/a11"
    options.bind_address = args.host
    options.port = args.a11_port

    options.http2_options.enable_h2 = False
    options.http2_options.enable_h2c = False

    gateway = app.init_app(args)
    server = net.WebSocketWireServer.create(gateway.handle_stream, options)

    try:
        with _stop_on_signals() as stop:
            logging.info(
                "[gateway] Listening on"
                f" ws://{options.bind_address}:{options.port}{options.path}"
            )
            await stop.wait()
    finally:
        logging.info("Stopping server.")
        server.stop()

    return 0


GATEWAY_COMMAND = Command(
    name="gateway",
    help="Run the A11 gateway.",
    description=(
        "Start the API for a unified access point to shell tools and other"
        " features."
    ),
    configure=_configure,
    run=_run,
)
