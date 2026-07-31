"""Regression tests for running a `Session` over a WebSocket wire stream.

These guard the deadlock fixed in ``Session::AddStream``: attaching a stream
used to drive the transport's (potentially blocking) ``Start()``/``Accept()``
on the *calling* thread. For a WebSocket client that blocks until the HTTP/2
CONNECT handshake completes, which stalls the asyncio event loop -- so an
in-process peer whose accept callback needs that same loop can never respond,
and ``add_stream`` hangs forever (the symptom reported for the
``000-websocket-echo`` example).

The scenarios below run client and server on a single event loop in a child
process with a hard wall-clock timeout. That is deliberate: when the bug is
present the loop thread is blocked in native code, so an in-loop
``asyncio.wait_for`` can never fire -- only an out-of-process timeout turns a
regression into a fast failure instead of a hang.
"""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

import pytest

_REPO_ROOT = Path(__file__).resolve().parents[3]
_EXAMPLE_DIR = _REPO_ROOT / "examples" / "000-websocket-echo"

# Generous relative to the sub-second happy path, but short enough that a
# reintroduced deadlock fails the suite promptly rather than hanging CI.
_TIMEOUT_SECONDS = 60


def _run(
    args: list[str], *, cwd: Path, stdin: str
) -> subprocess.CompletedProcess[str]:
    """Run a Python program to completion, failing the test if it hangs."""
    try:
        return subprocess.run(
            [sys.executable, *args],
            cwd=str(cwd),
            input=stdin,
            capture_output=True,
            text=True,
            timeout=_TIMEOUT_SECONDS,
        )
    except subprocess.TimeoutExpired as expired:
        raise AssertionError(
            f"Process hung for >{_TIMEOUT_SECONDS}s (likely an add_stream "
            f"deadlock).\n--- output ---\n{expired.output}"
        ) from expired


# A minimal client+server echo over WebSocket, both driven by one event loop.
# This is the library-level contract behind the example: attaching either side
# must not block the loop the other side runs on.
_SAME_LOOP_ECHO = """
import asyncio, sys

import a11
from a11.data import types
from a11.net.websocket_wire_stream import (
    WebSocketClientOptions,
    WebSocketServerOptions,
    WebSocketWireServer,
    WebSocketWireStream,
)
from a11.service.session import Session, SessionWithRecv


def _msg(text):
    return types.WireMessage(
        node_fragments=[
            types.NodeFragment(
                id="t",
                data=types.Chunk(
                    metadata=types.ChunkMetadata(mimetype="text/plain"),
                    data=text.encode(),
                ),
            )
        ]
    )


async def _accept(stream):
    async def on_message(message, stream, session):
        if message is None:
            stream.half_close()
            return
        session.send(message)

    session = Session(on_stream_message=on_message)
    await session.add_stream(stream, mode="accept")
    await session.done.wait()


async def main():
    options = WebSocketServerOptions()
    options.path = "/ws"
    server = WebSocketWireServer.create(_accept, options)
    try:
        session = SessionWithRecv()
        stream = WebSocketWireStream.connect(
            f"ws://127.0.0.1:{server.port}/ws",
            websocket_options=WebSocketClientOptions(),
        )
        # Would hang here before the fix: this call blocked the loop, so the
        # server's accept callback could never complete the handshake.
        await asyncio.wait_for(session.add_stream(stream, mode="start"), 10)
        session.send(_msg("ping"))
        echo = await asyncio.wait_for(session.receive(), 10)
        assert echo is not None and echo.node_fragments[0].data.data == b"ping"
        session.half_close()
        await asyncio.wait_for(session.done.wait(), 10)
    finally:
        server.stop()
    print("ECHO_OK")


asyncio.run(main())
"""


def test_session_over_websocket_shares_event_loop():
    """A Session client and server echo on one loop without deadlocking."""
    result = _run(["-c", _SAME_LOOP_ECHO], cwd=_REPO_ROOT, stdin="")
    assert result.returncode == 0, result.stderr
    assert "ECHO_OK" in result.stdout


@pytest.mark.skipif(
    not (_EXAMPLE_DIR / "main.py").exists(),
    reason="websocket-echo example not present",
)
def test_example_echoes_then_exits():
    """The example echoes a line and exits cleanly on the ``exit`` command."""
    result = _run(["main.py"], cwd=_EXAMPLE_DIR, stdin="hello\nexit\n")
    assert result.returncode == 0, result.stderr
    assert "Echo" in result.stderr  # absl logging goes to stderr


@pytest.mark.skipif(
    not (_EXAMPLE_DIR / "main.py").exists(),
    reason="websocket-echo example not present",
)
def test_example_exits_on_eof():
    """Closed stdin (Ctrl-D) drains the session and exits instead of hanging."""
    result = _run(["main.py"], cwd=_EXAMPLE_DIR, stdin="")
    assert result.returncode == 0, result.stderr
