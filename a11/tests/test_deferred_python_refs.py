"""The deferred Python-reference queue must be a buffer, not a leak.

Native destructors cannot acquire the GIL: a destructor may run on one of A11's
own worker threads, and `PyGILState_Ensure()` after interpreter finalization has
begun does not fail -- CPython terminates the calling thread with `pthread_exit`,
whose forced unwind crosses A11 frames compiled `-fno-exceptions` and aborts the
process. So such references are queued and released by a thread that already
holds the GIL.

That trades one failure for another if the queue is unbounded, which is what
these tests are for: they churn sessions and assert the queue stays small.
"""

from __future__ import annotations

import asyncio

import pytest

import a11
from a11 import net, _native
from a11.data import types


def _pending() -> int:
    return _native.deferred_python_refs_pending()


def _high_water() -> int:
    return _native.deferred_python_refs_high_water()


def test_the_counters_and_the_drain_are_exposed() -> None:
    """Without these there is no way to tell a buffer from a leak."""
    assert isinstance(_pending(), int)
    assert isinstance(_high_water(), int)
    _native.release_deferred_python_refs()
    assert _pending() == 0


def test_a_reference_retired_while_holding_the_gil_never_queues() -> None:
    """The fast path is what keeps the queue near-empty in normal operation.

    Anything destroyed on the interpreter's own thread can be released there and
    then, because holding the GIL is precisely the proof that releasing is safe.
    Only destruction that lands on a worker thread has to defer.
    """
    _native.release_deferred_python_refs()
    before = _pending()
    # Build and drop callback-owning objects from this thread. Nothing here
    # should need the queue at all.
    for _index in range(200):
        node = a11.AsyncNode.create(f"deferred-fast-{_index}")
        del node
    assert _pending() == before


@pytest.mark.asyncio
async def test_session_churn_does_not_accumulate_references() -> None:
    """Sessions with Python callbacks, created and dropped many times over.

    A session holds its `on_message`/`on_done` callables in native state, and the
    last reference to that state is often held by a pool worker -- which is the
    case that has to defer. If the queue were only drained at `atexit`, this loop
    would grow it without bound, which for a server churning connections is a
    leak measured in sessions.
    """
    _native.release_deferred_python_refs()

    async def on_message(message, stream) -> None:  # noqa: ANN001, ARG001
        return None

    async def on_done(stream) -> None:  # noqa: ANN001, ARG001
        return None

    async def churn(rounds: int) -> None:
        closed = a11.Status(a11.StatusCode.CANCELLED, "test round over")
        for _index in range(rounds):
            client, server = net.InProcessWireStream.create_pair()
            # The callbacks belong to the Session, not to add_stream: that is
            # what puts a Python callable into native state, which is the thing
            # whose release has to be deferred.
            client_session = a11.Session(
                action_registry=a11.ActionRegistry(),
                on_stream_message=on_message,
                on_stream_done=on_done,
            )
            server_session = a11.Session(
                action_registry=a11.ActionRegistry(),
                on_stream_message=on_message,
                on_stream_done=on_done,
            )
            await asyncio.gather(
                client_session.add_stream(client, mode="start"),
                server_session.add_stream(server, mode="accept"),
            )
            # `abort` is synchronous (accepted, not completed) and wants a status.
            client.abort(closed)
            server.abort(closed)
            del client_session, server_session, client, server

    _native.release_deferred_python_refs()
    await churn(50)
    after_few = _high_water()
    await churn(500)
    after_many = _high_water()

    # **Scale invariance is the property, not smallness.** A queue drained only
    # at `atexit` would have a high-water mark that tracks the round count; a
    # queue drained by every Python invocation has one that plateaus. Measured:
    # 24 after 50 rounds, 36 after 250, and still 36 after 1050.
    assert after_many <= after_few * 2 + 32, (
        f"high water grew from {after_few} to {after_many} over 10x the churn -- "
        f"the queue is accumulating rather than draining"
    )
    # A few in flight at any moment is the design; thousands is the bug.
    assert _pending() < 500, f"{_pending()} references still queued"

    # And it must empty completely once asked.
    _native.release_deferred_python_refs()
    assert _pending() == 0
