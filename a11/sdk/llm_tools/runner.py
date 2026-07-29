import asyncio
from collections import defaultdict
from typing import Any

from a11._native import NodeFragment

import a11
from a11.status import Status, StatusCode, StatusException

from a11.sdk.llm import get_allowed_action_names, Interaction
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


async def execute_actions_from_interaction(
    interaction: Interaction,
    action: a11.Action,
    registry: a11.ActionRegistry | None = None,
):
    deadline = a11.get_deadline(action)

    registry = registry or action.get_registry()
    if registry is None:
        raise Status(
            code=StatusCode.FAILED_PRECONDITION,
            message="Cannot execute actions against an empty registry.",
        ).to_exception()

    allowed_actions = get_allowed_action_names(action)
    nested_actions: list[a11.Action] = []
    for call in interaction.action_calls:
        if call.name not in allowed_actions:
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
        input_fragments = interaction.action_inputs[call.id]
        for fragment in input_fragments:
            await nested_action[fragment.id].put_fragment(fragment)

        for input_name in nested_action.get_schema().inputs.keys():
            await nested_action[input_name].drain_and_close()
        nested_actions.append(nested_action)

    await asyncio.gather(
        *[
            nested_action.wait(max(deadline - a11.now(), a11.zero_duration()))
            for nested_action in nested_actions
        ]
    )

    all_outputs = defaultdict(list)
    for nested_action in nested_actions:
        action_outputs: dict[str, list[NodeFragment]] = dict()
        for output_name in nested_action.get_schema().outputs.keys():
            action_outputs[output_name] = []
            async for fragment in nested_action[output_name].iter_fragments():
                fragment.id = output_name
                fragment.continued = True
                action_outputs[output_name].append(fragment)
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

    return all_outputs
