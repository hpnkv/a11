"""The Python-facing protocol for the native `Session`.

A `Session` is A11's connection-scoped runtime: it multiplexes one or
more [WireStream][a11.net.wire_stream.WireStream] transports, dispatches
incoming
[Action][a11.actions.action.Action] calls against a registry, and tracks their
lifetimes so the connection can be drained and closed cleanly. It is the object
you build a server or client agent around -- add a stream, and the session
routes messages to and from action handlers for you.

The class exported here is the native ``a11._native.Session``; this module
attaches the asyncio-shaped completion and receive conveniences via
[attach_protocol][a11._native_protocol.attach_protocol].
"""

from __future__ import annotations

import asyncio

from a11 import _native
from a11._native_protocol import attach_protocol

from a11._native import Session
from a11._native import SessionWithRecv

# Native descriptors captured before ``attach_protocol`` overwrites them.
_native_wait_done = Session.wait_done
_native_receive = SessionWithRecv.receive
_native_receive_with_stream_id = SessionWithRecv.receive_with_stream_id


class _SessionDoneEvent:
    """Stable asyncio.Event-shaped view of a native completion future."""

    __slots__ = ("_future", "_session")

    def __init__(self, session: Session) -> None:
        self._session = session
        self._future = _native_wait_done(session)

    def is_set(self) -> bool:
        return self._session.is_done()

    async def wait(self) -> bool:
        # One cancelled Python waiter must not cancel the Session-wide native
        # completion future shared by every caller.
        await asyncio.shield(self._future)
        return True


def _done(session: Session) -> _SessionDoneEvent:
    event = session.__dict__.get("_a11_done_event")
    if event is None:
        event = _SessionDoneEvent(session)
        session.__dict__["_a11_done_event"] = event
    return event


class _SessionProtocol:
    """Completion protocol shared by every native Session."""

    @property
    def done(self) -> _SessionDoneEvent:
        """An `asyncio.Event`-shaped view of full session completion.

        `Session.is_closed` can become true as soon as shutdown starts. Await
        this event (or ``wait_done``) when streams and actions must all have
        released their runtime state.
        """
        return _done(self)


class _SessionWithRecvProtocol:
    """Adds coroutine ``receive`` methods for pull-style session consumption."""

    async def receive(self, deadline=None):
        """Await the next inbound message, or ``None`` when the session ends.

        Use this when one receive loop handles every attached stream. Choose
        `receive_with_stream_id` when replies or diagnostics must retain their
        transport identity. The optional absolute deadline limits only this
        wait; it does not change the session deadline.
        """
        return await _native_receive(self, deadline)

    async def receive_with_stream_id(self, deadline=None):
        """Await ``(message, stream_id)``, or ``None`` after completion.

        This is the pull-style counterpart to ``OnSessionStreamMessage`` and
        is useful when an agent multiplexes several transports in one loop.
        """
        return await _native_receive_with_stream_id(self, deadline)


attach_protocol(Session, _SessionProtocol)
attach_protocol(SessionWithRecv, _SessionWithRecvProtocol)
Session.__module__ = "a11.service.session"
SessionWithRecv.__module__ = "a11.service.session"

__all__ = ["Session", "SessionWithRecv"]
