# Copyright 2026 The A11 Authors.

"""Handlers for the three Flow Actions.

Each is a thin adapter over [a11.flow][]: read the action's inputs, compile,
and -- for ``flow_run`` -- invoke the composition in the caller's own runtime,
so its calls dispatch exactly as a nested action's would.

Two rules are enforced here rather than by the language, because they are about
who is asking rather than about what a flow means:

* **A flow may only call actions the caller may call.** A composition
  dispatches through the registry, which is under the layer that checks the
  ``x-a11-allowed-llm-actions`` header, so the check is made again here --
  against every call in the submitted source, before anything runs. With no
  header there is no restriction from this layer: a script or a test that
  invokes these handlers directly is not a model being kept to an allow-list.
* **A flow may not call the flow tools.** Composing ``flow_run`` into a flow is
  a way to run something the check above just refused, and a way to recurse.

Each handler narrates its run on the ``user_facing_log`` port, which the LLM
tool runner keeps out of the model's result; see
``a11.sdk.llm.USER_FACING_LOG_PORT``.
"""

from __future__ import annotations

from typing import Any

from absl import logging

import a11
from a11.flow import loads as compile_flows
from a11.flow.lexer import FlowSyntaxError
from a11.flow.plan import Body, CallStep, Program
from a11.flow.runtime import invoke as invoke_flow
from a11.flow.runtime import start as start_flow
from a11.sdk.flow_tools.schemas import FLOW_TOOL_NAMES
from a11.sdk.llm import (
    USER_FACING_LOG_PORT,
    action_name_matches_allowed,
    get_allowed_llm_action_patterns,
)
from a11.status import Status, StatusCode, StatusException

#: What the submitted source is called in an error message.
SOURCE_NAME = "flow"

#: How many characters of a flow a run log quotes.
_SOURCE_LOG_LIMIT = 2000


async def _write_log(action: a11.Action, log: str) -> None:
    """Narrate this run on the user-facing log port, then close it.

    Best effort, and for the same reason as the shell tools': a run log is
    worth nothing next to the result, so a port that will not take it does not
    fail the call.
    """
    node = action[USER_FACING_LOG_PORT]
    try:
        if log:
            await node.put(log, final=True)
        else:
            await node.put_null_final()
        await node.drain_and_close()
    except Exception:
        logging.warning("failed to write the run log", exc_info=True)


def _fenced(source: str) -> str:
    """A flow, fenced for the run log and cut if it is very long."""
    text = source.strip()
    if len(text) > _SOURCE_LOG_LIMIT:
        text = text[:_SOURCE_LOG_LIMIT] + "\n…"
    return f"```\n{text}\n```"


def _compile(source: str) -> Program:
    """Compile submitted source, reporting a syntax error as a status.

    A [FlowSyntaxError][a11.flow.lexer.FlowSyntaxError] already carries the
    line and column and converts to the ``INVALID_ARGUMENT`` a caller should be
    told about, which is exactly what a model needs to fix its own flow.
    """
    try:
        return compile_flows(source, SOURCE_NAME)
    except FlowSyntaxError as error:
        raise error.to_status().to_exception() from error


def _called_actions(program: Program) -> list[str]:
    """Every action any flow in ``program`` calls, in the order first seen.

    Every flow, not only the one about to run: a composition is submitted as
    one text and refused as one text, which is easier to explain than a rule
    that depends on which declaration is reachable from which.
    """
    found: list[str] = []
    for plan in program:
        bodies: list[Body] = [plan.root, *plan.root.nested_bodies()]
        for body in bodies:
            for step in body.steps:
                if isinstance(step, CallStep) and step.action not in found:
                    found.append(step.action)
    return found


def verify_calls(program: Program, patterns: list[str]) -> None:
    """Check every call in ``program`` against what the caller may call.

    Raises:
        StatusException: ``PERMISSION_DENIED``, naming the first action the
            caller is not allowed to reach, before any of them is dispatched.
    """
    declared = set(program.flows)
    for name in _called_actions(program):
        if name in declared:
            # A flow calling another flow in the same source reaches nothing
            # the caller did not itself submit.
            continue
        if name in FLOW_TOOL_NAMES:
            raise Status(
                code=StatusCode.PERMISSION_DENIED,
                message=(
                    f"A flow may not call {name!r}. Write what it would have"
                    " run as part of this flow instead."
                ),
            ).to_exception()
        if patterns and not action_name_matches_allowed(name, patterns):
            raise Status(
                code=StatusCode.PERMISSION_DENIED,
                message=(
                    f"This flow calls {name!r}, which is not an action you are"
                    " allowed to call. Call flow_actions for the ones you may"
                    " compose."
                ),
            ).to_exception()


def describe_composable_actions(
    registry: Any, patterns: list[str]
) -> list[dict[str, Any]]:
    """The actions a flow may name, each with its ports and its verb.

    The ports are the point: an action's *output* port names are what a pipe
    needs on the left of a ``->``, and a tool definition carries only inputs.

    ``runnable`` is the other thing a tool definition cannot say. A flow says
    `run` or `call` and the two are not interchangeable, so which one an action
    takes has to be data rather than a rule to remember: an action registered
    here with a handler is one to `run`, and one registered for its schema
    alone is one to `call`.

    Two kinds of port are left out, because a flow that named one would be
    making a mistake: an autofilled input, which is supplied before the handler
    runs (a tool definition hides these too), and the ``user_facing_log`` port,
    which is narration for whoever is watching rather than a value to move.
    """
    described: list[dict[str, Any]] = []
    for name in sorted(registry.list_registered_actions()):
        if name in FLOW_TOOL_NAMES:
            continue
        if patterns and not action_name_matches_allowed(name, patterns):
            continue
        schema = registry.get_schema(name)
        described.append(
            {
                "action": schema.name,
                "description": schema.description,
                "runnable": _has_handler(registry, name),
                "inputs": [
                    _describe_port(port)
                    for port in schema.inputs.values()
                    if not port.autofills and _composable(port)
                ],
                "outputs": [
                    _describe_port(port)
                    for port in schema.outputs.values()
                    if _composable(port)
                ],
            }
        )
    return described


def _has_handler(registry: Any, name: str) -> bool:
    """Whether the registry can run this action, or only describe it.

    The registry reports a schema-only registration as an *error* rather than
    as ``None``, so asking is how it is found out -- the same way
    [a11.flow.runtime][]'s resolver asks.
    """
    try:
        return registry.get_handler(name) is not None
    except StatusException:
        return False


def _composable(port: Any) -> bool:
    """Whether a port is one a flow has any business naming."""
    return port.name != USER_FACING_LOG_PORT


def _describe_port(port: Any) -> dict[str, Any]:
    """One port, in the terms a flow declares one in."""
    described: dict[str, Any] = {"port": port.name, "type": port.type}
    if not port.unary:
        described["stream"] = True
    if port.required:
        described["required"] = True
    if port.description:
        described["description"] = port.description
    return described


async def flow_actions(action: a11.Action) -> None:
    """Report the actions this caller may compose, and their ports."""
    described = describe_composable_actions(
        action.get_registry(), get_allowed_llm_action_patterns(action)
    )
    await action["actions"].put(described, final=True)
    await action["actions"].drain_and_close()
    names = ", ".join(one["action"] for one in described) or "none"
    await _write_log(
        action,
        f"Listed {len(described)} action(s) a flow may call.\n\n{names}",
    )


async def flow_check(action: a11.Action) -> None:
    """Compile a flow and describe it, without dispatching anything."""
    deadline = a11.get_deadline(action)
    source = await _required_source(action, deadline)
    program = _compile(source)
    verify_calls(program, get_allowed_llm_action_patterns(action))
    described = program.describe()
    await action["plan"].put(described, final=True)
    await action["plan"].drain_and_close()
    flows = ", ".join(one["flow"] for one in described["flows"])
    await _write_log(
        action,
        f"Compiled {len(described['flows'])} flow(s): {flows}."
        f"\n\n{_fenced(source)}",
    )


#: What `flow_run` names the action it runs the composition as, relative to
#: its own id. Public because it is half of the contract in
#: [flow_output_node_id][a11.sdk.flow_tools.handlers.flow_output_node_id].
FLOW_ACTION_SUFFIX = "-flow"


def flow_output_node_id(call_id: str, port: str) -> str:
    """Where a `flow_run` call publishes one of its flow's output ports.

    A flow's outputs are nodes, and `flow_run` mirrors them onto the stream the
    call arrived on, so a caller does not have to wait for the whole
    composition to finish to see what it is producing. The id is worked out
    from the call's own id, which the caller chose, so both ends know it
    without anything being announced:

    ```python
    call = a11.Action(FLOW_RUN_SCHEMA)...
    await call.call()
    replies = session.node_map.get(flow_output_node_id(call.get_id(), "reply"))
    async for token in replies:      # arrives as the model writes it
        ...
    ```

    The collected `result` still lands at the end, for callers -- a model
    making a tool call, most of all -- that only want the lot.
    """
    return a11.Action.make_node_id(call_id + FLOW_ACTION_SUFFIX, port)


async def flow_run(action: a11.Action) -> None:
    """Compile a flow, check what it calls, run it, and return its outputs."""
    deadline = a11.get_deadline(action)

    def remaining() -> a11.Duration:
        # Bounds each input read the way the shell tools do, so a caller that
        # neither supplies nor closes an optional input cannot hang the handler.
        return max(deadline - a11.now(), a11.zero_duration())

    source = await _required_source(action, deadline)
    inputs = await action["inputs"].consume(
        dict, timeout=remaining(), allow_none=True
    )
    wanted = await action["flow"].consume(
        str, timeout=remaining(), allow_none=True
    )

    program = _compile(source)
    verify_calls(program, get_allowed_llm_action_patterns(action))
    plan = _chosen_flow(program, wanted)

    running = await start_flow(
        plan,
        inputs or {},
        registry=action.get_registry(),
        session=action.get_session(),
        # No node map is passed on purpose: the composition gets one of its
        # own, so the values it moves between steps are not replicated to the
        # peer that dispatched this call. Keeping them here is the reason to
        # write a flow rather than call the actions one at a time.
        #
        # Its `call` steps are the exception, and that is what this is: a
        # `call` goes back out on the stream the flow arrived on, so a
        # composition running here can dispatch to the peer that sent it --
        # the peer's own microphone, the peer's own shell. Without the stream
        # the reply fragments have nowhere to land and the call never returns.
        # `run` steps are unaffected; they never touch a stream.
        dispatch_stream=action.get_stream(),
        timeout=remaining(),
        # Fixes the output node ids at what `flow_output_node_id` computes, so
        # the caller can subscribe without being told.
        action_id=action.get_id() + FLOW_ACTION_SUFFIX,
        # The caller's headers are the composition's: a flow declaring
        # `header "x-a11-llm-model" as model` is reading what was sent with
        # this call, and its steps forward them on with `with`. Without this a
        # submitted flow could never be told which model to answer with.
        headers=dict(action.headers),
    )
    # A flow's outputs are nodes, so mirror them to the caller now rather than
    # making it wait for the whole composition: a model's answer should arrive
    # as it is written. `result` below is the same values, collected, for
    # callers that only want the lot.
    stream = action.get_stream()
    if stream is not None:
        running.publish(stream)

    outputs = await running.collect()
    await action["result"].put(outputs, final=True)
    await action["result"].drain_and_close()
    ports = ", ".join(sorted(outputs)) or "no outputs"
    await _write_log(
        action,
        f"Ran the flow `{plan.name}` ({ports}).\n\n{_fenced(source)}",
    )


async def _required_source(action: a11.Action, deadline: a11.Time) -> str:
    """The ``source`` input, which every one of these needs."""
    source = await action["source"].consume(
        str,
        timeout=max(deadline - a11.now(), a11.zero_duration()),
        allow_none=True,
    )
    if not source or not source.strip():
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message="The 'source' input is required: pass the flow's text.",
        ).to_exception()
    return source


def _chosen_flow(program: Program, wanted: str | None):
    """The flow to run: the one asked for, or the first one declared."""
    if not wanted:
        return program.main
    try:
        return program[wanted]
    except KeyError as error:
        raise Status(
            code=StatusCode.NOT_FOUND, message=str(error.args[0])
        ).to_exception() from error


__all__ = [
    "SOURCE_NAME",
    "describe_composable_actions",
    "flow_actions",
    "flow_check",
    "flow_run",
    "verify_calls",
]
