"""The Python-facing protocol for the native `Action`.

An `Action` is A11's unit of work: a named, schema-described operation
with typed input and output ports (each an
[AsyncNode][a11.nodes.async_node.AsyncNode]) and a handler that runs
asynchronously, streaming into its outputs as it goes. Actions compose --
a handler can create and ``call`` nested actions -- and can run locally or be
dispatched across a [WireStream][a11.net.wire_stream.WireStream] to another
peer.

The class exported here is the native ``a11._native.Action``; this module
attaches the small Python conveniences layered on top of it (an
`asyncio.Event`-shaped ``done``, header decoding, span helpers, and
live-updating settings) via
[attach_protocol][a11._native_protocol.attach_protocol].
"""

from __future__ import annotations

import asyncio
import inspect
import json
import sys
from collections.abc import Awaitable, Callable, Mapping
from typing import Any, Literal, overload

from a11 import _native
from a11.data import serialization
from a11.data import types
from a11._native_protocol import attach_protocol
from a11.status import StatusException

from a11._native import Action
from a11._native import ActionSettings

# Native descriptors captured before ``attach_protocol`` overwrites them.
_native_get_header = Action.get_header
_native_run = Action.run
_native_log_chunk = Action.log_chunk
_native_settings = Action.__dict__["settings"]

# Langfuse reads a span/observation's input and output from these attributes.
_LANGFUSE_INPUT_ATTR = "langfuse.observation.input"
_LANGFUSE_OUTPUT_ATTR = "langfuse.observation.output"


def _caller_location(depth: int) -> tuple[str | None, int | None]:
    """The file and line ``depth`` frames above this one.

    So a log written by A11's own helpers points at the handler that wrote it
    rather than at the helper: the location is what a consumer displays, and the
    plumbing is never the interesting answer.
    """
    try:
        frame = sys._getframe(depth + 1)
    except ValueError:  # pragma: no cover -- shallower stack than expected
        return None, None
    return frame.f_code.co_filename, frame.f_lineno


async def _log_value(
    action: Action,
    value: Any,
    *,
    level: str | None,
    mimetype: str | None,
    metadata: Mapping[str, bytes | str] | None,
    channel: str | None,
    internal: bool,
    file: str | None,
    lineno: int | None,
) -> None:
    """Turn ``value`` into a chunk and write it to the log port."""
    if isinstance(value, types.Chunk):
        if mimetype is not None:
            raise ValueError(
                "Cannot give a log mimetype for a chunk that already has one"
            )
        chunk = value
    else:
        registry = serialization.get_global_serialization_registry()
        chunk = await registry.to_chunk_async(value, mimetype or "")
    attributes = (
        {
            key: value.encode() if isinstance(value, str) else value
            for key, value in metadata.items()
        }
        if metadata is not None
        else None
    )
    _native_log_chunk(
        action,
        chunk,
        level=level,
        metadata=attributes,
        channel=channel,
        file=file,
        lineno=lineno,
        internal=internal,
    )


class _ActionDoneEvent:
    """Event-shaped view of native Action completion."""

    __slots__ = ("_action",)

    def __init__(self, action: Action) -> None:
        self._action = action

    def is_set(self) -> bool:
        return self._action.is_done()

    async def wait(self) -> bool:
        try:
            await self._action.wait()
        except StatusException:
            # Completion events signal lifecycle, while Action.wait() also
            # reports the operation status. Preserve asyncio.Event semantics.
            pass
        return True


def _done(action: Action) -> _ActionDoneEvent:
    event = action.__dict__.get("_a11_done_event")
    if event is None:
        event = _ActionDoneEvent(action)
        action.__dict__["_a11_done_event"] = event
    return event


def _span_json(value: Any) -> str:
    if isinstance(value, str):
        return value
    return json.dumps(value, default=str)


def _get_settings(action: Action) -> ActionSettings:
    settings = _native_settings.__get__(action, type(action))
    settings.__dict__["_a11_action_owner"] = action
    return settings


def _set_settings(action: Action, settings: ActionSettings) -> None:
    _native_settings.__set__(action, settings)


def _install_live_setting(name: str) -> None:
    """Make an ``ActionSettings`` field write back to its owning Action.

    ``action.settings`` returns a native value; mutating one of its fields must
    propagate to the action it came from (and roll back if the native store
    rejects the change), so each field is a property that re-applies the whole
    settings object to its owner.
    """
    descriptor = ActionSettings.__dict__[name]

    def get(settings: ActionSettings) -> Any:
        return descriptor.__get__(settings, type(settings))

    def set(settings: ActionSettings, value: Any) -> None:
        previous = descriptor.__get__(settings, type(settings))
        descriptor.__set__(settings, value)
        owner = settings.__dict__.get("_a11_action_owner")
        if owner is None:
            return
        try:
            _native_settings.__set__(owner, settings)
        except BaseException:
            descriptor.__set__(settings, previous)
            raise

    setattr(ActionSettings, name, property(get, set))


for _field in (
    "bind_streams_on_inputs_by_default",
    "bind_streams_on_outputs_by_default",
    "clear_inputs_after_run",
    "clear_outputs_after_run",
):
    _install_live_setting(_field)


class _ActionProtocol:
    """Python conveniences layered on the native Action."""

    # ``run_in_background`` exposes the native, non-awaiting ``run`` under a
    # name that makes its fire-and-forget nature explicit; the async ``run``
    # protocol lives in ``a11.actions.action``.
    run_in_background = _native_run

    @property
    def done(self) -> _ActionDoneEvent:
        """An `asyncio.Event`-shaped view of completion (``await
        action.done.wait()``)."""
        return _done(self)

    def add_done_callback(
        self, callback: Callable[["Action"], Any | Awaitable[Any]]
    ) -> asyncio.Task:
        """Invoke ``callback(action)`` once this action completes.

        The callback fires exactly once when the action finishes for any reason
        -- normal completion, a handler error, or cancellation -- because it is
        driven off the same completion view as ``done`` (whose wait swallows the
        operation status). If the action is already done, the callback still
        runs on the next event-loop iteration.

        A synchronous callback runs to completion; one returning an awaitable is
        awaited. Cleanup routines registered here should therefore never assume
        success -- they run on the failure and cancellation paths too, which is
        exactly what makes this the right hook for releasing resources (e.g.
        tearing down a transient shell) tied to the action's lifetime.

        Returns the scheduled ``asyncio.Task`` so the caller can cancel the
        pending callback or await it. Must be called from within a running event
        loop.
        """

        async def _run() -> None:
            await self.done.wait()
            result = callback(self)
            if inspect.isawaitable(result):
                await result

        return asyncio.ensure_future(_run())

    @overload
    def get_header(
        self, name: str, decode: Literal[False] = False
    ) -> bytes | None: ...

    @overload
    def get_header(
        self, name: str, decode: Literal[True] = True
    ) -> str | None: ...

    @overload
    def get_header(
        self, name: str, decode: bool = False
    ) -> bytes | str | None: ...

    def get_header(self, name: str, decode: bool = False) -> bytes | str | None:
        """Return header ``name`` (``None`` if absent); ``decode`` UTF-8 to
        ``str``."""
        value = _native_get_header(self, name)
        if value is not None and decode:
            return value.decode()
        return value

    async def log(
        self,
        value: Any,
        *,
        level: str | None = None,
        mimetype: str | None = None,
        metadata: Mapping[str, bytes | str] | None = None,
        channel: str | None = None,
        internal: bool = False,
        file: str | None = None,
        lineno: int | None = None,
    ) -> None:
        """Log ``value`` on the action's reserved log port.

        The object becomes a chunk exactly as ``node.put(value)`` would make one
        -- a ``str`` is ``text/plain``, ``bytes`` are
        ``application/octet-stream``, anything else is JSON -- and the chunk
        always carries a timestamp. Pass an already-built
        [Chunk][a11.data.types.Chunk] to log it as it is.

        The reserved log port requires no schema declaration or manual drain.
        The action closes it with its other outputs and creates it only when
        used. Only a running action may log; logging before ``run`` or from the
        calling side of ``call`` raises.

        A nested action forwards unclaimed logs through its parent. A root
        action sends them to A11's logger (see [a11.logging][]). Call
        `get_log_node` before the action runs to consume its chunks directly
        and suppress the default route.

        Args:
            value: What to log.
            level: One of `a11._native.LOG_LEVELS`; ``None`` is ``"info"``.
            mimetype: Media-type hint for the serializer.
            metadata: Extra chunk attributes, merged before the arguments above
                -- so an explicit ``level`` wins over one written here.
            channel: A label a consumer can filter on.
            internal: Whether this is A11's own bookkeeping rather than
                something an end user should be shown.
            file: Source file to report; defaults to the caller's.
            lineno: Source line to report; defaults to the caller's.
        """
        if file is None and lineno is None:
            file, lineno = _caller_location(1)
        await _log_value(
            self,
            value,
            level=level,
            mimetype=mimetype,
            metadata=metadata,
            channel=channel,
            internal=internal,
            file=file,
            lineno=lineno,
        )

    async def logf(
        self,
        format: str,
        /,
        *args: Any,
        level: str | None = None,
        metadata: Mapping[str, bytes | str] | None = None,
        channel: str | None = None,
        internal: bool = False,
        file: str | None = None,
        lineno: int | None = None,
    ) -> None:
        """Log ``format % args`` -- percent-style, as `logging` formats.

        ``await action.logf("read %d of %d pages", done, total)``. The
        interpolation happens here rather than in a handler, so a message whose
        level is filtered out still costs only the call. Everything else is
        `log`'s.
        """
        if file is None and lineno is None:
            file, lineno = _caller_location(1)
        await _log_value(
            self,
            format % args if args else format,
            level=level,
            mimetype=None,
            metadata=metadata,
            channel=channel,
            internal=internal,
            file=file,
            lineno=lineno,
        )

    def set_span_input(self, value: Any) -> None:
        """Record this action span's input (Langfuse observation input)."""
        self.set_span_attribute(_LANGFUSE_INPUT_ATTR, _span_json(value))

    def set_span_output(self, value: Any) -> None:
        """Record this action span's output (Langfuse observation output)."""
        self.set_span_attribute(_LANGFUSE_OUTPUT_ATTR, _span_json(value))

    @property
    def settings(self) -> ActionSettings:
        """The action's live `ActionSettings` (field writes propagate back)."""
        return _get_settings(self)

    @settings.setter
    def settings(self, settings: ActionSettings) -> None:
        _set_settings(self, settings)


attach_protocol(Action, _ActionProtocol)
Action.__module__ = "a11.actions.action"

__all__ = ["Action"]
