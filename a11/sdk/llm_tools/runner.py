import asyncio
import dataclasses
import json
from collections import defaultdict
from typing import Any

from absl import logging

from a11 import _native
from a11._native import NodeFragment

import a11
from a11.status import Status, StatusCode, StatusException

from a11.sdk.llm import (
    action_name_matches_allowed,
    get_allowed_llm_action_patterns,
    Interaction,
    TOOL_LOGS_METADATA_KEY,
)
from a11.sdk.llm_tools.adapter import WHOLE_JSON_OUTPUT, ToolAdapter


def get_tool_definitions(
    registry: a11.ActionRegistry | None,
    allowed_actions: list[str] | None = None,
) -> list[dict[str, Any]]:
    if registry is None:
        return []

    allowed_actions = allowed_actions or []

    definitions: list[dict[str, Any]] = []

    for name in allowed_actions:
        schema: a11.ActionSchema | None = None

        try:
            schema = registry.get_schema(name)
        except StatusException as exc:
            if exc.status.code == StatusCode.NOT_FOUND:
                continue

        schema: a11.ActionSchema
        adapter = ToolAdapter(schema)
        definitions.append(
            {
                "name": schema.name,
                "description": schema.description,
                "input_schema": adapter.input_schema,
            }
        )

    return definitions


async def collect_tools(
    action: a11.Action, deadline: a11.Duration | None = None
) -> list[dict[str, Any]]:
    """The tools this turn may use: the caller's, plus the registry's own.

    Two sources meet here, and the allowed-action patterns are what admits
    either. A caller streams tool definitions onto the ``tools`` input -- its
    own tools, which it will serve itself when the model calls them -- and
    anything it sends that the patterns do not match is dropped. The actions
    registered *here* are then offered too, for every registered name the
    patterns match that the caller did not describe: a peer that says
    ``shell_.*`` is asking for this side's shell tools, and the alternative
    would be for it to reproduce their schemas by hand to be offered them.

    A registry action can only be *called* if it matches the same patterns
    (:func:`execute_actions_from_interaction` checks them again at call time),
    so this widens what the model is shown as far as the caller allowed.
    """
    deadline = deadline if deadline is not None else a11.get_deadline(action)
    allowed_patterns = get_allowed_llm_action_patterns(action)

    tools: list[dict[str, Any]] = []
    async for tool in action["tools"].iter_with_deadline(deadline):
        if not action_name_matches_allowed(tool["name"], allowed_patterns):
            logging.warning(
                "Tool `%s` was requested, but isn't allowed.", tool["name"]
            )
            continue
        tools.append(tool)

    registry = action.get_registry()
    if registry is None:
        return tools

    requested = {tool["name"] for tool in tools}
    local = sorted(
        name
        for name in registry.list_registered_actions()
        if name not in requested
        and name != action.get_schema().name
        and action_name_matches_allowed(name, allowed_patterns)
    )
    tools.extend(get_tool_definitions(registry, local))
    return tools


@dataclasses.dataclass(frozen=True)
class ExecutedActions:
    """The result of running one interaction's tool calls, keyed by call id.

    ``outputs`` holds each call's output fragments and ``errors`` the status
    of each call that failed. A call appears in ``outputs`` either way — a
    backend owes the model one result per call it made, and for a failed call
    that result is the failure.

    ``logs`` holds what the called actions wrote through
    [log][a11.actions.action.Action.log]: narration for the person watching. It
    is not in ``outputs`` and cannot get there -- the log port is not one of the
    action's declared outputs -- so it can never reach the model. Only the calls
    that logged something user-facing appear; a log marked ``internal`` is A11's
    own bookkeeping and is left out.
    """

    outputs: dict[str, list[NodeFragment]]
    errors: dict[str, Status]
    logs: dict[str, str] = dataclasses.field(default_factory=dict)

    def log_metadata(self) -> dict[str, bytes]:
        """This round's logs as interaction metadata; ``{}`` if there are none.

        Merge into the ``backend_specific_metadata`` of the interaction carrying
        these calls' results, which is where a consumer looks for them; see
        [TOOL_LOGS_METADATA_KEY][a11.sdk.llm.TOOL_LOGS_METADATA_KEY].
        """
        if not self.logs:
            return {}
        return {TOOL_LOGS_METADATA_KEY: json.dumps(self.logs).encode()}

    def error_message(self, call_id: str) -> str | None:
        """A caller-facing description of a call's failure, if it failed."""
        status = self.errors.get(call_id)
        if status is None:
            return None
        if not status.message:
            return status.code.name
        return f"{status.code.name}: {status.message}"


async def _user_facing_log(node: a11.AsyncNode, deadline: a11.Duration) -> str:
    """What a call logged for the person watching, as one block of text.

    Reads the call's log port and keeps the entries that are *not* marked
    internal -- "user facing" is the absence of that flag, so a handler narrating
    itself needs no second port and no second decision. Each entry is rendered by
    [log_record_from_chunk][a11._native.log_record_from_chunk], which is also
    what the ``a11.action`` logger uses, so a line reads the same wherever it is
    shown.

    Best effort throughout: a log that will not decode is not worth failing a
    tool over. Bounded by the turn's deadline like every other read here.
    """
    parts: list[str] = []
    async for chunk in node.iter_chunks(
        timeout=max(deadline - a11.now(), a11.zero_duration())
    ):
        if chunk is None or chunk.is_null() or _native.is_status_chunk(chunk):
            continue
        try:
            record = _native.log_record_from_chunk(chunk)
        except Exception:
            logging.debug("undecodable log chunk", exc_info=True)
            continue
        if record["internal"]:
            continue
        parts.append(record["text"])
    return "".join(parts)


def _status_of(error: BaseException) -> Status:
    """The status an action failed with, however the failure reached us."""
    if isinstance(error, StatusException):
        return error.status
    if isinstance(error, asyncio.TimeoutError):
        return Status(
            code=StatusCode.DEADLINE_EXCEEDED, message="The action timed out."
        )
    return Status(code=StatusCode.UNKNOWN, message=str(error))


async def execute_actions_from_interaction(
    interaction: Interaction,
    action: a11.Action,
    registry: a11.ActionRegistry | None = None,
) -> ExecutedActions:
    """Run an interaction's tool calls and collect a result for each of them.

    One call failing does not sink the others: every call is run, every call
    is waited for, and a failure is recorded against that call alone. The
    model asked for each of these independently and can act on a partial
    answer.
    """
    deadline = a11.get_deadline(action)

    registry = registry or action.get_registry()
    if registry is None:
        raise Status(
            code=StatusCode.FAILED_PRECONDITION,
            message="Cannot execute actions against an empty registry.",
        ).to_exception()

    allowed_patterns = get_allowed_llm_action_patterns(action)
    nested_actions: list[a11.Action] = []
    log_nodes: dict[str, a11.AsyncNode] = {}
    errors: dict[str, Status] = {}
    for call in interaction.action_calls:
        # Setting a call up can fail on its own — an action the model may not
        # call, a name no longer in the registry, inputs that will not go onto
        # their ports. That is this call's failure, not the round's.
        try:
            if not action_name_matches_allowed(call.name, allowed_patterns):
                raise Status(
                    code=StatusCode.PERMISSION_DENIED,
                    message=f"Action {call.name} is not allowed",
                ).to_exception()

            nested_action = (
                action.make_nested(registry.get_schema(call.name))
                .set_id(call.id)
                .bind_stream(None)
                .bind_handler(registry.get_handler(call.name))
            )
            a11.set_deadline_header(nested_action, deadline)
            # Claimed before the run, because the log the handler writes on its
            # first line has to arrive here rather than on the process sink: this
            # runner is the consumer that shows it to a person.
            log_nodes[call.id] = nested_action.get_log_node()
            nested_action.run()
            input_fragments = interaction.action_inputs.get(call.id, [])
            for fragment in input_fragments:
                await nested_action[fragment.id].put_fragment(fragment)

            # Autofilled inputs are written, drained, and closed by the native
            # run flow, so the runner must only close the inputs it fed itself.
            # Closed, not finalized: the forwarded fragments carry whatever
            # finality the model's arguments had.
            for input_name, port in nested_action.get_schema().inputs.items():
                if port.autofills:
                    continue
                await nested_action[input_name].close()
            nested_actions.append(nested_action)
        except Exception as error:
            errors[call.id] = _status_of(error)

    # `return_exceptions` is the point: a raising `wait` must not abandon the
    # other calls mid-flight, leaving results the model is never told about.
    finished = await asyncio.gather(
        *[
            nested_action.wait(max(deadline - a11.now(), a11.zero_duration()))
            for nested_action in nested_actions
        ],
        return_exceptions=True,
    )
    for nested_action, outcome in zip(nested_actions, finished):
        if isinstance(outcome, BaseException):
            errors[nested_action.id] = _status_of(outcome)

    all_outputs: dict[str, list[NodeFragment]] = defaultdict(list)
    logs: dict[str, str] = {}
    # Every call the model made gets an entry, including one that failed
    # before it could run. Backends pair exactly one tool result with each tool
    # call, so a call missing here is a protocol error on the next request
    # rather than an empty result — and the turn then dies after the tools
    # have run, which reads as the model falling silent.
    for call in interaction.action_calls:
        all_outputs.setdefault(call.id, [])

    for nested_action in nested_actions:
        action_outputs: dict[str, list[NodeFragment]] = dict()
        for output_name in nested_action.get_schema().outputs.keys():
            action_outputs[output_name] = []
            try:
                node = nested_action[output_name]
                # Bounded by the turn's deadline, like everything else here. A
                # call whose action has finished can still leave an output that
                # never ends — one nobody closed, or one whose node was resolved
                # from a stale id and belongs to an earlier call — and reading it
                # without a timeout hangs the whole turn: no text is ever
                # written, so the caller learns nothing until *its* read expires.
                async for fragment in node.iter_fragments(
                    timeout=max(deadline - a11.now(), a11.zero_duration())
                ):
                    fragment.id = output_name
                    fragment.continued = True
                    action_outputs[output_name].append(fragment)
            except StatusException as error:
                # An aborted output carries the failure that aborted it, and a
                # timed-out one the deadline. Keep whatever arrived before it and
                # record the reason once.
                errors.setdefault(nested_action.id, error.status)
            if action_outputs[output_name]:
                action_outputs[output_name][-1].continued = False

        # Read separately from the outputs above, because the log port is not one
        # of them: it is not in the schema, so the model's tool result is built
        # from what the action declared and the narration cannot leak into it by
        # accident. Nothing has to close the port either -- the action does, with
        # its other outputs -- so an action that never logs costs one ended read.
        node = log_nodes.get(nested_action.id)
        if node is not None:
            try:
                if log := await _user_facing_log(node, deadline):
                    logs[nested_action.id] = log
            except StatusException as error:
                errors.setdefault(nested_action.id, error.status)

        schema = nested_action.get_schema()
        mapping = dict(schema.output_to_json_field)
        # `{"port": "$"}` means "this port *is* the whole result", so the result
        # is that port's payload rather than an object wrapping it. The sentinel
        # is the mapped-to *field*, matching ActionSchema's own validation
        # (cpp/a11/actions/schema.cc), the tool-definition side
        # ([a11.sdk.llm_tools.adapter][a11.sdk.llm_tools.adapter]) and the Kotlin
        # port. Reading it as the key instead left the sentinel to fall through
        # as a fragment id, where it fails name validation.
        whole_output_port = None
        if len(mapping) == 1:
            port, field = next(iter(mapping.items()))
            if field == WHOLE_JSON_OUTPUT:
                whole_output_port = port
        if whole_output_port is not None:
            for fragment in action_outputs.get(whole_output_port, []):
                fragment.id = "_"
                all_outputs[nested_action.id].append(fragment)
            continue

        for output_name, fragments in action_outputs.items():
            map_to = output_name
            if mapping:
                map_to = mapping.get(output_name)

            if map_to is None:
                continue

            for fragment in fragments:
                fragment.id = map_to
                all_outputs[nested_action.id].append(fragment)

    return ExecutedActions(
        outputs=dict(all_outputs), errors=errors, logs=logs
    )
