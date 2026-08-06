import asyncio
import dataclasses
from collections import defaultdict
from typing import Any

from a11._native import NodeFragment

import a11
from a11.status import Status, StatusCode, StatusException

from a11.sdk.llm import (
    action_name_matches_allowed,
    get_allowed_llm_action_patterns,
    Interaction,
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


@dataclasses.dataclass(frozen=True)
class ExecutedActions:
    """The result of running one interaction's tool calls, keyed by call id.

    ``outputs`` holds each call's output fragments and ``errors`` the status
    of each call that failed. A call appears in ``outputs`` either way — a
    backend owes the model one result per call it made, and for a failed call
    that result is the failure.
    """

    outputs: dict[str, list[NodeFragment]]
    errors: dict[str, Status]

    def error_message(self, call_id: str) -> str | None:
        """A caller-facing description of a call's failure, if it failed."""
        status = self.errors.get(call_id)
        if status is None:
            return None
        if not status.message:
            return status.code.name
        return f"{status.code.name}: {status.message}"


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
                async for fragment in node.iter_fragments():
                    fragment.id = output_name
                    fragment.continued = True
                    action_outputs[output_name].append(fragment)
            except StatusException as error:
                # An aborted output carries the failure that aborted it. Keep
                # whatever arrived before the abort and record the reason once.
                errors.setdefault(nested_action.id, error.status)
            if action_outputs[output_name]:
                action_outputs[output_name][-1].continued = False

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

    return ExecutedActions(outputs=dict(all_outputs), errors=errors)
