"""Private asyncio coordination used by the native extension."""

import asyncio
from collections.abc import Awaitable, Callable
from typing import Any


class _NativeFuture(asyncio.Future[Any]):
    """Propagate cancellation before control returns to the caller."""

    __slots__ = ("_on_cancel", "_on_await")

    def __init__(self, loop, on_cancel: Callable[[], None]) -> None:
        super().__init__(loop=loop)
        self._on_cancel: Callable[[], None] | None = on_cancel
        #: Runs once, when somebody first awaits this future and it is not
        #: already resolved. Work that the awaiting thread could do itself
        #: belongs here rather than at the call that created the future: a
        #: chunk store writer flushes here, so awaiting a confirmation
        #: performs the store write on this thread and resolves without an
        #: event-loop turn, while a producer that awaits only admission leaves
        #: the write to the writer's pump.
        self._on_await: Callable[[], None] | None = None

    def cancel(self, msg: Any = None) -> bool:
        cancelled = super().cancel(msg)
        if cancelled:
            on_cancel, self._on_cancel = self._on_cancel, None
            if on_cancel is not None:
                try:
                    on_cancel()
                except BaseException:
                    pass
        return cancelled

    def set_result(self, result: Any) -> None:
        self._on_cancel = None
        super().set_result(result)

    def set_exception(self, exception: BaseException) -> None:
        self._on_cancel = None
        super().set_exception(exception)


class _FlushingNativeFuture(_NativeFuture):
    """A native future that runs `_on_await` before anybody waits on it.

    A separate class, switched on per future by `_flush_before_awaiting`,
    rather than an override on `_NativeFuture`: `__await__` in Python replaces
    a C slot on the busiest path in the runtime, and only futures with a hook
    have anything to do there. The cost of the override sits at the noise floor
    (1.458us against 1.459us for a native await), so this buys little beyond
    keeping the hot path exactly as it was.

    It declares no slots of its own, which keeps the layout identical to the
    base and lets `__class__` be reassigned.
    """

    __slots__ = ()

    def __await__(self):
        on_await, self._on_await = self._on_await, None
        if on_await is not None and not self.done():
            on_await()
        return super().__await__()

    __iter__ = __await__


def _flush_before_awaiting(
    future: asyncio.Future[Any], on_await: Callable[[], None]
) -> None:
    """Arrange for `on_await` to run when `future` is first awaited.

    For work the awaiting caller could do itself -- flushing a chunk store
    writer, say -- which should happen only when the caller waits for the
    result. A resolved future has nothing to wait for and is left alone.
    """
    if future.done():
        return
    future.__class__ = _FlushingNativeFuture
    future._on_await = on_await


def _create_native_future(
    loop: asyncio.AbstractEventLoop, on_cancel: Callable[[], None]
) -> asyncio.Future[Any]:
    return _NativeFuture(loop, on_cancel)


async def _invoke_async(function: Callable[..., Awaitable[Any]], *args: Any):
    """Invoke a Python async override only after reaching its event loop."""

    return await function(*args)


async def _dispatch_session_message(message: Any, stream: Any, session: Any):
    """Default Python Session callback with dynamic method dispatch."""

    if message is not None:
        await session.dispatch_wire_message(message, origin_stream=stream)


def _wrap_async_callback(function: Callable[..., Awaitable[Any]]):
    """Return a real coroutine function around a native Future callback."""

    async def wrapped(*args: Any) -> Any:
        return await function(*args)

    return wrapped


def _complete_future(
    future: asyncio.Future[Any], value: Any, exception: BaseException | None
) -> None:
    if future.done():
        return
    if exception is None:
        future.set_result(value)
    else:
        future.set_exception(exception)


def _schedule_awaitable_threadsafe(
    loop: asyncio.AbstractEventLoop,
    awaitable: Awaitable[Any],
    completion: Callable[[asyncio.Future[Any]], None],
) -> Callable[[], None]:
    """Schedule an awaitable from an A11 fiber or native worker thread.

    Always through ``call_soon_threadsafe``, including when the caller happens
    to be on the loop's own thread. Starting the task directly would schedule
    its first step ahead of a cancellation posted afterwards, so `run()`
    immediately followed by `cancel()` would run the handler it was meant to
    stop; and applying that cancellation directly in turn re-enters native code
    that is already holding the lock it was cancelled from. The post is what
    orders the two and breaks that cycle.
    """

    task: asyncio.Future[Any] | None = None
    cancellation_requested = False

    def start() -> None:
        nonlocal task
        try:
            task = asyncio.ensure_future(awaitable, loop=loop)
        except BaseException as exc:
            failed = loop.create_future()
            failed.set_exception(exc)
            completion(failed)
            return
        task.add_done_callback(completion)
        if cancellation_requested:
            task.cancel()

    def cancel() -> None:
        def apply() -> None:
            nonlocal cancellation_requested
            cancellation_requested = True
            if task is not None:
                task.cancel()

        loop.call_soon_threadsafe(apply)

    loop.call_soon_threadsafe(start)
    return cancel


def _get_python_override_attribute(instance: Any, name: str) -> Any:
    """Read a Python descriptor without recursing into the native base."""

    for cls in type(instance).__mro__:
        if cls.__module__ == "a11._native":
            break
        descriptor = vars(cls).get(name)
        if descriptor is not None:
            return descriptor.__get__(instance, type(instance))
    raise NotImplementedError(f"{type(instance).__name__}.{name} is not set")
