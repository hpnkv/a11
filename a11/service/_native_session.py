"""Python protocol conveniences for native Session implementations."""

from __future__ import annotations

import asyncio

from a11 import _native

Session = _native.Session
SessionWithRecv = _native.SessionWithRecv

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


async def _receive(session: SessionWithRecv, deadline=None):
    return await _native_receive(session, deadline)


async def _receive_with_stream_id(session: SessionWithRecv, deadline=None):
    return await _native_receive_with_stream_id(session, deadline)


Session.__module__ = "a11.service.session"
SessionWithRecv.__module__ = "a11.service.session"
Session.done = property(_done)
SessionWithRecv.receive = _receive
SessionWithRecv.receive_with_stream_id = _receive_with_stream_id

__all__ = ["Session", "SessionWithRecv"]
