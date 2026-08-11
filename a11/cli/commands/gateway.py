# Copyright 2026 The A11 Authors.

"""``a11 gateway``: run the gateway, or manage one running in the background.

Subcommands, with a bare ``a11 gateway`` still meaning ``run`` so the way it has
always been invoked keeps working:

* ``run`` -- serve in the foreground.
* ``start [--detach]`` -- serve, optionally in the background.
* ``stop`` -- stop a detached gateway.
* ``status`` -- is one running? Exits 0 if yes, 1 if no, so it is usable in a
  shell test.
* ``logs [-n N] [-f]`` -- what a detached gateway has been saying.

Every subcommand's output is scriptable. Tables and colour when someone is
watching, ``key=value`` lines when stdout is a pipe; see
[a11.cli.console][a11.cli.console]. Nothing is reachable *only* through the
decorated form.
"""

from __future__ import annotations

import argparse
import asyncio
import contextlib
import pathlib
import signal
from typing import Iterator

from absl import logging

from a11 import net
from a11.cli import console as console_module
from a11.cli.app import Command
from a11.gateway import config, conversations, daemon


def _add_serving_flags(parser: argparse.ArgumentParser) -> None:
    """Flags shared by the bare command, ``run`` and ``start``."""
    parser.add_argument(
        "--host",
        type=str,
        default=config.DEFAULT_HOST,
        help="Host address to bind the service to.",
    )
    parser.add_argument(
        "--a11-port",
        type=int,
        default=config.DEFAULT_A11_PORT,
        help="Port for the A11 protocol (WebSockets).",
    )
    parser.add_argument(
        "--listen",
        action="append",
        default=None,
        metavar="URL",
        help=(
            "Additional endpoint to serve the same service on; repeatable."
            " Accepts ws://host:port/path. One service, however many endpoints"
            " it answers on."
        ),
    )
    parser.add_argument(
        "--conversation-store-root",
        type=pathlib.Path,
        default=conversations.default_root(),
        help="Where to store conversation data.",
    )
    parser.add_argument(
        "--no-shell-tools", action="store_true", help="Disable shell tools."
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
    parser.add_argument(
        "--no-flow-tools",
        action="store_true",
        help="Disable the flow tools (flow_actions, flow_check, flow_run).",
    )


def _configure(parser: argparse.ArgumentParser) -> None:
    console_module.add_plain_flag(parser)
    # On the bare command too, so `a11 gateway --a11-port 9000` keeps working
    # without naming a subcommand.
    _add_serving_flags(parser)

    sub = parser.add_subparsers(
        title="subcommands", metavar="<subcommand>", dest="subcommand"
    )

    run = sub.add_parser("run", help="Serve in the foreground.")
    _add_serving_flags(run)
    console_module.add_plain_flag(run)

    start = sub.add_parser("start", help="Serve, optionally detached.")
    _add_serving_flags(start)
    console_module.add_plain_flag(start)
    start.add_argument(
        "--detach",
        "-d",
        action="store_true",
        help=(
            "Run in the background and return once it is listening. Manage it"
            " afterwards with `a11 gateway status|logs|stop`."
        ),
    )

    stop = sub.add_parser("stop", help="Stop a detached gateway.")
    console_module.add_plain_flag(stop)
    stop.add_argument(
        "--timeout",
        type=float,
        default=daemon.STOP_TIMEOUT_SECONDS,
        help="Seconds to wait for a clean exit before forcing it.",
    )

    status = sub.add_parser("status", help="Report whether one is running.")
    console_module.add_plain_flag(status)
    status.add_argument(
        "--probe",
        action="store_true",
        help=(
            "Also complete a ping round trip, so a recycled pid or a wedged"
            " process does not read as healthy."
        ),
    )

    logs = sub.add_parser("logs", help="Show a detached gateway's output.")
    console_module.add_plain_flag(logs)
    logs.add_argument(
        "-n",
        "--lines",
        type=int,
        default=40,
        help="How many trailing lines to show (0 for all).",
    )
    logs.add_argument(
        "-f",
        "--follow",
        action="store_true",
        help="Keep printing new lines as they are written.",
    )


@contextlib.contextmanager
def _stop_on_signals() -> Iterator[asyncio.Event]:
    """An event set by ``SIGINT``/``SIGTERM``, so a signal is a clean shutdown.

    Installing these matters for more than tidiness: the native runtime installs
    Abseil's failure-signal handler, which treats a plain ``SIGTERM`` as a crash
    and dumps a stack trace before the process dies with the server still
    listening. Handling the signal on the loop takes those two back, and the
    handlers are removed again on the way out so nothing outlives the command.

    It is also what makes ``a11 gateway stop`` work at all, since that sends
    SIGTERM and expects a clean exit.
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


def _websocket_options(url: str) -> net.WebSocketServerOptions:
    """Server options for one ``ws://host:port/path`` endpoint."""
    from a11.net import http as net_http

    parsed = net_http.parse_url(url)
    if parsed.scheme not in ("ws", "http"):
        raise ValueError(
            f"--listen accepts ws:// or http:// endpoints, got {parsed.scheme}"
        )
    options = net.WebSocketServerOptions()
    options.path = parsed.path or config.DEFAULT_PATH
    options.bind_address = parsed.host
    options.port = parsed.port
    # The gateway speaks RFC 6455 over HTTP/1.1, and clients match this; see
    # a11.client.connection.
    options.http2_options.enable_h2 = False
    options.http2_options.enable_h2c = False
    return options


def _endpoints(args: argparse.Namespace) -> list[str]:
    """The endpoint URLs to serve, in order.

    ``--host``/``--a11-port`` is the shorthand for the default endpoint and is
    always served; ``--listen`` adds more.
    """
    settings = config.GatewayConfig.from_args(args)
    urls = [settings.url]
    for extra in getattr(args, "listen", None) or []:
        if extra not in urls:
            urls.append(extra)
    return urls


async def _serve(args: argparse.Namespace) -> int:
    """Run the gateway in the foreground until signalled."""
    from a11.gateway import app
    from a11.service.serving import serving, websocket

    settings = config.GatewayConfig.from_args(args)
    gateway = app.init_app(settings)
    listeners = [_websocket_options(url) for url in _endpoints(args)]

    async with serving(
        gateway.service, *[websocket(options) for options in listeners]
    ) as live:
        # Read back from the live listeners rather than from the options, so a
        # requested port of 0 reports the port actually chosen.
        served = [
            f"ws://{options.bind_address}:{server.port}{options.path}"
            for options, server in zip(listeners, live)
        ]
        for url in served:
            logging.info("[gateway] listening on %s", url)
        logging.info(
            "[gateway] shell tools %s, audio capture %s, speech recognition"
            " %s, flow tools %s",
            "on" if settings.shell_tools else "off",
            "on" if settings.audio_capture else "off",
            "on" if settings.speech_recognition else "off",
            "on" if settings.flow_tools else "off",
        )
        logging.info(
            "[gateway] conversations in %s", settings.conversation_store_root
        )
        with daemon.recorded(settings, served), _stop_on_signals() as stop:
            await stop.wait()
        logging.info("[gateway] stopping")
    return 0


def _detach(args: argparse.Namespace) -> int:
    """``start --detach``: spawn, wait until listening, report."""
    out = console_module.console()
    passthrough: list[str] = [
        "--host",
        args.host,
        "--a11-port",
        str(args.a11_port),
        "--conversation-store-root",
        str(args.conversation_store_root),
    ]
    for extra in getattr(args, "listen", None) or []:
        passthrough += ["--listen", extra]
    for flag in (
        "no_shell_tools",
        "no_audio_capture",
        "no_speech_recognition",
        "no_flow_tools",
    ):
        if getattr(args, flag, False):
            passthrough.append("--" + flag.replace("_", "-"))

    try:
        started = daemon.spawn(passthrough)
    except RuntimeError as error:
        out.print(f"error: {error}", style="red", markup=False)
        return 1
    console_module.print_fields(
        started.as_fields(), title="gateway started", target=out
    )
    return 0


def _stop(args: argparse.Namespace) -> int:
    out = console_module.console()
    try:
        stopped = daemon.stop(timeout=args.timeout)
    except RuntimeError as error:
        out.print(f"error: {error}", style="red", markup=False)
        return 1
    console_module.print_fields(
        {"stopped": True, "pid": stopped.pid, "url": stopped.url}, target=out
    )
    return 0


async def _status(args: argparse.Namespace) -> int:
    out = console_module.console()
    current = (
        await daemon.probed_status()
        if getattr(args, "probe", False)
        else daemon.status()
    )
    console_module.print_fields(current.as_fields(), title="gateway", target=out)
    if current.stale:
        out.print(
            "note: a stale record was removed", style="yellow", markup=False
        )
    # The exit code is the answer, so `a11 gateway status && ...` works.
    return 0 if current.running else 1


def _logs(args: argparse.Namespace) -> int:
    out = console_module.console()
    # Raw lines always: a log is not something to decorate.
    if args.follow:
        with contextlib.suppress(KeyboardInterrupt):
            for line in daemon.follow_logs(args.lines):
                out.print(line, markup=False, highlight=False)
        return 0
    lines = daemon.read_logs(args.lines)
    if not lines:
        out.print(
            f"no gateway log at {daemon.log_file()}", style="dim", markup=False
        )
        return 1
    for line in lines:
        out.print(line, markup=False, highlight=False)
    return 0


async def _run(args: argparse.Namespace) -> int:
    console_module.set_plain(getattr(args, "plain", False))
    subcommand = getattr(args, "subcommand", None) or "run"

    if subcommand == "status":
        return await _status(args)
    if subcommand == "stop":
        return _stop(args)
    if subcommand == "logs":
        return _logs(args)
    if subcommand == "start" and getattr(args, "detach", False):
        return _detach(args)
    return await _serve(args)


GATEWAY_COMMAND = Command(
    name="gateway",
    help="Run or manage the A11 gateway.",
    description=(
        "Start the API for a unified access point to shell tools and other"
        " features, or manage one running in the background."
    ),
    configure=_configure,
    run=_run,
)
