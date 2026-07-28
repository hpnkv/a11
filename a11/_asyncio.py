"""Private asyncio coordination used by the native extension."""

import asyncio
from collections.abc import Awaitable, Callable
from typing import Any


class _NativeFuture(asyncio.Future[Any]):
    """Propagate cancellation before control returns to the caller."""

    __slots__ = ("_on_cancel",)

    def __init__(self, loop, on_cancel: Callable[[], None]) -> None:
        super().__init__(loop=loop)
        self._on_cancel: Callable[[], None] | None = on_cancel

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
    """Schedule an awaitable from an A11 fiber or external native thread."""

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
