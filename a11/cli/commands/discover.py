# Copyright 2026 The A11 Authors.

"""``a11 discover`` -- ask a peer what it can do.

The command that exists because every peer can now be asked. It calls
`__list_actions__` (or `__get_schema__`, given a name) over the ordinary A11
protocol, so it works against a gateway, an `a11 serve`, an IDE plugin, or
anything else that speaks A11 -- there is nothing to enable on the far side.

For an HTTP endpoint it does a plain `GET /actions` instead, which is the same
document by construction: the server answers it out of the same describer.
"""

from __future__ import annotations

import argparse
import json
import sys
from typing import Any

from a11.cli import durations
from a11.cli.app import Command

_DESCRIPTION = """\
Ask an A11 peer which actions it serves, and print their schemas.

    a11 discover ws://127.0.0.1:8011/a11
    a11 discover ws://127.0.0.1:8011/a11 shell_execute
    a11 discover http://127.0.0.1:8012 --name 'shell_.*' --json

A `ws://` or `wss://` endpoint is asked over the A11 protocol; an `http://` or
`https://` one is asked with a GET. Both produce the same a11.actions/v1
document, because the server answers both from the same describer.
"""


def _configure(parser: argparse.ArgumentParser) -> None:
    parser.add_argument(
        "endpoint",
        help="The peer to ask: ws://host:port/a11, or http://host:port.",
    )
    parser.add_argument(
        "action",
        nargs="?",
        help="One action to describe. Omit to list everything.",
    )
    parser.add_argument(
        "--name",
        action="append",
        default=[],
        metavar="PATTERN",
        dest="names",
        help=(
            "Only actions whose name fully matches this regex. Repeatable."
        ),
    )
    parser.add_argument(
        "--all-ports",
        action="store_true",
        help=(
            "Keep inputs the peer fills in itself, flagged. They cannot be"
            " written by a caller, so they are hidden by default."
        ),
    )
    parser.add_argument(
        "--reserved",
        action="store_true",
        help="Include A11's own actions, such as __list_actions__ itself.",
    )
    parser.add_argument(
        "--runnable",
        action="store_true",
        help=(
            "Skip actions the peer registered for their schema alone, which"
            " live on some further peer rather than there."
        ),
    )
    parser.add_argument(
        "--json",
        action="store_true",
        dest="as_json",
        help="Print the a11.actions/v1 document rather than a summary.",
    )
    parser.add_argument(
        "--timeout",
        type=durations.duration_seconds,
        default=None,
        metavar="DURATION",
        help="How long to wait, as 30s, 250ms, 1m30s, or seconds.",
    )


def _port_line(port: dict[str, Any], direction: str) -> str:
    """One port, spelled the way a flow spells it."""
    bits = [f"  {direction} {port.get('name', '?')}: {port.get('type', '?')}"]
    flags = []
    if port.get("required"):
        flags.append("required")
    if not port.get("unary", False):
        flags.append("stream")
    if port.get("autofilled"):
        flags.append("autofilled")
    if flags:
        bits.append(f"[{', '.join(flags)}]")
    description = port.get("description")
    if description:
        bits.append(f"-- {description}")
    return " ".join(bits)


def _print_summary(entries: list[dict[str, Any]]) -> None:
    if not entries:
        print("The peer serves no actions matching that.")
        return
    for index, entry in enumerate(entries):
        if index:
            print()
        name = entry.get("name", "?")
        where = "" if entry.get("runnable", True) else "  (lives on a peer)"
        print(f"{name}{where}")
        description = entry.get("description")
        if description:
            print(f"  {description}")
        for port in entry.get("inputs", []):
            print(_port_line(port, "in "))
        for port in entry.get("outputs", []):
            print(_port_line(port, "out"))
        for header in entry.get("headers", []):
            default = " (has a default)" if header.get("has_default") else ""
            print(f"  header {header.get('name', '?')}{default}")


def _http_document(args: argparse.Namespace) -> dict[str, Any]:
    """`GET /actions` on an HTTP endpoint."""
    import urllib.error
    import urllib.parse
    import urllib.request

    base = args.endpoint.rstrip("/")
    # An endpoint given as ".../actions" is what somebody reading the docs would
    # paste; treat it as the base either way.
    if base.endswith("/actions"):
        base = base[: -len("/actions")]
    path = "/actions" + (f"/{urllib.parse.quote(args.action)}" if args.action else "")
    query = [("name", pattern) for pattern in args.names]
    if args.all_ports:
        query.append(("ports", "all"))
    if args.reserved:
        query.append(("reserved", "1"))
    if args.runnable:
        query.append(("runnable", "1"))
    url = base + path + (f"?{urllib.parse.urlencode(query)}" if query else "")
    try:
        with urllib.request.urlopen(url, timeout=args.timeout or 30) as answer:
            return json.loads(answer.read().decode("utf-8"))
    except urllib.error.HTTPError as error:
        body = error.read().decode("utf-8", "replace").strip()
        raise SystemExit(
            f"{args.endpoint} answered {error.code}: {body or error.reason}"
        ) from error


async def _protocol_document(args: argparse.Namespace) -> dict[str, Any]:
    """`__list_actions__` or `__get_schema__` over the A11 protocol."""
    from a11 import timing
    from a11.client import discovery
    from a11.client.connection import GatewayConnection

    bound = (
        timing.Duration.seconds(args.timeout)
        if args.timeout is not None
        else None
    )
    # Connect through the client's own helper rather than rebuilding the dance:
    # it disables h2/h2c for an RFC6455 peer and bounds only the handshake, both
    # of which are load-bearing and commented as such where they live.
    connection = await GatewayConnection.connect(args.endpoint, timeout=bound)
    try:
        if args.action:
            entry = await discovery.fetch_schema(
                connection, args.action, timeout=bound
            )
            return {"format": "a11.actions/v1", "actions": [entry]}
        entries = await discovery.fetch_schemas(
            connection,
            names=args.names,
            all_ports=args.all_ports,
            include_reserved=args.reserved,
            runnable_only=args.runnable,
            timeout=bound,
        )
        return {"format": "a11.actions/v1", "actions": entries}
    finally:
        await connection.aclose()


async def _run(args: argparse.Namespace) -> int:
    from a11.actions import describe
    from a11.status import StatusException

    scheme = args.endpoint.split("://", 1)[0].lower()
    try:
        if scheme in ("http", "https"):
            document = _http_document(args)
        elif scheme in ("ws", "wss"):
            document = await _protocol_document(args)
        else:
            print(
                f"{args.endpoint!r} is not an endpoint this understands; write"
                " ws://, wss://, http:// or https://",
                file=sys.stderr,
            )
            return 2
    except StatusException as error:
        print(f"{args.endpoint}: {error}", file=sys.stderr)
        return 1
    except (OSError, TimeoutError) as error:
        print(f"{args.endpoint}: {error}", file=sys.stderr)
        return 1

    if args.as_json:
        print(json.dumps(document, indent=2, sort_keys=True))
        return 0
    _print_summary(describe.schemas_in_document(document))
    return 0


DISCOVER_COMMAND = Command(
    name="discover",
    help="Ask an A11 peer which actions it serves.",
    description=_DESCRIPTION,
    configure=_configure,
    run=_run,
)

__all__ = ["DISCOVER_COMMAND"]
