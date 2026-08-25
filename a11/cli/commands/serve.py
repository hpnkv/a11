# Copyright 2026 The A11 Authors.

"""``a11 serve``: expose an Action registry from a module over one or more
transports.

```sh
a11 serve mypkg.actions                      # REGISTRY, WebSocket, defaults
a11 serve mypkg.actions:TOOLS --ws --sse     # a named registry, two endpoints
a11 serve ./examples/demo/main.py            # a file, nothing installed
```

The target names a module either way -- as an import path
(``pkg.subpkg.module``) or as a path to a ``.py`` file -- with an optional
``:SYMBOL`` that defaults to ``REGISTRY``. The module is loaded and the symbol
read; it has to be an
[`ActionRegistry`][a11.actions.registry.ActionRegistry]. Writing one is the
[annotated][a11.actions.annotated] shape:

```python
REGISTRY = ActionRegistry()

@REGISTRY.action
async def summarise(document: str) -> str:
    ...
```

## One service, however many endpoints

Each ``--ws`` / ``--sse`` / ``--webrtc`` group adds a *listener*, and every
listener is bound to the same [`Service`][a11.service.service.Service] -- so an
action's state, its registry and its concurrency limits are shared no matter
which endpoint a caller arrived on. That is
[`serving`][a11.service.serving.serving]'s doing, which also stops the listeners
before draining the service, so nothing new arrives while it is finishing.

One endpoint per group: this is a command, not a load balancer.

## A file, or an import path

Which one was meant is read off the target: a ``.py`` suffix, a path separator,
a leading ``.``/``~``, or a file that is simply there, and it is a path.
Otherwise it is imported the ordinary way. A file is loaded under its own stem
rather than as ``__main__``, so a ``if __name__ == "__main__":`` block stays
asleep and the module's own entry point does not run; and its directory goes on
``sys.path`` the way it would for ``python thatfile.py``, so imports of its
siblings resolve.

## HTTP protocol and TLS

`--h11` (the default), `--h2c` and `--h2` are mutually exclusive and apply to
every HTTP-based endpoint, as do `--cert`/`--privkey`. HTTP/1.1 is the default
because that is what an RFC 6455 WebSocket client speaks, so a browser and
`a11.client` reach the same port without being told anything.

SSE runs on HTTP/1.1 too, at the cost of a second connection: a connection
carries one request and the event stream has it, so the outbound direction gets
one of its own. See
[`HttpSseWireStream`][a11.net.http_sse_wire_stream.HttpSseWireStream].
"""

from __future__ import annotations

import argparse
import asyncio
import importlib
import importlib.util
import os
import pathlib
import sys
import types
from typing import Any

from absl import logging

import a11
from a11 import net
from a11.cli import console as console_module
from a11.cli.app import Command
from a11.cli.signals import stop_on_signals
from a11.status import StatusException

#: Module symbol read when the target names none.
DEFAULT_SYMBOL = "REGISTRY"

#: Where each transport listens when only its ``--<group>`` flag is given.
DEFAULT_HOST = "127.0.0.1"
DEFAULT_WS_PORT = 8011
DEFAULT_WS_PATH = "/a11"
DEFAULT_SSE_PORT = 8012

#: What ``--hosted`` with no name means: let the exchange assign a scoped
#: identity. A sentinel rather than an empty string, so "asked for any" and
#: "did not ask" stay distinguishable in the parsed arguments.
HOSTED_ANY = "*"


class ServeError(Exception):
    """A configuration mistake worth a message rather than a traceback."""


# --- The target --------------------------------------------------------------


def split_target(target: str) -> tuple[str, str]:
    """Split ``MODULE[:SYMBOL]`` into the module part and the symbol.

    Split from the right, and only where the tail is an identifier, so a
    Windows path keeps its drive letter (``C:\\src\\actions.py`` is all module)
    and a dotted path without a symbol keeps all its dots.

    Args:
        target: The command's positional argument.

    Returns:
        The module part, and the symbol -- `DEFAULT_SYMBOL` if none was given.
    """
    head, separator, tail = target.rpartition(":")
    if separator and tail.isidentifier():
        return head, tail
    return target, DEFAULT_SYMBOL


def is_path_target(module: str) -> bool:
    """Whether ``module`` names a file rather than an import path.

    A ``.py`` suffix, a path separator, a leading ``.`` or ``~``, or a file that
    is simply there. ``main.py`` is a file, not the ``py`` submodule of a
    ``main`` package, because nobody has ever meant the latter.
    """
    if module.endswith(".py"):
        return True
    if "/" in module or os.sep in module or (os.altsep and os.altsep in module):
        return True
    if module.startswith((".", "~")):
        return True
    return bool(module) and pathlib.Path(module).expanduser().is_file()


def _load_from_path(module: str) -> types.ModuleType:
    """Load a ``.py`` file as a module, without running it as a script.

    Under its own stem rather than ``__main__``, so a module that guards an
    entry point with ``if __name__ == "__main__":`` does not start it, and
    registered into `sys.modules` before it executes, so anything in it that
    looks itself up (a dataclass, a pickle, a decorator recording its module)
    finds itself. Its directory joins `sys.path` as it would for ``python
    thatfile.py``, so importing a sibling works.
    """
    path = pathlib.Path(module).expanduser()
    if path.is_dir():
        raise ServeError(
            f"{module} is a directory. Name the file inside it"
            f" ({path / '__init__.py'}), or use the package's import path."
        )
    if not path.is_file():
        raise ServeError(f"no such file: {path}")
    if path.suffix != ".py":
        raise ServeError(f"{path} is not a .py file.")

    resolved = path.resolve()
    name = resolved.stem
    if not name.isidentifier():
        raise ServeError(
            f"{path.name} is not usable as a module name; rename it or import"
            " it under a name that is."
        )
    spec = importlib.util.spec_from_file_location(name, resolved)
    if spec is None or spec.loader is None:
        raise ServeError(f"cannot load {path} as a module.")

    loaded = importlib.util.module_from_spec(spec)
    directory = str(resolved.parent)
    if directory not in sys.path:
        sys.path.insert(0, directory)
    previous = sys.modules.get(name)
    sys.modules[name] = loaded
    try:
        spec.loader.exec_module(loaded)
    except BaseException as error:
        # Put back whatever was there: a half-executed module under a name
        # something else owns is worse than no module at all.
        if previous is None:
            sys.modules.pop(name, None)
        else:
            sys.modules[name] = previous
        raise ServeError(f"{path} failed to load: {error!r}") from error
    return loaded


def resolve_registry(target: str) -> tuple[a11.ActionRegistry, str, str]:
    """Load ``MODULE[:SYMBOL]`` and return its registry.

    Args:
        target: An import path (``pkg.subpkg.module``) or a path to a ``.py``
            file, optionally with ``:SYMBOL``; the symbol defaults to
            ``REGISTRY``. See `is_path_target` for how the two are told apart.

    Returns:
        The registry, the module as it was named, and the symbol it came from.

    Raises:
        ServeError: If the module cannot be loaded, has no such symbol, or the
            symbol is not an `ActionRegistry`.
    """
    module_path, symbol = split_target(target)
    if not module_path:
        raise ServeError(
            "give a module to serve, as pkg.subpkg.module[:REGISTRY] or"
            " path/to/module.py[:REGISTRY]"
        )

    if is_path_target(module_path):
        module = _load_from_path(module_path)
    else:
        try:
            module = importlib.import_module(module_path)
        except ImportError as error:
            raise ServeError(
                f"cannot import {module_path!r}: {error}. It has to be"
                " importable from here -- try PYTHONPATH=<dir>, install the"
                " package, or give the path to the file instead."
            ) from error

    found = getattr(module, symbol, None)
    if found is None:
        # The near-miss is worth naming: a module written for its own
        # `asyncio.run` usually calls it `registry`, and the fix is a colon.
        alternatives = sorted(
            name
            for name, value in vars(module).items()
            if not name.startswith("_")
            and isinstance(value, a11.ActionRegistry)
        )
        hint = (
            f" Did you mean {module_path}:{alternatives[0]}?"
            if len(alternatives) == 1
            else (
                f" Registries in that module: {', '.join(alternatives)}."
                if alternatives
                else ""
            )
        )
        raise ServeError(f"{module_path} has no {symbol!r}.{hint}")

    if not isinstance(found, a11.ActionRegistry):
        raise ServeError(
            f"{module_path}:{symbol} is a {type(found).__name__}, not an"
            " ActionRegistry."
        )
    return found, module_path, symbol


# --- Flags -------------------------------------------------------------------


def _configure(parser: argparse.ArgumentParser) -> None:
    parser.add_argument(
        "target",
        metavar="MODULE[:SYMBOL]",
        help=(
            "Module holding the registry to serve: an import path"
            " (mypkg.actions, mypkg.actions:TOOLS) or a path to a .py file"
            f" (./demo/main.py). SYMBOL defaults to {DEFAULT_SYMBOL}."
        ),
    )
    console_module.add_plain_flag(parser)
    parser.add_argument(
        "--loglevel",
        default=None,
        metavar="LEVEL",
        help=(
            "Turn A11's logging on at this level, Python and native alike:"
            " debug, info, warning, error, critical, or a number (negative"
            " selects an Abseil VLOG tier). Off by default, which is why a"
            " bring-up says nothing until you ask it to."
        ),
    )

    websocket = parser.add_argument_group(
        "websocket", "Serve the A11 protocol over WebSocket."
    )
    websocket.add_argument(
        "--ws",
        action="store_true",
        help=(
            "Listen on WebSocket. Implied when no transport is asked for at"
            " all, which is the shortest way to a running service."
        ),
    )
    websocket.add_argument("--ws-host", default=DEFAULT_HOST, metavar="ADDRESS")
    websocket.add_argument(
        "--ws-port",
        type=int,
        default=DEFAULT_WS_PORT,
        metavar="PORT",
        help=f"Default {DEFAULT_WS_PORT}; 0 picks an ephemeral port.",
    )
    websocket.add_argument("--ws-path", default=DEFAULT_WS_PATH, metavar="PATH")

    sse = parser.add_argument_group(
        "sse", "Serve the A11 protocol over HTTP server-sent events."
    )
    sse.add_argument("--sse", action="store_true", help="Listen on HTTP SSE.")
    sse.add_argument("--sse-host", default=DEFAULT_HOST, metavar="ADDRESS")
    sse.add_argument(
        "--sse-port",
        type=int,
        default=DEFAULT_SSE_PORT,
        metavar="PORT",
        help=f"Default {DEFAULT_SSE_PORT}; 0 picks an ephemeral port.",
    )
    sse.add_argument(
        "--sse-connect-path",
        default=None,
        metavar="PATH",
        help="Path a client POSTs to open its event stream.",
    )
    sse.add_argument(
        "--sse-message-path",
        default=None,
        metavar="TEMPLATE",
        help="Path template a client POSTs outbound messages to.",
    )

    webrtc = parser.add_argument_group(
        "webrtc",
        "Serve over WebRTC data channels, reachable through a signalling"
        " server. Opens no port of its own.",
    )
    webrtc.add_argument(
        "--webrtc", action="store_true", help="Listen for WebRTC peers."
    )
    webrtc.add_argument(
        "--webrtc-signalling-server",
        default=None,
        metavar="URL",
        help=(
            "ws:// or wss:// signalling server to register with. Required with"
            " --webrtc."
        ),
    )
    webrtc.add_argument(
        "--webrtc-signalling-identity",
        default=None,
        metavar="NAME",
        help="Identity to register under; peers dial this. Required.",
    )
    webrtc.add_argument(
        "--webrtc-signalling-authorization",
        default=None,
        metavar="CREDENTIAL",
        help=(
            "Bearer credential for the signalling handshake. Prefer --hosted,"
            " which obtains one and keeps it fresh."
        ),
    )

    hosted = parser.add_argument_group(
        "hosted",
        "Serve through an A11 exchange, reachable at a public URL without"
        " accepting inbound connections.",
    )
    hosted.add_argument(
        "--hosted",
        nargs="?",
        default=None,
        const=HOSTED_ANY,
        metavar="IDENTITY",
        help=(
            "Host under this identity on the exchange you are logged in to."
            " Takes a claim, connects to signalling, and keeps both alive;"
            " implies --webrtc. Given without a name, the exchange assigns a"
            " scoped identity of your organization -- disposable, and reclaimed"
            " once nothing hosts it -- and the name it granted is printed."
        ),
    )
    hosted.add_argument(
        "--organization",
        default="",
        metavar="NAME",
        help=(
            "Which organization an assigned identity belongs to, when your"
            " credential can act for more than one."
        ),
    )
    hosted.add_argument(
        "--exchange",
        default="",
        metavar="URL",
        help="Which exchange, when logged in to more than one.",
    )
    hosted.add_argument(
        "--claim-ttl",
        type=int,
        default=None,
        metavar="SECONDS",
        help="Claim lifetime to ask for; the exchange decides the maximum.",
    )

    http = parser.add_argument_group(
        "http", "Protocol and TLS for every HTTP-based endpoint above."
    )
    protocol = http.add_mutually_exclusive_group()
    protocol.add_argument(
        "--h11",
        action="store_true",
        help="HTTP/1.1 only (the default; what RFC 6455 clients speak).",
    )
    protocol.add_argument(
        "--h2c", action="store_true", help="Cleartext HTTP/2 only."
    )
    protocol.add_argument(
        "--h2", action="store_true", help="HTTP/2 over TLS only; needs --cert."
    )
    http.add_argument(
        "--cert",
        type=pathlib.Path,
        default=None,
        metavar="FILE",
        help="PEM certificate chain. Giving it enables TLS.",
    )
    http.add_argument(
        "--privkey",
        type=pathlib.Path,
        default=None,
        metavar="FILE",
        help="PEM private key for --cert.",
    )


def http2_options(args: argparse.Namespace) -> net.Http2Options:
    """Transport options shared by every HTTP-based endpoint.

    Each protocol is set explicitly rather than left at its default, because
    `Http2Options` enables all three and "HTTP/1.1" has to mean *only* that --
    otherwise a request that could be upgraded silently is, and the endpoint no
    longer speaks what it was asked to.

    HTTP/1.1 is the default for both transports. WebSocket needs it for RFC
    6455; SSE runs over it by giving its outbound direction a connection of its
    own, since an HTTP/1.1 connection carries one request and the event stream
    has it. See
    [`HttpSseWireStream`][a11.net.http_sse_wire_stream.HttpSseWireStream].

    Raises:
        ServeError: For a combination that cannot be served: ``--h2`` without a
            certificate, ``--h2c`` with one, or half a TLS identity.
    """
    options = net.Http2Options()

    if (args.cert is None) != (args.privkey is None):
        raise ServeError("--cert and --privkey go together; give both.")
    tls = args.cert is not None
    for label, path in (("--cert", args.cert), ("--privkey", args.privkey)):
        if path is not None and not path.is_file():
            raise ServeError(f"{label} {path} does not exist.")

    if args.h2c:
        if tls:
            raise ServeError(
                "--h2c is cleartext HTTP/2, so it cannot be served with"
                " --cert/--privkey. Use --h2 for HTTP/2 over TLS."
            )
        options.enable_http1 = False
        options.enable_h2 = False
        options.enable_h2c = True
    elif args.h2:
        if not tls:
            raise ServeError(
                "--h2 is HTTP/2 over TLS and needs --cert and --privkey. Use"
                " --h2c for cleartext HTTP/2."
            )
        options.enable_http1 = False
        options.enable_h2 = True
        options.enable_h2c = False
    else:
        # The default, and `--h11` explicitly. Both HTTP/2 forms off: a
        # WebSocket client speaking RFC 6455 needs HTTP/1.1, and leaving h2c
        # enabled lets a prior-knowledge client take a different path to the
        # same port for no reason anyone asked for.
        options.enable_http1 = True
        options.enable_h2 = False
        options.enable_h2c = False

    if tls:
        options.tls.enabled = True
        options.tls.certificate_pem_file = str(args.cert)
        options.tls.key_pem_file = str(args.privkey)
    return options


def _scheme(args: argparse.Namespace, secure: str, plain: str) -> str:
    return secure if args.cert is not None else plain


# --- Listeners ---------------------------------------------------------------


def _websocket_options(args: argparse.Namespace) -> net.WebSocketServerOptions:
    options = net.WebSocketServerOptions()
    options.bind_address = args.ws_host
    options.port = args.ws_port
    options.path = args.ws_path
    options.http2_options = http2_options(args)
    return options


def _sse_options(args: argparse.Namespace) -> net.HttpSseOptions:
    options = net.HttpSseOptions()
    options.http2_options = http2_options(args)
    if args.sse_connect_path is not None:
        options.connect_endpoint = args.sse_connect_path
    if args.sse_message_path is not None:
        options.message_endpoint = args.sse_message_path
    return options


def _follow_the_claim(
    endpoint: Any, service: "a11.Service", live: dict[str, Any]
) -> None:
    """Keep the WebRTC listener on the claim's *current* transport and servers.

    Both of the things `HostedEndpoint` maintains outlive the listener bound to
    them: a reconnected signalling socket is a different transport, and a
    renewed claim carries different relay credentials. A listener built once at
    start-up holds neither, and the resulting failure is silent -- signalling
    up, presence online, new connections quietly not completing.

    Rebinding replaces the listener, which drops the peer connections the old
    one was carrying. The relay's grace period turns that into latency for a
    caller rather than an error, and the alternative is being unreachable.
    """

    # Imported here rather than read from the enclosing scope: the serving
    # helpers are imported inside `_serve`, so a module-level function does not
    # see them, and the failure surfaces only when a reconnect happens -- as
    # `could not re-register <identity>: name 'webrtc' is not defined`, hours
    # in, with the host left registered and serving nothing.
    from a11.service.serving import webrtc

    async def drop_listener() -> None:
        previous = live.pop("webrtc", None)
        if previous is not None:
            try:
                previous.stop()
            except Exception:  # noqa: BLE001 - it may already be gone
                logging.debug(
                    "the previous listener did not stop", exc_info=True
                )

    async def on_transport(transport) -> None:
        # A stopped WebRTC server closes its transport, so the listener is
        # always built against the transport just handed over -- never against
        # `endpoint.transport` read later, which may already have moved on.
        live["webrtc"] = webrtc(
            transport, endpoint.webrtc_configuration()
        )(service)
        logging.info("[serve] bound the WebRTC listener")

    endpoint.on_drop_listener = drop_listener
    endpoint.on_transport = on_transport


async def _signalling_client(
    args: argparse.Namespace,
) -> net.WebSocketSignallingClient:
    """Register with the signalling server the WebRTC group names."""
    if not args.webrtc_signalling_server:
        raise ServeError("--webrtc needs --webrtc-signalling-server URL.")
    if not args.webrtc_signalling_identity:
        raise ServeError("--webrtc needs --webrtc-signalling-identity NAME.")
    options = net.signalling.client_options()
    if args.webrtc_signalling_authorization is not None:
        options.headers = {
            "authorization": args.webrtc_signalling_authorization
        }
    return await net.WebSocketSignallingClient.connect(
        args.webrtc_signalling_server,
        args.webrtc_signalling_identity,
        None,
        options,
    )


async def _hosted_endpoint(args: argparse.Namespace):
    """Take a claim on ``--hosted`` and connect, using the stored credential.

    Returns the exchange client and the live `HostedEndpoint`; both are the
    caller's to close. The endpoint keeps the claim renewed and the signalling
    socket re-registered for as long as it lives, which is what makes a host
    that runs for days stay reachable.

    ``--hosted`` with no name asks the exchange for a scoped identity instead of
    naming one; which it granted is on the endpoint afterwards.
    """
    from a11.client.credentials import CredentialStore
    from a11.client.exchange import ExchangeClient
    from a11.client.hosting import HostedEndpoint

    try:
        credential = CredentialStore().require(args.exchange or None)
    except StatusException as exc:
        raise ServeError(exc.status.message) from exc

    asked_for = None if args.hosted == HOSTED_ANY else args.hosted
    client = ExchangeClient(
        credential.exchange, api_key=credential.api_key
    )
    endpoint = HostedEndpoint(
        client,
        asked_for,
        ttl_seconds=args.claim_ttl,
        signalling_url=credential.signalling_url,
        organization=getattr(args, "organization", ""),
    )
    try:
        await endpoint.start()
    except StatusException as exc:
        await client.aclose()
        what = (
            f"host {asked_for!r}"
            if asked_for
            else "be assigned an identity to host"
        )
        raise ServeError(f"Could not {what}: {exc.status.message}") from exc
    return client, endpoint


def _endpoint_urls(
    args: argparse.Namespace,
    live: dict[str, Any],
    identity: str = "",
) -> dict[str, str]:
    """How to reach each live listener, with the port it actually bound.

    Read back from the listener rather than from the options, so ``--ws-port 0``
    reports the port it was given instead of the zero that was asked for -- and,
    for a hosted endpoint, the identity it was *granted* rather than the one
    asked for, which may have been "any".
    """
    urls: dict[str, str] = {}
    if "ws" in live:
        scheme = _scheme(args, "wss", "ws")
        urls["ws"] = (
            f"{scheme}://{args.ws_host}:{live['ws'].port}{args.ws_path}"
        )
    if "sse" in live:
        scheme = _scheme(args, "https", "http")
        urls["sse"] = f"{scheme}://{args.sse_host}:{live['sse'].port}"
    if "webrtc" in live:
        if args.hosted:
            # What a caller would actually type, rather than where the
            # signalling happens to be. The identity is the one the exchange
            # granted, which is not what was asked for when it assigned it.
            from a11.client.credentials import CredentialStore

            stored = CredentialStore().get(args.exchange or None)
            base = (stored.relay_ws_url if stored else "") or "wss://a11.to/ws"
            urls["hosted"] = f"{base}/{identity or args.hosted}"
        else:
            urls["webrtc"] = (
                f"{args.webrtc_signalling_server} as {live['webrtc'].identity}"
            )
    return urls


# --- Running -----------------------------------------------------------------


async def serve(args: argparse.Namespace) -> int:
    """Import the registry, bind the listeners, run until signalled."""
    from a11.service.serving import http_sse, serving, webrtc, websocket

    registry, module_path, symbol = resolve_registry(args.target)
    actions = registry.list_registered_actions()

    # Nothing asked for means WebSocket: the point of the command is a running
    # service, and every transport being opt-in would make the short form do
    # nothing. Said out loud so it is never a surprise.
    implied = not (args.ws or args.sse or args.webrtc)
    if implied:
        logging.info(
            "[serve] no transport given; defaulting to --ws on %s:%d%s",
            args.ws_host,
            args.ws_port,
            args.ws_path,
        )

    signalling: net.WebSocketSignallingClient | None = None
    endpoint = None
    exchange_client = None
    listeners: dict[str, Any] = {}
    if args.hosted:
        # Hosting supplies the signalling transport, so the WebRTC listener is
        # implied rather than asked for separately: `--hosted` without
        # `--webrtc` should serve, not quietly do nothing.
        exchange_client, endpoint = await _hosted_endpoint(args)
        signalling = endpoint.transport
        # From the endpoint, so the STUN and TURN servers the exchange issued
        # with this claim are the ones actually used.
        listeners["webrtc"] = webrtc(
            signalling, endpoint.webrtc_configuration()
        )
    if args.ws or (implied and not args.hosted):
        listeners["ws"] = websocket(_websocket_options(args))
    if args.sse:
        listeners["sse"] = http_sse(
            args.sse_host, args.sse_port, _sse_options(args)
        )
    if args.webrtc and not args.hosted:
        signalling = await _signalling_client(args)
        listeners["webrtc"] = webrtc(signalling)

    service = a11.Service(action_registry=registry)
    out = console_module.console()
    try:
        async with serving(service, *listeners.values()) as started:
            live = dict(zip(listeners, started))
            if endpoint is not None:
                _follow_the_claim(endpoint, service, live)
            urls = _endpoint_urls(
                args, live, endpoint.identity if endpoint else ""
            )
            for name, url in urls.items():
                logging.info("[serve] listening on %s (%s)", url, name)
            console_module.print_fields(
                {
                    "module": module_path,
                    "registry": symbol,
                    "actions": len(actions),
                    **urls,
                },
                title="a11 serve",
                target=out,
            )
            with stop_on_signals() as stop:
                await stop.wait()
            logging.info("[serve] stopping")
    finally:
        # After `serving`, which stopped the WebRTC server and with it this
        # transport; closing twice is harmless and closing never is a leak.
        if endpoint is not None:
            # Releases the claim as well as the socket, so the identity is
            # free for a replacement immediately rather than at expiry.
            await endpoint.aclose()
        elif signalling is not None:
            signalling.close()
        if exchange_client is not None:
            await exchange_client.aclose()
    return 0


def _enable_logging(level: str | None) -> None:
    """Turn A11's logging on at ``level``, or leave it off.

    `enable` rather than `set_level`: the CLI is not an absl app, so nothing has
    installed a handler or the native bridge, and setting a level alone would
    leave every entry -- Python and C++ -- with nowhere to go.

    Raises:
        ServeError: For a level nobody can read, named so the fix is obvious.
    """
    if level is None:
        return
    try:
        a11.logging.enable(level)
    except (TypeError, ValueError) as error:
        raise ServeError(f"--loglevel: {error}") from error


async def _run(args: argparse.Namespace) -> int:
    console_module.set_plain(getattr(args, "plain", False))
    try:
        _enable_logging(getattr(args, "loglevel", None))
        return await serve(args)
    except ServeError as error:
        console_module.console().print(
            f"error: {error}", style="red", markup=False
        )
        return 2


SERVE_COMMAND = Command(
    name="serve",
    help="Serve an Action registry from a module.",
    description=(
        "Import a module, take its ActionRegistry, and expose it as an A11"
        " service on the transports asked for -- WebSocket, HTTP SSE, WebRTC,"
        " or any combination, all sharing one service."
    ),
    configure=_configure,
    run=_run,
)


__all__ = [
    "DEFAULT_SYMBOL",
    "SERVE_COMMAND",
    "ServeError",
    "http2_options",
    "is_path_target",
    "resolve_registry",
    "serve",
    "split_target",
]
