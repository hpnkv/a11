# Copyright 2026 The A11 Authors.

"""The Python-facing protocol for the native `Service`.

A `Service` is the action registry plus the sessions serving it, with no opinion
about where those sessions come from. `Service.accept` is shaped to be a
transport's on-stream callback, which is what lets one service be bound to
several listeners, or to none at all.

The class exported here is the native ``a11._native.Service``; this module
attaches the asyncio-shaped completion and context-manager conveniences via
[attach_protocol][a11._native_protocol.attach_protocol].
"""

from __future__ import annotations

import asyncio

from a11 import timing
from a11._native_options import install_native_options
from a11._native_protocol import attach_protocol

from a11._native import Service
from a11._native import ServiceOptions

# Native descriptors captured before ``attach_protocol`` overwrites them.
_native_wait_done = Service.wait_done
_native_drain = Service.drain

install_native_options(
    ServiceOptions,
    {
        "copy_registry_per_connection": (bool, False),
        "drain_timeout": (timing.Duration, timing.Duration.seconds(30)),
    },
)
ServiceOptions.__module__ = "a11.service.service"


class _ServiceDoneEvent:
    """Stable asyncio.Event-shaped view of a native completion future."""

    __slots__ = ("_future", "_service")

    def __init__(self, service: Service) -> None:
        self._service = service
        self._future = _native_wait_done(service)

    def is_set(self) -> bool:
        return not self._service.accepting and self._service.session_count == 0

    async def wait(self) -> bool:
        # One cancelled Python waiter must not cancel the service-wide native
        # completion future shared by every caller.
        await asyncio.shield(self._future)
        return True


def _done(service: Service) -> _ServiceDoneEvent:
    event = service.__dict__.get("_a11_done_event")
    if event is None:
        event = _ServiceDoneEvent(service)
        service.__dict__["_a11_done_event"] = event
    return event


class _ServiceProtocol:
    """Completion, draining and context-manager protocol for a Service."""

    @property
    def done(self) -> _ServiceDoneEvent:
        """An `asyncio.Event`-shaped view of the service being closed and empty.

        Set once the service has stopped accepting *and* every session it was
        serving has finished.
        """
        return _done(self)

    async def drain(self, timeout: timing.Duration | None = None) -> None:
        """Await the completion of every session currently being served.

        Args:
            timeout: How long to wait. ``None`` waits indefinitely.

        Raises:
            StatusException: ``DEADLINE_EXCEEDED`` when sessions remain after
                ``timeout``; they are left running, so follow with `abort` if
                they must go.
        """
        await _native_drain(self, timeout)

    async def aclose(
        self, *, timeout: timing.Duration | None = None
    ) -> None:
        """Stop accepting, then wait for what is in flight.

        The graceful shutdown, in the order that makes it graceful: refusing new
        connections first means the set being waited on cannot grow.
        """
        self.stop_accepting()
        try:
            await self.drain(timeout)
        except Exception:  # noqa: BLE001 - shutdown is best effort
            pass

    async def __aenter__(self) -> "Service":
        return self

    async def __aexit__(self, *exc_info) -> None:
        await self.aclose()


attach_protocol(Service, _ServiceProtocol)

__all__ = ["Service", "ServiceOptions"]
