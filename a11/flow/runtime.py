# Copyright 2026 The A11 Authors.

"""Running a compiled Flow program on A11.

The runtime is native (`cpp/a11/flow/runtime.{h,cc}`): a flow becomes one
action, each `call` in it becomes a nested action on the same session, and each
pipe becomes a fibre copying one node into another as values arrive. What lives
here is the way a Python caller *starts* one and reads it back --
[start][a11.flow.runtime.start] for the streaming path,
[invoke][a11.flow.runtime.invoke] for the convenience of collecting the lot at
the end -- and nothing about the language itself.

The engine provides three behaviours required by streaming compositions:

* **Every output is drained.** An output port of a called action that the flow
  does not read is read and discarded anyway, because an unread output stalls
  the action producing it. `skip` is the explicit spelling of the same thing.
* **A `run` step keeps its nodes off the wire.** A step that runs in this
  process is bound to no stream unless it asks for `tee`, so the intermediate
  streams between two steps of a composition are never replicated to the peer
  that dispatched it. A `nodes` block goes further and keeps them out of the
  session's node map entirely. A `call` step, by contrast, is exactly a step put
  on the stream this flow is attached to.
* **Inputs are closed.** A port the flow feeds is closed when its last writer
  finishes, and one it never feeds is closed immediately, so a callee waiting
  for end-of-input is never left waiting on a port the composition was never
  going to write.
"""

from __future__ import annotations

import asyncio
from collections.abc import Callable, Iterable, Mapping
from typing import Any

from a11 import timing
from a11._native import flow as _flow
from a11.actions.action import Action
from a11.flow.plan import FlowPlan
from a11.nodes.async_node import AsyncNode
from a11.status import Status, StatusCode


def _fail(message: str, code: StatusCode = StatusCode.INVALID_ARGUMENT):
    return Status(code=code, message=message).to_exception()


def _header_bytes(value: Any) -> bytes:
    """A header value as the bytes it goes on the wire as.

    Rendered by the language rather than by ``str()``: a header a flow
    *computes* with ``with "x": expr`` is rendered by the runtime the same way,
    and a value that arrived here instead should not read differently for it.
    """
    if isinstance(value, bytes):
        return value
    return _flow.strformat("%s", [value]).encode()


def make_handler(flow: FlowPlan, dispatch_stream: Any = None) -> Callable:
    """The action handler that runs ``flow``.

    Registering this makes the composition an action like any other: a peer can
    dispatch it, another flow can call it, and an LLM can be offered it as a
    tool, without any of them knowing it is a composition.

    ``dispatch_stream`` is only for a flow a client runs itself over a session
    it already holds; [invoke][a11.flow.runtime.invoke] passes it, and nothing
    that registers a flow as an action needs it.
    """
    return flow.make_handler(dispatch_stream)


class Running:
    """A flow that has been started, and the port nodes it is moving values on.

    A flow's ports are [AsyncNode][a11.nodes.async_node.AsyncNode] values like
    any other action's. Read an output node to receive values as the flow
    produces them.
    [invoke][a11.flow.runtime.invoke]'s dict of lists is a convenience on top
    of this for the callers that want the lot at the end (a tool call, a test,
    a script), not the other way round.

    Inputs work the same way round when they are asked for: a port named in
    [start][a11.flow.runtime.start]'s ``open_inputs`` is handed back on
    ``inputs`` for whoever wants to fill it while the flow runs.
    """

    __slots__ = ("action", "outputs", "inputs", "flow", "_timeout")

    def __init__(
        self,
        flow: FlowPlan,
        action: Action,
        outputs: Mapping[str, AsyncNode],
        timeout: timing.Duration | None,
        inputs: Mapping[str, AsyncNode] | None = None,
    ) -> None:
        self.flow = flow
        #: The action the flow is running as.
        self.action = action
        #: Every declared output port, by name, live.
        self.outputs = dict(outputs)
        #: Every input port ``open_inputs`` left open, by name, live. Writing
        #: one feeds the flow as it runs; closing one is the caller's job, and a
        #: flow reading a port nobody closes waits for it.
        self.inputs = dict(inputs or {})
        self._timeout = timeout

    def __getitem__(self, name: str) -> AsyncNode:
        return self.outputs[name]

    async def wait(self) -> None:
        """Wait for the flow to finish, raising whatever it finished with."""
        await self.action.wait(self._timeout)

    def publish(self, stream: Any, names: Iterable[str] | None = None) -> dict:
        """Mirror the named outputs to ``stream``, and say where they landed.

        Returns the node id of each published port, which is what a peer needs
        to read it: `NodeMap.get(id)` on the other side of the stream is the
        same node. The action is not bound to the stream, preventing local
        action completion from closing the session transport.
        """
        published: dict[str, str] = {}
        for name in self.outputs if names is None else names:
            node = self.outputs[name]
            node.attach_stream(stream)
            published[name] = node.get_id()
        return published

    async def collect(self) -> dict[str, Any]:
        """Wait for the flow, and gather every output port into a dict.

        One value for a `one` port, a list for a `many` one. Reading starts
        before the wait, because an output nobody reads stalls the flow filling
        it -- the same rule that applies to any action's ports.
        """
        collectors = {
            name: asyncio.ensure_future(_collect(node))
            for name, node in self.outputs.items()
        }
        try:
            await self.wait()
        except BaseException:
            for collector in collectors.values():
                collector.cancel()
            raise
        declared = self.flow.outputs
        gathered: dict[str, Any] = {}
        for name, collector in collectors.items():
            values = await collector
            gathered[name] = (
                (values[0] if values else None)
                if declared[name].unary
                else values
            )
        return gathered


async def start(
    flow: FlowPlan,
    inputs: Mapping[str, Any] | None = None,
    *,
    registry: Any = None,
    session: Any = None,
    node_map: Any = None,
    stream: Any = None,
    dispatch_stream: Any = None,
    headers: Mapping[str, Any] | None = None,
    timeout: timing.Duration | None = None,
    action_id: str | None = None,
    open_inputs: Iterable[str] = (),
    publish_to: Any = None,
    **keyword_inputs: Any,
) -> Running:
    """Start ``flow`` and return its live input and output ports.

    Provided inputs are written and closed before this returns. Read outputs
    from ``Running.outputs``, then await
    [Running.wait][a11.flow.runtime.Running.wait] for the final status.

    Ports named by ``open_inputs`` remain available through ``Running.inputs``;
    their writer must close them. Their node ids are ``<action_id>#<port>``, so
    a peer can stream chunks with their original media types into the flow.

    ``publish_to`` attaches an output stream before execution, ensuring it sees
    values produced immediately after startup. Use ``action_id`` when peers
    need deterministic port node ids.

    See [invoke][a11.flow.runtime.invoke] for ``stream`` against
    ``dispatch_stream``.
    """
    inputs = {**(inputs or {}), **keyword_inputs}
    action = Action(
        flow.schema,
        handler=make_handler(flow, dispatch_stream=dispatch_stream),
        registry=registry,
        session=session,
        node_map=node_map,
        stream=stream,
    )
    if action_id is not None:
        action.set_id(action_id)
    for name, value in (headers or {}).items():
        action.set_header(name, _header_bytes(value))

    declared_inputs = flow.inputs
    left_open = frozenset(open_inputs)
    for name in (*inputs, *sorted(left_open)):
        if name not in declared_inputs:
            known = ", ".join(sorted(declared_inputs)) or "none"
            raise _fail(
                f"{flow.name} has no input port {name!r} (declared: {known})."
            )
    both = sorted(left_open.intersection(inputs))
    if both:
        raise _fail(
            f"{flow.name}'s input port {both[0]!r} was given a value and left"
            " open. A port is filled one way or the other: with a value here,"
            " or by whoever holds the node."
        )

    # Create outputs before execution so later readers see the full stream.
    outputs = {
        name: action.get_output(name, bind_stream=False)
        for name in flow.outputs
    }
    # Create open inputs before execution so early writes are retained.
    opened = {
        name: action.get_input(name, bind_stream=False)
        for name in sorted(left_open)
    }
    running = Running(flow, action, outputs, timeout, inputs=opened)
    if publish_to is not None:
        # Attach before execution so the stream receives every output.
        running.publish(publish_to)

    action.run()
    for name, value in inputs.items():
        node = action.get_input(name, bind_stream=False)
        # Enqueue every value first, then collect the confirmations.
        #
        # `await (await node.put(one))` charged two event-loop turns per value:
        # one to enqueue and one to hear that the store had it. Enqueueing is
        # what fixes the order -- the writer's queue is FIFO -- so the
        # confirmations can be collected afterwards, and by then a store that
        # answers inline has already answered, which makes the second await
        # free rather than a turn. Failures are still reported, and still
        # before the flow is allowed to proceed.
        confirmations = [
            await node.put(one)
            for one in (value if isinstance(value, (list, tuple)) else [value])
        ]
        for confirmation in confirmations:
            await confirmation
        await node.finalize()
    for name in declared_inputs:
        if name not in inputs and name not in left_open:
            node = action.get_input(name, bind_stream=False)
            await node.finalize()
    return running


async def invoke(
    flow: FlowPlan,
    inputs: Mapping[str, Any] | None = None,
    *,
    registry: Any = None,
    session: Any = None,
    node_map: Any = None,
    stream: Any = None,
    dispatch_stream: Any = None,
    headers: Mapping[str, Any] | None = None,
    timeout: timing.Duration | None = None,
    **keyword_inputs: Any,
) -> dict[str, Any]:
    """Run ``flow`` once, here, and return its outputs collected.

    The convenience path for tests, scripts, and tool calls: it runs the flow to
    completion and returns every output port keyed by name -- one value for a
    `one` port, a list for a `many` one. Inputs may be keywords or a mapping
    (which is what a port whose name collides with one of the options needs).
    ``registry``, ``session`` and ``node_map`` place the flow in an existing
    runtime, so its calls resolve and dispatch exactly as they would inside a
    server.

    Collecting is the convenience, not the mechanism: a flow's outputs are
    nodes, and [start][a11.flow.runtime.start] hands them over live for a
    caller that would rather read them as they fill.

    The two stream arguments are different questions, and a caller wants exactly
    one of them:

    * ``stream`` runs the flow **as though a peer had dispatched it** over that
      stream. The flow's own ports are mirrored to the peer, which is what a
      server's caller is waiting for.
    * ``dispatch_stream`` runs the flow **as the client's own**, and gives the
      stream only to the calls that go to the peer. The flow's ports stay here.
      This is what a client with a session of its own wants: an action that is
      run locally *and* holds a stream ends that stream when it finishes, after
      which the session can dispatch nothing -- so a client passing ``stream``
      would find its second flow unable to reach the peer at all.
    """
    running = await start(
        flow,
        inputs,
        registry=registry,
        session=session,
        node_map=node_map,
        stream=stream,
        dispatch_stream=dispatch_stream,
        headers=headers,
        timeout=timeout,
        **keyword_inputs,
    )
    return await running.collect()


async def _collect(node: AsyncNode) -> list[Any]:
    # `iter_values`, not `async for value in node`: the latter reads one
    # fragment per await and an await is an event-loop turn, so collecting a
    # 4096-value output cost 4096 turns. Nothing else reads this node -- it is
    # the flow's own output and this function exists to drain it whole -- so the
    # batched iterator's one hazard (fragments already taken from the reader if
    # the iteration is abandoned) cannot arise here.
    gathered: list[Any] = []
    async for value in node.iter_values():
        gathered.append(value)
    return gathered


__all__ = ["Running", "invoke", "make_handler", "start"]
