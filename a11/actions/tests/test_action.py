import asyncio
from typing import Any

import pytest

import a11
from a11 import timing
from a11.actions import (
    Action,
    ActionPortSchema,
    ActionRegistry,
    ActionSchema,
    status_from_chunk,
)
from a11.data import types
from a11.net.in_process_wire_stream import InProcessWireStream
from a11.net.wire_stream import OnDone, OnMessage, WireStream
from a11.nodes.async_node import NodeMap
from a11.service.session import Session, SessionOptions
from a11.status import Status, StatusCode, StatusException
from a11.stores.local_chunk_store import LocalChunkStore


def _port(name: str) -> ActionPortSchema:
    return ActionPortSchema(name=name, type="text/plain")


def _session_options(**kwargs: Any) -> SessionOptions:
    return SessionOptions(
        no_stream_timeout=timing.infinite_duration(), **kwargs
    )


async def _confirm(write: asyncio.Future[int]) -> int:
    return await write


class _CloseFailStore(LocalChunkStore):
    def __init__(
        self,
        node_id: str,
        failure: Status,
        *,
        fail_ok: bool = False,
        fail_error: bool = False,
    ) -> None:
        super().__init__(node_id)
        self._failure = failure
        self._fail_ok = fail_ok
        self._fail_error = fail_error

    async def close_writes_with_status(
        self,
        status: Status,
        return_status_if_already_closed: bool = False,
    ) -> Status:
        if (status.is_ok() and self._fail_ok) or (
            not status.is_ok() and self._fail_error
        ):
            raise self._failure.to_exception()
        return await super().close_writes_with_status(
            status, return_status_if_already_closed
        )


class _RecordingWireStream(WireStream):
    def __init__(self, stream_id: str) -> None:
        super().__init__()
        self._id = stream_id
        self.sent = []
        self.send_exception: Exception | None = None

    def send(self, message) -> None:
        if self.send_exception is not None:
            raise self.send_exception
        self.sent.append(message)

    async def start(self, on_message: OnMessage, on_done: OnDone) -> None:
        return

    async def accept(self, on_message: OnMessage, on_done: OnDone) -> None:
        return

    def half_close(self, trailers=None) -> None:
        return

    async def drain_outgoing_messages(self) -> None:
        return

    def abort(self, status: Status) -> None:
        return

    def set_deadline(self, deadline=None) -> None:
        return

    @property
    def deadline(self):
        return timing.infinite_future()

    def get_status(self) -> Status:
        return Status.ok()

    def get_trailers(self):
        return None

    def get_id(self) -> str:
        return self._id

    def get_impl(self):
        return None


@pytest.mark.asyncio
async def test_local_run_streams_data_and_writes_completion_status():
    schema = ActionSchema(
        name="local-success", outputs={"result": _port("result")}
    )

    async def handler(action: Action) -> None:
        await _confirm(await action["result"].put("value", final=True))

    action = Action(schema, handler=handler)
    result = action.get_output("result", bind_stream=False)
    status_node = action.get_output("__status__", bind_stream=False)

    assert action.run() is action
    assert await action.wait() is action
    assert await result.next_object(str) == "value"
    assert await result.next_object(str) is None

    fragment = await status_node.next_fragment()
    assert fragment is not None
    assert status_from_chunk(fragment.get_chunk()).is_ok()
    assert await status_node.next_fragment() is None


@pytest.mark.asyncio
async def test_handler_error_aborts_only_unfinished_outputs_and_sets_status():
    schema = ActionSchema(
        name="local-error",
        outputs={
            "unfinished": _port("unfinished"),
            "finished": _port("finished"),
        },
    )
    failure = Status(code=StatusCode.DATA_LOSS, message="handler failed")

    async def handler(action: Action) -> None:
        await _confirm(await action["finished"].put("kept", final=True))
        raise failure.to_exception()

    action = Action(schema, handler=handler)
    unfinished = action.get_output("unfinished", bind_stream=False)
    finished = action.get_output("finished", bind_stream=False)
    status_node = action.get_output("__status__", bind_stream=False)

    action.run()
    with pytest.raises(StatusException) as raised:
        await action.wait()
    assert raised.value.status == failure

    with pytest.raises(StatusException) as raised:
        await unfinished.next_fragment()
    assert raised.value.status == failure
    assert await finished.next_object(str) == "kept"
    assert await finished.next_object(str) is None

    fragment = await status_node.next_fragment()
    assert fragment is not None
    assert status_from_chunk(fragment.get_chunk()) == failure


@pytest.mark.asyncio
async def test_output_cleanup_is_best_effort_and_cannot_hide_handler_status():
    schema = ActionSchema(
        name="best-effort",
        outputs={"first": _port("first"), "second": _port("second")},
    )
    handler_status = Status(
        code=StatusCode.INVALID_ARGUMENT, message="original failure"
    )
    cleanup_status = Status(
        code=StatusCode.INTERNAL, message="one output failed to abort"
    )

    async def handler(_action: Action) -> None:
        raise handler_status.to_exception()

    node_map = NodeMap(
        lambda node_id: (
            _CloseFailStore(node_id, cleanup_status, fail_error=True)
            if node_id.endswith("#first")
            else LocalChunkStore(node_id)
        )
    )
    action = Action(schema, handler=handler, node_map=node_map)
    first = action.get_output("first", bind_stream=False)
    second = action.get_output("second", bind_stream=False)
    status_node = action.get_output("__status__", bind_stream=False)

    action.run()
    with pytest.raises(StatusException) as raised:
        await action.wait()
    assert raised.value.status == handler_status

    with pytest.raises(StatusException) as raised:
        await second.next_fragment()
    assert raised.value.status == handler_status
    fragment = await status_node.next_fragment()
    assert fragment is not None
    assert status_from_chunk(fragment.get_chunk()) == handler_status
    first.cancel()


@pytest.mark.asyncio
async def test_output_close_failure_becomes_action_status_and_aborts_output():
    schema = ActionSchema(
        name="close-failure", outputs={"result": _port("result")}
    )
    cleanup_status = Status(
        code=StatusCode.INTERNAL, message="output could not close"
    )

    async def handler(_action: Action) -> None:
        return

    node_map = NodeMap(
        lambda node_id: (
            _CloseFailStore(node_id, cleanup_status, fail_ok=True)
            if node_id.endswith("#result")
            else LocalChunkStore(node_id)
        )
    )
    action = Action(schema, handler=handler, node_map=node_map)
    result = action.get_output("result", bind_stream=False)
    status_node = action.get_output("__status__", bind_stream=False)

    action.run()
    with pytest.raises(StatusException) as raised:
        await action.wait()
    assert raised.value.status == cleanup_status
    with pytest.raises(StatusException) as raised:
        await result.next_fragment()
    assert raised.value.status == cleanup_status
    fragment = await status_node.next_fragment()
    assert fragment is not None
    assert status_from_chunk(fragment.get_chunk()) == cleanup_status


@pytest.mark.asyncio
async def test_parent_failure_aborts_nested_action_inputs():
    child_schema = ActionSchema(
        name="child", inputs={"request": _port("request")}
    )
    parent_status = Status(
        code=StatusCode.FAILED_PRECONDITION, message="parent stopped"
    )
    nested_input = None

    async def parent_handler(action: Action) -> None:
        nonlocal nested_input
        child = action.make_nested(child_schema)
        nested_input = child.get_input("request", bind_stream=False)
        raise parent_status.to_exception()

    parent = Action(ActionSchema(name="parent"), handler=parent_handler)
    parent.run()
    with pytest.raises(StatusException) as raised:
        await parent.wait()
    assert raised.value.status == parent_status
    assert nested_input is not None
    with pytest.raises(StatusException) as raised:
        await nested_input.next_fragment()
    assert raised.value.status == parent_status


@pytest.mark.asyncio
async def test_cancelling_parent_recursively_cancels_running_children():
    child_started = asyncio.Event()
    child_holder: list[Action] = []

    async def child_handler(_action: Action) -> None:
        child_started.set()
        await asyncio.Event().wait()

    async def parent_handler(action: Action) -> None:
        child = action.make_nested(ActionSchema(name="child"))
        child.bind_handler(child_handler)
        child_holder.append(child)
        child.run()
        await asyncio.Event().wait()

    parent = Action(ActionSchema(name="parent"), handler=parent_handler)
    parent.run()
    await asyncio.wait_for(child_started.wait(), timeout=1)

    parent.cancel()
    with pytest.raises(StatusException) as parent_error:
        await parent.wait()
    with pytest.raises(StatusException) as child_error:
        await child_holder[0].wait()
    assert parent_error.value.status.code == StatusCode.CANCELLED
    assert child_error.value.status.code == StatusCode.CANCELLED


@pytest.mark.asyncio
async def test_session_limits_root_action_concurrency():
    session = Session(options=_session_options(max_concurrent_root_actions=2))
    release = asyncio.Event()
    two_running = asyncio.Event()
    running = 0
    peak = 0

    async def handler(_action: Action) -> None:
        nonlocal running, peak
        running += 1
        peak = max(peak, running)
        if running == 2:
            two_running.set()
        try:
            await release.wait()
        finally:
            running -= 1

    actions = [
        Action(ActionSchema(name="limited"), handler=handler, session=session)
        for _ in range(5)
    ]
    for action in actions:
        action.run()
    await asyncio.wait_for(two_running.wait(), timeout=1)
    await asyncio.sleep(0)
    assert peak == 2
    assert len(list(session.actions())) == 5

    release.set()
    await session.await_all_actions()
    assert peak == 2
    assert list(session.actions()) == []


@pytest.mark.asyncio
async def test_session_distinguishes_action_deadline_status_from_wait_timeout():
    session = Session(options=_session_options())
    execution_status = Status(
        code=StatusCode.DEADLINE_EXCEEDED,
        message="the handler's own deadline elapsed",
    )

    async def handler(_action: Action) -> None:
        raise execution_status.to_exception()

    action = Action(
        ActionSchema(name="deadline"), handler=handler, session=session
    )
    action.run()

    with pytest.raises(StatusException) as raised:
        await session.await_all_actions()
    assert raised.value.status.code == StatusCode.DEADLINE_EXCEEDED
    assert raised.value.status.message == "1 Actions completed with errors."
    assert raised.value.status.details[0]["status"]["message"] == (
        execution_status.message
    )


@pytest.mark.asyncio
async def test_session_limits_nested_action_concurrency_separately():
    session = Session(options=_session_options(max_concurrent_nested_actions=1))
    first_started = asyncio.Event()
    release = asyncio.Event()
    running = 0
    peak = 0

    async def child_handler(_action: Action) -> None:
        nonlocal running, peak
        running += 1
        peak = max(peak, running)
        first_started.set()
        try:
            await release.wait()
        finally:
            running -= 1

    async def parent_handler(action: Action) -> None:
        children = []
        for _ in range(3):
            child = action.make_nested(
                ActionSchema(name="nested"), propagate_io=False
            )
            child.bind_handler(child_handler)
            child.run()
            children.append(child)
        await asyncio.gather(*(child.wait() for child in children))

    parent = Action(
        ActionSchema(name="parent"),
        handler=parent_handler,
        session=session,
    )
    parent.run()
    await asyncio.wait_for(first_started.wait(), timeout=1)
    await asyncio.sleep(0)
    assert peak == 1

    release.set()
    await parent.wait()
    assert peak == 1


@pytest.mark.asyncio
async def test_bound_resources_can_move_while_action_is_running():
    first_map = NodeMap()
    second_map = NodeMap()
    first_session = Session(options=_session_options())
    second_session = Session(options=_session_options())
    proceed = asyncio.Event()

    schema = ActionSchema(name="dynamic", outputs={"result": _port("result")})

    async def handler(action: Action) -> None:
        await proceed.wait()
        await _confirm(await action["result"].put("moved", final=True))

    action = Action(
        schema,
        handler=handler,
        node_map=first_map,
        session=first_session,
    )
    action.run()
    assert action.id in dict(first_session.actions())

    action.bind_node_map(second_map)
    action.bind_session(second_session)
    assert action.id not in dict(first_session.actions())
    assert action.id in dict(second_session.actions())
    proceed.set()
    await action.wait()

    result_id = Action.make_node_id(action.id, "result")
    status_id = Action.make_node_id(action.id, "__status__")
    assert result_id not in first_map
    assert status_id not in first_map
    assert await second_map.get(result_id).next_object(str) == "moved"
    assert action.id not in dict(second_session.actions())


@pytest.mark.asyncio
async def test_rebinding_stream_moves_existing_action_node_tees():
    first = _RecordingWireStream("first")
    second = _RecordingWireStream("second")
    schema = ActionSchema(
        name="dynamic-stream", inputs={"request": _port("request")}
    )
    action = Action(schema, stream=first)
    request = action.get_input("request", bind_stream=True)

    action.bind_stream(second)
    await _confirm(await request.put("payload", final=True))
    # The tee runs off the writer's state machine, so a confirmed sequence does
    # not mean the mirror has been handed over yet. Draining is the barrier.
    await request.wait_for_buffer_to_drain()

    assert first.sent == []
    assert len(second.sent) == 1
    assert second.sent[0].node_fragments[0].id == request.get_id()


@pytest.mark.asyncio
async def test_registry_can_build_caller_only_action_without_handler():
    registry = ActionRegistry()
    registry.register("remote-only", ActionSchema(name="remote-only"))
    stream = _RecordingWireStream("remote")
    action = registry.make_action("remote-only", stream=stream)

    assert not action.has_handler()
    await action.call()
    assert stream.sent[0].actions[0].name == "remote-only"
    action.cancel()
    with pytest.raises(StatusException) as raised:
        await action.wait()
    assert raised.value.status.code == StatusCode.CANCELLED


@pytest.mark.asyncio
async def test_remote_cancel_send_failure_still_completes_local_cancellation():
    initial_stream = _RecordingWireStream("initial")
    failing_stream = _RecordingWireStream("failing")
    schema = ActionSchema(name="remote", outputs={"result": _port("result")})
    action = Action(schema, stream=initial_stream)
    result = action["result"]
    await action.call()
    action.bind_stream(failing_stream)
    failing_stream.send_exception = ValueError("cannot send cancellation")

    with pytest.raises(StatusException) as raised:
        action.cancel()
    assert raised.value.status.code == StatusCode.UNKNOWN
    assert raised.value.status.message == "cannot send cancellation"

    with pytest.raises(StatusException) as raised:
        await action.wait()
    assert raised.value.status.code == StatusCode.CANCELLED
    with pytest.raises(StatusException) as raised:
        await result.next_fragment()
    assert raised.value.status.code == StatusCode.CANCELLED


@pytest.mark.asyncio
async def test_run_cancelled_before_handler_starts_still_finishes_outputs():
    handler_called = False
    schema = ActionSchema(
        name="cancel-before-start", outputs={"result": _port("result")}
    )

    async def handler(_action: Action) -> None:
        nonlocal handler_called
        handler_called = True

    action = Action(schema, handler=handler)
    result = action["result"]
    action.run()
    action.cancel()

    with pytest.raises(StatusException) as raised:
        await asyncio.wait_for(action.wait(), timeout=1)
    assert raised.value.status.code == StatusCode.CANCELLED
    with pytest.raises(StatusException) as raised:
        await asyncio.wait_for(result.next_fragment(), timeout=1)
    assert raised.value.status.code == StatusCode.CANCELLED
    assert not handler_called


async def _close_session_pair(
    client: Session,
    server: Session,
    client_stream: InProcessWireStream,
    server_stream: InProcessWireStream,
) -> None:
    client.half_close()
    server.half_close()
    await asyncio.gather(
        client_stream.drain_outgoing_messages(),
        server_stream.drain_outgoing_messages(),
    )
    await asyncio.gather(client.done.wait(), server.done.wait())


@pytest.mark.asyncio
async def test_remote_call_has_symmetric_io_and_status():
    schema = ActionSchema(
        name="echo",
        inputs={"request": _port("request")},
        outputs={"response": _port("response")},
    )

    async def handler(action: Action) -> None:
        request = await action["request"].consume(str)
        await _confirm(await action["response"].put(f"{request}!", final=True))

    registry = ActionRegistry()
    registry.register("echo", schema, handler)
    client_stream, server_stream = InProcessWireStream.create_pair()
    client = Session(options=_session_options())
    server = Session(options=_session_options(), action_registry=registry)
    await asyncio.gather(
        client.add_stream(client_stream),
        server.add_stream(server_stream, mode="accept"),
    )

    try:
        action = registry.make_action(
            "echo",
            node_map=client.node_map,
            stream=client_stream,
            session=client,
        )
        await action.call()
        await _confirm(await action["request"].put("hello", final=True))

        assert (await action.wait_for_dispatch()).is_ok()
        assert await action["response"].consume(str) == "hello!"
        assert await action.wait() is action
        assert action.get_status().is_ok()
        assert list(client.actions()) == []
        assert list(server.actions()) == []
    finally:
        await _close_session_pair(client, server, client_stream, server_stream)


@pytest.mark.asyncio
async def test_remote_output_ends_on_drain_without_a_final_fragment():
    """A drained output ends a remote reader, with no final fragment written.

    The handler streams with plain puts and never marks finality, so the only
    thing that can end the caller's read is the closure marker the writer tees
    when it closes.
    """
    schema = ActionSchema(name="stream", outputs={"lines": _port("lines")})

    async def handler(action: Action) -> None:
        await _confirm(await action["lines"].put("first"))
        await _confirm(await action["lines"].put("second"))

    registry = ActionRegistry()
    registry.register("stream", schema, handler)
    client_stream, server_stream = InProcessWireStream.create_pair()
    client = Session(options=_session_options())
    server = Session(options=_session_options(), action_registry=registry)
    await asyncio.gather(
        client.add_stream(client_stream),
        server.add_stream(server_stream, mode="accept"),
    )

    try:
        action = registry.make_action(
            "stream",
            node_map=client.node_map,
            stream=client_stream,
            session=client,
        )
        lines = action["lines"]
        await action.call()

        assert (await action.wait_for_dispatch()).is_ok()
        assert await asyncio.wait_for(lines.next_object(str), timeout=5) == (
            "first"
        )
        assert await asyncio.wait_for(lines.next_object(str), timeout=5) == (
            "second"
        )
        # The closure marker, not a final fragment, is what ends this read.
        assert await asyncio.wait_for(lines.next_object(str), timeout=5) is None
        writer_status = lines.writer.get_status()
        assert writer_status is not None and writer_status.is_ok()
        assert await action.wait() is action
    finally:
        await _close_session_pair(client, server, client_stream, server_stream)


@pytest.mark.asyncio
async def test_remote_input_mirror_closes_when_the_caller_drains_it():
    """The same marker travels caller -> receiver, closing an input mirror."""
    schema = ActionSchema(
        name="sink",
        inputs={"lines": _port("lines")},
        outputs={"count": _port("count")},
    )
    seen: list[str] = []
    drained = asyncio.Event()

    async def handler(action: Action) -> None:
        while (line := await action["lines"].next_object(str)) is not None:
            seen.append(line)
        # Reaching here at all means the mirrored input ended locally.
        drained.set()
        await _confirm(await action["count"].put(str(len(seen)), final=True))

    registry = ActionRegistry()
    registry.register("sink", schema, handler)
    client_stream, server_stream = InProcessWireStream.create_pair()
    client = Session(options=_session_options())
    server = Session(options=_session_options(), action_registry=registry)
    await asyncio.gather(
        client.add_stream(client_stream),
        server.add_stream(server_stream, mode="accept"),
    )

    try:
        action = registry.make_action(
            "sink",
            node_map=client.node_map,
            stream=client_stream,
            session=client,
        )
        await action.call()
        assert (await action.wait_for_dispatch()).is_ok()
        await _confirm(await action["lines"].put("one"))
        await _confirm(await action["lines"].put("two"))
        await action["lines"].close()

        await asyncio.wait_for(drained.wait(), timeout=5)
        assert seen == ["one", "two"]
        assert await action["count"].consume(str) == "2"
        assert await action.wait() is action
    finally:
        await _close_session_pair(client, server, client_stream, server_stream)


@pytest.mark.asyncio
async def test_remote_handler_error_propagates_status_and_output_abort():
    failure = Status(code=StatusCode.DATA_LOSS, message="remote handler failed")
    schema = ActionSchema(
        name="remote-error", outputs={"result": _port("result")}
    )

    async def handler(action: Action) -> None:
        # The Action's own cleanup aborts the output with this status.
        raise failure.to_exception()

    registry = ActionRegistry()
    registry.register("remote-error", schema, handler)
    client_stream, server_stream = InProcessWireStream.create_pair()
    client = Session(options=_session_options())
    server = Session(options=_session_options(), action_registry=registry)
    await asyncio.gather(
        client.add_stream(client_stream),
        server.add_stream(server_stream, mode="accept"),
    )

    try:
        action = registry.make_action(
            "remote-error",
            node_map=client.node_map,
            stream=client_stream,
            session=client,
        )
        result = action["result"]
        await action.call()

        with pytest.raises(StatusException) as raised:
            await action.wait()
        assert raised.value.status == failure
        with pytest.raises(StatusException) as raised:
            await result.next_fragment()
        assert raised.value.status == failure
        assert client_stream.get_status().is_ok()
        assert server_stream.get_status().is_ok()
    finally:
        await _close_session_pair(client, server, client_stream, server_stream)


@pytest.mark.asyncio
async def test_remote_dispatch_failure_does_not_abort_stream():
    registry = ActionRegistry()
    client_stream, server_stream = InProcessWireStream.create_pair()
    client = Session(options=_session_options())
    server = Session(options=_session_options(), action_registry=registry)
    await asyncio.gather(
        client.add_stream(client_stream),
        server.add_stream(server_stream, mode="accept"),
    )

    try:
        action = Action(
            ActionSchema(name="missing"),
            node_map=client.node_map,
            stream=client_stream,
            session=client,
        )
        await action.call()
        with pytest.raises(StatusException) as raised:
            await action.wait()
        assert raised.value.status.code == StatusCode.NOT_FOUND
        assert action.get_dispatch_status() == raised.value.status
        assert client_stream.get_status().is_ok()
        assert server_stream.get_status().is_ok()
        assert list(client.actions()) == []
    finally:
        await _close_session_pair(client, server, client_stream, server_stream)


@pytest.mark.asyncio
async def test_remote_cancel_action_cancels_handler_and_aborts_outputs():
    started = asyncio.Event()
    handler_cancelled = asyncio.Event()
    schema = ActionSchema(
        name="slow",
        inputs={"request": _port("request")},
        outputs={"result": _port("result")},
    )

    async def handler(action: Action) -> None:
        try:
            started.set()
            await asyncio.Event().wait()
        finally:
            handler_cancelled.set()

    registry = ActionRegistry()
    registry.register("slow", schema, handler)
    client_stream, server_stream = InProcessWireStream.create_pair()
    client = Session(options=_session_options())
    server = Session(options=_session_options(), action_registry=registry)
    await asyncio.gather(
        client.add_stream(client_stream),
        server.add_stream(server_stream, mode="accept"),
    )

    try:
        action = registry.make_action(
            "slow",
            node_map=client.node_map,
            stream=client_stream,
            session=client,
        )
        request = action["request"]
        result = action["result"]
        await action.call()
        await action.wait_for_dispatch()
        await asyncio.wait_for(started.wait(), timeout=1)

        action.cancel()
        with pytest.raises(StatusException) as raised:
            await action.wait()
        assert raised.value.status.code == StatusCode.CANCELLED
        await asyncio.wait_for(handler_cancelled.wait(), timeout=1)
        with pytest.raises(StatusException) as raised:
            await result.next_fragment()
        assert raised.value.status.code == StatusCode.CANCELLED
        with pytest.raises(StatusException) as raised:
            await request.next_fragment()
        assert raised.value.status.code == StatusCode.CANCELLED

        for _ in range(100):
            if not list(client.actions()) and not list(server.actions()):
                break
            await asyncio.sleep(0.01)
        assert list(client.actions()) == []
        assert list(server.actions()) == []
        assert client_stream.get_status().is_ok()
        assert server_stream.get_status().is_ok()
    finally:
        await _close_session_pair(client, server, client_stream, server_stream)


@pytest.mark.asyncio
async def test_remote_cancel_immediately_after_call_is_not_lost():
    schema = ActionSchema(
        name="cancel-immediately", outputs={"result": _port("result")}
    )

    async def handler(action: Action) -> None:
        await asyncio.Event().wait()

    registry = ActionRegistry()
    registry.register("cancel-immediately", schema, handler)
    client_stream, server_stream = InProcessWireStream.create_pair()
    client = Session(options=_session_options())
    server = Session(options=_session_options(), action_registry=registry)
    client_start = client.add_stream(client_stream)
    server_start = server.add_stream(server_stream, mode="accept")

    try:
        action = registry.make_action(
            "cancel-immediately",
            node_map=client.node_map,
            stream=client_stream,
            session=client,
        )
        result = action["result"]
        await action.call()
        action.cancel()

        with pytest.raises(StatusException) as raised:
            await asyncio.wait_for(result.next_fragment(), timeout=1)
        assert raised.value.status.code == StatusCode.CANCELLED

        await asyncio.gather(client_start, server_start)
        for _ in range(100):
            if not list(client.actions()) and not list(server.actions()):
                break
            await asyncio.sleep(0.01)
        assert list(client.actions()) == []
        assert list(server.actions()) == []
        assert client_stream.get_status().is_ok()
        assert server_stream.get_status().is_ok()
    finally:
        await _close_session_pair(client, server, client_stream, server_stream)


def _autofill(value: str) -> types.NodeFragment:
    return types.NodeFragment(data=a11.to_chunk(value), continued=False)


@pytest.mark.asyncio
async def test_local_run_applies_input_autofills_before_handler():
    schema = ActionSchema(
        name="autofill-run",
        inputs={
            "prefilled": ActionPortSchema(
                name="prefilled",
                type="text/plain",
                autofills=[
                    types.NodeFragment(
                        data=a11.to_chunk("auto"), continued=True
                    ),
                    _autofill("value"),
                ],
            ),
            "nulled": ActionPortSchema(
                name="nulled", type="text/plain", autofills=[None]
            ),
        },
        outputs={"result": _port("result")},
    )

    async def handler(action: Action) -> None:
        parts = []
        while (value := await action["prefilled"].next_object(str)) is not None:
            parts.append(value)
        nulled = await action["nulled"].next_object(str)
        await _confirm(
            await action["result"].put(
                f"{','.join(parts)}|{nulled}", final=True
            )
        )

    action = Action(schema, handler=handler)
    result = action.get_output("result", bind_stream=False)

    action.run()
    assert await action.wait() is action
    assert await result.next_object(str) == "auto,value|None"


@pytest.mark.asyncio
async def test_run_rejects_autofill_when_input_already_has_data():
    schema = ActionSchema(
        name="autofill-guard",
        inputs={
            "guarded": ActionPortSchema(
                name="guarded", type="text/plain", autofills=[_autofill("auto")]
            )
        },
        outputs={"result": _port("result")},
    )

    async def handler(action: Action) -> None:
        await _confirm(await action["result"].put("ran", final=True))

    action = Action(schema, handler=handler)
    # Smuggle data into the autofilled input before the run applies it.
    guarded = action.get_input("guarded", bind_stream=False)
    await _confirm(await guarded.put("intruder", final=True))

    action.run()
    with pytest.raises(StatusException) as raised:
        await action.wait()
    assert raised.value.status.code == StatusCode.FAILED_PRECONDITION


@pytest.mark.asyncio
async def test_remote_call_sends_caller_autofills_over_the_wire():
    # The receiver treats the input as ordinary; the caller autofills it, and
    # the value travels inside the call's WireMessage.
    server_schema = ActionSchema(
        name="echo",
        inputs={"text": ActionPortSchema(name="text", type="text/plain")},
        outputs={"response": _port("response")},
    )

    async def handler(action: Action) -> None:
        text = await action["text"].consume(str)
        await _confirm(await action["response"].put(f"{text}!", final=True))

    registry = ActionRegistry()
    registry.register("echo", server_schema, handler)
    client_stream, server_stream = InProcessWireStream.create_pair()
    client = Session(options=_session_options())
    server = Session(options=_session_options(), action_registry=registry)
    await asyncio.gather(
        client.add_stream(client_stream),
        server.add_stream(server_stream, mode="accept"),
    )

    try:
        caller_schema = ActionSchema(
            name="echo",
            inputs={
                "text": ActionPortSchema(
                    name="text", type="text/plain", autofills=[_autofill("hi")]
                )
            },
            outputs={"response": _port("response")},
        )
        action = Action(
            caller_schema,
            node_map=client.node_map,
            stream=client_stream,
            session=client,
        )
        # The caller provides no input: the autofill rides the call message.
        await action.call()
        assert (await action.wait_for_dispatch()).is_ok()
        assert await action["response"].consume(str) == "hi!"
        assert await action.wait() is action
        assert action.get_status().is_ok()
    finally:
        await _close_session_pair(client, server, client_stream, server_stream)


@pytest.mark.asyncio
async def test_remote_call_applies_receiver_side_autofills():
    # The receiver autofills 'secret' with its own value; the caller neither
    # knows about nor sends it.
    server_schema = ActionSchema(
        name="srv",
        inputs={
            "secret": ActionPortSchema(
                name="secret", type="text/plain", autofills=[_autofill("shh")]
            )
        },
        outputs={"response": _port("response")},
    )

    async def handler(action: Action) -> None:
        secret = await action["secret"].consume(str)
        await _confirm(
            await action["response"].put(f"got:{secret}", final=True)
        )

    registry = ActionRegistry()
    registry.register("srv", server_schema, handler)
    client_stream, server_stream = InProcessWireStream.create_pair()
    client = Session(options=_session_options())
    server = Session(options=_session_options(), action_registry=registry)
    await asyncio.gather(
        client.add_stream(client_stream),
        server.add_stream(server_stream, mode="accept"),
    )

    try:
        caller_schema = ActionSchema(
            name="srv", outputs={"response": _port("response")}
        )
        action = Action(
            caller_schema,
            node_map=client.node_map,
            stream=client_stream,
            session=client,
        )
        await action.call()
        assert (await action.wait_for_dispatch()).is_ok()
        assert await action["response"].consume(str) == "got:shh"
        assert await action.wait() is action
        assert action.get_status().is_ok()
    finally:
        await _close_session_pair(client, server, client_stream, server_stream)


@pytest.mark.asyncio
async def test_receiver_rejects_a_write_to_its_autofilled_input():
    # A peer that ships a fragment for a receiver-autofilled input (alongside
    # the ActionMessage) is rejected: the receiver applies its autofill first
    # and closes the input, so the smuggled write fails.
    node_map = NodeMap()
    schema = ActionSchema(
        name="guarded-srv",
        inputs={
            "secret": ActionPortSchema(
                name="secret", type="text/plain", autofills=[_autofill("shh")]
            )
        },
        outputs={"response": _port("response")},
    )

    async def handler(action: Action) -> None:
        secret = await action["secret"].consume(str)
        await _confirm(
            await action["response"].put(f"got:{secret}", final=True)
        )

    registry = ActionRegistry()
    registry.register("guarded-srv", schema, handler)
    server = Session(
        options=_session_options(),
        action_registry=registry,
        node_map=node_map,
    )

    action_id = "intruder-call"
    secret_node = Action.make_node_id(action_id, "secret")
    message = types.WireMessage(
        node_fragments=[
            types.NodeFragment(
                id=secret_node, data=a11.to_chunk("evil"), continued=False
            )
        ],
        actions=[registry.make_action_message("guarded-srv", action_id)],
    )

    with pytest.raises(StatusException) as raised:
        await server.dispatch_wire_message(message)

    assert any(
        detail["element_type"] == "node_fragment"
        for detail in raised.value.status.details
    )
