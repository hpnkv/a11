# Copyright 2026 The A11 Authors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

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
