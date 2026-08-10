import asyncio
import dataclasses
import json
from collections import defaultdict
from typing import Any

from absl import logging

from a11._native import NodeFragment

import a11
from a11.status import Status, StatusCode, StatusException

from a11.sdk.llm import (
    action_name_matches_allowed,
    get_allowed_llm_action_patterns,
    Interaction,
    TOOL_LOGS_METADATA_KEY,
    USER_FACING_LOG_PORT,
)
from a11.sdk.llm_tools.adapter import ToolAdapter


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

    ``logs`` holds whatever the called actions wrote to their
    [user_facing_log][a11.sdk.llm.USER_FACING_LOG_PORT] port: narration for the
    person watching, kept out of ``outputs`` so it can never reach the model.
    Only the calls that wrote one appear.
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


def _log_text(fragments: list[NodeFragment]) -> str:
    """The text a call wrote to its user-facing log port, if any.

    Best effort throughout: a log that will not decode is not worth failing a
    tool over, and a null chunk is an end-of-stream marker rather than a value.
    """
    parts: list[str] = []
    for fragment in fragments:
        chunk = fragment.get_chunk()
        if chunk is None or chunk.is_null():
            continue
        try:
            parts.append(str(a11.from_chunk(chunk)))
        except Exception:
            logging.debug("undecodable user-facing log chunk", exc_info=True)
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
    answer — one tool being unavailable is not a reason to withhold the three
    that worked, nor to end the turn.
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
            nested_action.run()
            input_fragments = interaction.action_inputs.get(call.id, [])
            for fragment in input_fragments:
                await nested_action[fragment.id].put_fragment(fragment)

            # Autofilled inputs are written, drained, and closed by the native
            # run flow, so the runner must only close the inputs it fed itself.
            for input_name, port in nested_action.get_schema().inputs.items():
                if port.autofills:
                    continue
                await nested_action[input_name].drain_and_close()
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

        # Taken out before the result is assembled, so nothing downstream has to
        # know this port exists: the model's tool result is built from what is
        # left, and the narration goes to whoever shows the run to a person.
        # It is drained above like any other port, which it must be -- a port
        # with no reader stalls the action writing it.
        if log := _log_text(action_outputs.pop(USER_FACING_LOG_PORT, [])):
            logs[nested_action.id] = log

        schema = nested_action.get_schema()
        mapped_output_names = list(schema.output_to_json_field.keys())
        first_key = mapped_output_names[0] if mapped_output_names else None
        if len(schema.output_to_json_field) == 1 and first_key == "$":
            for fragment in action_outputs[first_key]:
                fragment.id = "_"
                all_outputs[nested_action.id].append(fragment)
            continue

        for output_name, fragments in action_outputs.items():
            map_to = output_name
            if len(schema.output_to_json_field) > 0:
                map_to = schema.output_to_json_field.get(output_name)

            if map_to is None:
                continue

            for fragment in fragments:
                fragment.id = map_to
                all_outputs[nested_action.id].append(fragment)

    return ExecutedActions(
        outputs=dict(all_outputs), errors=errors, logs=logs
    )
