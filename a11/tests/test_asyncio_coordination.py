# Copyright 2026 The A11 Authors.

import asyncio

import pytest

from a11._asyncio import _schedule_awaitable_threadsafe


@pytest.mark.asyncio
async def test_completed_future_does_not_enter_the_event_loop() -> None:
    loop = asyncio.get_running_loop()
    ready = loop.create_future()
    ready.set_result(17)
    completed: list[int] = []

    _schedule_awaitable_threadsafe(
        loop, ready, lambda future: completed.append(future.result())
    )

    assert completed == [17]


@pytest.mark.asyncio
async def test_awaitable_completion_does_not_add_a_callback_turn() -> None:
    loop = asyncio.get_running_loop()
    completed: list[int] = []

    async def operation() -> int:
        return 23

    _schedule_awaitable_threadsafe(
        loop, operation(), lambda future: completed.append(future.result())
    )
    await asyncio.sleep(0)
    assert completed == []

    await asyncio.sleep(0)
    assert completed == [23]


@pytest.mark.asyncio
async def test_scheduled_awaitable_cancellation_completes_once() -> None:
    loop = asyncio.get_running_loop()
    started = asyncio.Event()
    completed: list[type[BaseException]] = []

    async def operation() -> None:
        started.set()
        await asyncio.Event().wait()

    def observe(future: asyncio.Future[None]) -> None:
        try:
            future.result()
        except BaseException as exc:
            completed.append(type(exc))

    cancel = _schedule_awaitable_threadsafe(loop, operation(), observe)
    await started.wait()
    cancel()
    for _ in range(4):
        await asyncio.sleep(0)
        if completed:
            break

    assert completed == [asyncio.CancelledError]
