# Copyright 2026 The A11 Authors.

"""Health probe for the demo server.

Connects to the demo server at a WebSocket URL, calls the ``echo`` action,
and verifies the round trip.  On failure, sends an email via SMTP (Resend).

Intended to run as a Kubernetes CronJob in the same cluster as the relay,
so that it catches demo-server and relay failures independently of the
host machine.

Usage::

    python -m a11.demos.probe \\
        --url wss://a11.to/ws/demoserver \\
        --smtp-url smtps://resend:KEY@smtp.resend.com \\
        --mail-to helenapankov@pm.me

Environment variables ``PROBE_SMTP_URL`` and ``PROBE_MAIL_TO`` are accepted
as defaults when the flags are not given.
"""

from __future__ import annotations

import argparse
import asyncio
import email.message
import os
import smtplib
import socket
import ssl
import sys
import traceback
from urllib.parse import urlparse

import a11
from a11 import net, timing
from a11.client.connection import websocket_client_options
from a11.demos.echo_server import ECHO_SCHEMA
from a11.service.session import Session

#: How long the entire probe is allowed to take.
PROBE_TIMEOUT = timing.Duration.seconds(30)

#: Sender address for alert emails.
MAIL_FROM = "A11 Demo Probe <onboarding@resend.dev>"


# ---------------------------------------------------------------------- probe


async def probe_echo(url: str) -> str | None:
    """Connect and call ``echo``.  Return ``None`` on success, else error."""

    try:
        deadline = timing.now() + PROBE_TIMEOUT
        # Force HTTP/1.1: a reverse proxy (nginx) in front of the relay
        # does not support RFC 8441 extended CONNECT, answering h2
        # WebSocket attempts with a bare 400.
        ws_opts = websocket_client_options(deadline)
        opts = net.WireStreamOptions()

        stream = net.WebSocketWireStream.connect(
            url, opts, websocket_options=ws_opts
        )
        session = Session(action_registry=a11.ActionRegistry())
        await session.add_stream(stream, mode="start")

        call = (
            a11.Action(ECHO_SCHEMA)
            .bind_node_map(session.node_map)
            .bind_session(session)
            .bind_stream(stream)
        )
        await call.call()
        await call["input"].finalize("probe")
        reply = await call["output"].consume(str)
        await asyncio.wait_for(
            call.wait(), timeout=PROBE_TIMEOUT.float_seconds()
        )

        if reply != "probe":
            return f"echo mismatch: expected 'probe', got {reply!r}"

        # Clean shutdown.
        stream.half_close()
        await stream.drain_outgoing_messages()
        stream.abort(
            a11.Status(
                code=a11.StatusCode.CANCELLED, message="probe done"
            )
        )
        return None

    except Exception:
        return traceback.format_exc()


# --------------------------------------------------------------- email alert


def _parse_smtp_url(url: str):
    """Parse ``smtp[s]://user:pass@host[:port]``."""

    parsed = urlparse(url)
    host = parsed.hostname or "smtp.resend.com"
    use_ssl = parsed.scheme in ("smtps", "submissions")
    port = parsed.port or (465 if use_ssl else 587)
    user = parsed.username or ""
    password = parsed.password or ""
    return host, port, user, password, use_ssl


def send_alert(
    smtp_url: str, mail_to: str, subject: str, body: str
) -> None:
    """Send a plain-text alert email via SMTP."""

    host, port, user, password, use_ssl = _parse_smtp_url(smtp_url)

    msg = email.message.EmailMessage()
    msg["From"] = MAIL_FROM
    msg["To"] = mail_to
    msg["Subject"] = subject
    msg.set_content(body)

    ctx = ssl.create_default_context()
    if use_ssl:
        with smtplib.SMTP_SSL(host, port, context=ctx, timeout=15) as smtp:
            if user:
                smtp.login(user, password)
            smtp.send_message(msg)
    else:
        with smtplib.SMTP(host, port, timeout=15) as smtp:
            smtp.starttls(context=ctx)
            if user:
                smtp.login(user, password)
            smtp.send_message(msg)


# -------------------------------------------------------------------- main


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--url",
        default="wss://a11.to/ws/demoserver",
        help="WebSocket URL of the demo server.",
    )
    parser.add_argument(
        "--smtp-url",
        default=os.environ.get("PROBE_SMTP_URL", ""),
        help="SMTP URL (smtps://user:pass@host).",
    )
    parser.add_argument(
        "--mail-to",
        default=os.environ.get("PROBE_MAIL_TO", ""),
        help="Recipient address for failure alerts.",
    )
    args = parser.parse_args()

    error = asyncio.run(
        asyncio.wait_for(
            probe_echo(args.url),
            timeout=PROBE_TIMEOUT.float_seconds() + 5,
        )
    )

    if error is None:
        print("OK", flush=True)
        sys.exit(0)

    hostname = socket.gethostname()
    subject = f"[A11 probe] demo server unreachable from {hostname}"
    body = (
        f"The echo probe against {args.url} failed.\n\n"
        f"Host: {hostname}\n"
        f"Error:\n{error}\n"
    )
    print(body, file=sys.stderr, flush=True)

    print(f"FAIL: {error}", file=sys.stderr, flush=True)

    if args.smtp_url and args.mail_to:
        try:
            send_alert(args.smtp_url, args.mail_to, subject, body)
            print(f"Alert sent to {args.mail_to}", flush=True)
        except Exception as exc:
            print(
                f"Failed to send alert: {exc}",
                file=sys.stderr,
                flush=True,
            )

    sys.exit(1)


if __name__ == "__main__":
    main()
