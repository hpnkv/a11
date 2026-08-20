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

Each handler narrates its run through :meth:`a11.actions.action.Action.log`. The
LLM tool runner reads that separately from the action's outputs, so the narration
cannot reach the model's result -- and no schema here declares a port for it.
"""

from __future__ import annotations

from typing import Any

from absl import logging

import a11
from a11.flow import loads as compile_flows
from a11.flow.diagnostics import FlowSyntaxError
from a11.flow.plan import Program
from a11.flow.runtime import invoke as invoke_flow
from a11.flow.runtime import start as start_flow
from a11.sdk.flow_tools.schemas import FLOW_TOOL_NAMES
from a11.sdk.llm import (
    action_name_matches_allowed,
    get_allowed_llm_action_patterns,
)
from a11.status import Status, StatusCode, StatusException

#: What the submitted source is called in an error message.
SOURCE_NAME = "flow"

#: How many characters of a flow a run log quotes.
_SOURCE_LOG_LIMIT = 2000


def _fenced(source: str) -> str:
    """A flow, fenced for the run log and cut if it is very long."""
    text = source.strip()
    if len(text) > _SOURCE_LOG_LIMIT:
        text = text[:_SOURCE_LOG_LIMIT] + "\n..."
    return f"```\n{text}\n```"


def _compile(source: str) -> Program:
    """Compile submitted source, reporting a syntax error as a status.

    A [FlowSyntaxError][a11.flow.diagnostics.FlowSyntaxError] already carries the
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

    def walk(steps: list[dict]) -> None:
        for step in steps:
            action = step.get("action")
            if action is not None and action not in found:
                found.append(action)
            for key in ("body", "then", "else"):
                nested = step.get(key)
                if nested:
                    walk(nested)

    for plan in program:
        walk(plan.describe()["steps"])
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

    One kind of port is left out, because a flow that named it would be making a
    mistake: an autofilled input, which is supplied before the handler runs (a
    tool definition hides these too). Narration needs no exclusion -- an action
    logs through :meth:`a11.actions.action.Action.log`, whose port is not in the
    schema, so there is nothing here to hide.
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
                    if not port.autofills
                ],
                "outputs": [
                    _describe_port(port) for port in schema.outputs.values()
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
    await action["actions"].finalize(described)
    names = ", ".join(one["action"] for one in described) or "none"
    await action.log(
        f"Listed {len(described)} action(s) a flow may call.\n\n{names}",
    )


async def flow_check(action: a11.Action) -> None:
    """Compile a flow and describe it, without dispatching anything."""
    deadline = a11.get_deadline(action)
    source = await _required_source(action, deadline)
    program = _compile(source)
    verify_calls(program, get_allowed_llm_action_patterns(action))
    described = program.describe()
    await action["plan"].finalize(described)
    flows = ", ".join(one["flow"] for one in described["flows"])
    await action.log(
        f"Compiled {len(described['flows'])} flow(s): {flows}."
        f"\n\n{_fenced(source)}",
    )


#: What these handlers name the action they run the composition as, relative to
#: their own id. Public because it is half of the contract in
#: [flow_output_node_id][a11.sdk.flow_tools.handlers.flow_output_node_id] and
#: [flow_input_node_id][a11.sdk.flow_tools.handlers.flow_input_node_id].
FLOW_ACTION_SUFFIX = "-flow"


def flow_output_node_id(call_id: str, port: str) -> str:
    """Where a flow's output port lands, for the caller that dispatched it.

    A flow's outputs are nodes, and `flow_run` mirrors them onto the stream the
    call arrived on, so a caller does not have to wait for the whole composition
    to see what it is producing. The id is worked out from the call's own id,
    which the caller chose, so both ends know it without anything being
    announced:

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


def flow_input_node_id(call_id: str, port: str) -> str:
    """Where a caller writes an input port it named on ``input_streams``.

    The mirror image of
    [flow_output_node_id][a11.sdk.flow_tools.handlers.flow_output_node_id], and
    the same derivation -- a port's node id does not depend on its direction, so
    the two functions agree by construction and exist separately to say which one
    a caller means. (Which is also why a flow cannot declare an input and an
    output of the same name: they would be one node.) A flow's input ports are in
    the session's node map like its outputs, so the caller fills one by writing
    to its id -- and keeps writing, for as long as it has values.

    ```python
    call = a11.Action(FLOW_RUN_SCHEMA)...
    await call.call()
    await call["input_streams"].finalize(["words"])  # this port is mine
    words = session.node_map.get(flow_input_node_id(call.get_id(), "words"))
    words.attach_stream(stream)
    await (await words.put("one"))  # the flow reads it now, not later
    await words.finalize()          # and this is what ends the port
    ```

    Arity is not a second mechanism: a port declared `stream` takes as many
    values as the caller has, an ordinary port takes the one it carries, and an
    empty port is a port that carried none. **The caller owns the close** --
    nothing else will do it, and a flow reading a port nobody closes waits,
    bounded only by the call's own `x-a11-deadline` (with no such header, not at
    all).
    """
    return a11.Action.make_node_id(call_id + FLOW_ACTION_SUFFIX, port)


async def flow_run(action: a11.Action) -> None:
    """Compile a flow, check what it calls, run it, and return its outputs.

    A port is filled one of two ways, and which one is the caller's to choose. A
    value in ``inputs`` is written and the port closed, which is all a model can
    express. A port named on ``input_streams`` is left open for the caller to
    write as a node while the flow runs -- which is how a value reaches a flow
    that has already started, and how a port carrying a real type is fed at all.
    Neither way looks at how many values the port carries.
    """
    deadline = a11.get_deadline(action)

    def remaining() -> a11.Duration:
        # Bounds each input read the way the shell tools do, so a caller that
        # neither supplies nor closes an optional input cannot hang the handler.
        return max(deadline - a11.now(), a11.zero_duration())

    source = await _required_source(action, deadline)
    inputs = await action["inputs"].consume(
        dict, timeout=remaining(), allow_none=True
    )
    open_inputs = _open_input_ports(
        await action["input_streams"].consume(
            list, timeout=remaining(), allow_none=True
        )
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
        # The map this call already uses, which is the session's. Saying so is
        # what lets a caller reach the flow's ports by id -- to read an output
        # as it fills, or to write an input while the flow runs. What keeps a
        # composition's *intermediate* values off the wire is not the map: it is
        # that the flow's action holds no stream and its `run` steps bind none,
        # so nothing between two steps is ever mirrored.
        node_map=action.get_node_map(),
        # Its `call` steps are the exception, and that is what this is: a
        # `call` goes back out on the stream the flow arrived on, so a
        # composition running here can dispatch to the peer that sent it --
        # the peer's own microphone, the peer's own shell. Without the stream
        # the reply fragments have nowhere to land and the call never returns.
        # `run` steps are unaffected; they never touch a stream.
        dispatch_stream=action.get_stream(),
        timeout=remaining(),
        # Fixes every port's node id at what `flow_output_node_id` and
        # `flow_input_node_id` compute, so the caller needs nothing announced.
        action_id=action.get_id() + FLOW_ACTION_SUFFIX,
        # The caller's headers are the composition's: a flow declaring
        # `header "x-a11-llm-model" as model` is reading what was sent with
        # this call, and its steps forward them on with `with`. Without this a
        # submitted flow could never be told which model to answer with.
        headers=dict(action.headers),
        # Left for the caller to fill and to close. Which of them carry one
        # value and which carry many is the flow's business, not this
        # contract's: a port is a port.
        open_inputs=open_inputs,
        # A flow's outputs are nodes, so mirror them to the caller rather than
        # making it wait for the whole composition: a model's answer should
        # arrive as it is written. Attached here, before the flow starts,
        # because attaching a stream does not replay what a writer has already
        # flushed -- a value produced in between would reach `result` and
        # nothing else. `result` is the same values, collected, for callers that
        # only want the lot.
        publish_to=action.get_stream(),
    )

    outputs = await running.collect()
    await action["result"].finalize(outputs)
    ports = ", ".join(sorted(outputs)) or "no outputs"
    filled = ", ".join(sorted(running.inputs))
    fed = f" The caller filled {filled}." if filled else ""
    await action.log(
        f"Ran the flow `{plan.name}` ({ports}).{fed}\n\n{_fenced(source)}",
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


def _open_input_ports(named: list | None) -> tuple[str, ...]:
    """The ports the caller says it will fill itself, checked for shape.

    Only the shape is checked here, because it is all this end knows: whether
    the flow declares such a port, and whether it was also given a value, are
    the runtime's to answer, and it answers them for every caller of
    [start][a11.flow.runtime.start] rather than only for this one.
    """
    if not named:
        return ()
    for value in named:
        if not isinstance(value, str) or not value.strip():
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message=(
                    "'input_streams' is a list of the flow's input port names;"
                    f" got {value!r}."
                ),
            ).to_exception()
    ports = tuple(name.strip() for name in named)
    if len(set(ports)) != len(ports):
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message="'input_streams' names the same port twice.",
        ).to_exception()
    return ports


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
