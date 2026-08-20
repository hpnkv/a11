# Copyright 2026 The A11 Authors.

"""Tests for the ``add_done_callback`` primitive added to Action and Session.

The shell tool relies on these to release resources when a parent action
completes or a session ends, so they must fire exactly once on *every*
completion path -- success, failure, and cancellation/abort -- and even when the
target is already done at registration time.
"""

import asyncio

import pytest

from a11.actions import Action, ActionPortSchema, ActionSchema
from a11.service.session import Session, SessionOptions
from a11 import timing
from a11.status import Status, StatusCode, StatusException


def _options() -> SessionOptions:
    return SessionOptions(no_stream_timeout=timing.infinite_duration())


def _result_port() -> ActionPortSchema:
    return ActionPortSchema(name="result", type="text/plain")


@pytest.mark.asyncio
async def test_action_callback_fires_on_success():
    schema = ActionSchema(name="ok", outputs={"result": _result_port()})

    async def handler(action: Action) -> None:
        await action["result"].finalize("v")

    action = Action(schema, handler=handler)
    seen = []
    task = action.add_done_callback(
        lambda a: seen.append(a.get_status().code)
    )
    action.run()
    await action.wait()
    await task
    assert seen == [StatusCode.OK]


@pytest.mark.asyncio
async def test_action_callback_fires_on_handler_error():
    failure = Status(code=StatusCode.DATA_LOSS, message="boom")

    async def handler(action: Action) -> None:
        raise failure.to_exception()

    action = Action(ActionSchema(name="err"), handler=handler)
    seen = []
    task = action.add_done_callback(
        lambda a: seen.append(a.get_status().code)
    )
    action.run()
    with pytest.raises(StatusException):
        await action.wait()
    await task
    assert seen == [StatusCode.DATA_LOSS]


@pytest.mark.asyncio
async def test_action_callback_fires_on_cancellation():
    async def handler(action: Action) -> None:
        await asyncio.Event().wait()

    action = Action(ActionSchema(name="slow"), handler=handler)
    seen = []
    task = action.add_done_callback(lambda a: seen.append("done"))
    action.run()
    await asyncio.sleep(0.01)
    action.cancel()
    with pytest.raises(StatusException):
        await action.wait()
    await task
    assert seen == ["done"]


@pytest.mark.asyncio
async def test_action_callback_runs_when_already_done():
    action = Action(ActionSchema(name="quick"), handler=_noop_handler)
    action.run()
    await action.wait()

    seen = []
    task = action.add_done_callback(lambda a: seen.append("late"))
    await task
    assert seen == ["late"]


@pytest.mark.asyncio
async def test_action_async_callback_is_awaited():
    action = Action(ActionSchema(name="acb"), handler=_noop_handler)
    seen = []

    async def callback(a: Action) -> None:
        await asyncio.sleep(0)
        seen.append("async")

    task = action.add_done_callback(callback)
    action.run()
    await action.wait()
    await task
    assert seen == ["async"]


@pytest.mark.asyncio
async def test_session_callback_fires_on_clean_close():
    session = Session(options=_options())
    seen = []
    task = session.add_done_callback(lambda s: seen.append(s.get_id()))
    session.half_close()
    await asyncio.wait_for(session.done.wait(), timeout=1)
    await task
    assert seen == [session.get_id()]


@pytest.mark.asyncio
async def test_session_callback_fires_on_abort():
    session = Session(options=_options())
    seen = []
    task = session.add_done_callback(lambda s: seen.append("aborted"))
    session.abort(Status(code=StatusCode.DATA_LOSS, message="the session died"))
    await asyncio.wait_for(session.done.wait(), timeout=1)
    await task
    assert seen == ["aborted"]


async def _noop_handler(action: Action) -> None:
    return None
