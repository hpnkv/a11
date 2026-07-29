# Copyright 2026 The A11 Authors.

import asyncio
import traceback
from typing import Any

from absl import logging

import a11
import anthropic
import pydantic_core

from a11.status import Status, StatusCode, StatusException
from anthropic.lib.streaming._messages import accumulate_event

from a11.sdk.anthropic.client import get_anthropic_client
from a11.sdk.anthropic.interact_with_claude_schema import (
    CreateMessageConfig,
    DEFAULT_MODEL,
)
from a11.sdk.llm import Interaction, LlmHeaders, Role, get_allowed_action_names
from a11.sdk.llm_tools import runner


class Conversation:
    _interactions: list[Interaction]
    _messages: list[dict[str, Any]]
    _system_instructions: list[str]

    def __init__(self):
        self._interactions = []
        self._messages = []
        self._system_instructions = []

    @property
    def last_interaction_id(self):
        if not self._interactions:
            return None
        return self._interactions[-1].id

    @property
    def system_prompt(self):
        if not self._system_instructions:
            return None
        return "\n\n".join(self._system_instructions)

    @property
    def messages(self) -> list[dict[str, Any]]:
        return self._messages

    def feed_next_interaction(self, interaction: Interaction) -> Interaction:
        if interaction.previous_interaction_id:
            if (
                self._interactions
                and self._interactions[-1].id
                != interaction.previous_interaction_id
            ):
                raise Status(
                    code=StatusCode.INVALID_ARGUMENT,
                    message=(
                        "Interaction does not follow previous interaction:"
                        f" {interaction.previous_interaction_id} vs"
                        f" {self._interactions[-1].id}."
                    ),
                ).to_exception()

            elif not self._interactions:
                raise Status(
                    code=StatusCode.FAILED_PRECONDITION,
                    message=(
                        "Cannot insert a non-root interaction as conversation"
                        " root."
                    ),
                ).to_exception()

        if interaction.system_instructions:
            if self._system_instructions or self._interactions:
                raise Status(
                    code=StatusCode.INVALID_ARGUMENT,
                    message=(
                        "Cannot set system instructions after initial"
                        " interaction."
                    ),
                ).to_exception()

            for instruction_chunk in interaction.system_instructions:
                instruction = a11.from_chunk(instruction_chunk)
                if not isinstance(instruction, str):
                    raise Status(
                        code=StatusCode.INVALID_ARGUMENT,
                        message="Only text system instructions are allowed.",
                    ).to_exception()
                self._system_instructions.append(instruction)

        if not interaction.content:
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message="Interaction content is required.",
            ).to_exception()

        if len(interaction.content) > 1:
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message="Only one content chunk is allowed.",
            ).to_exception()

        content = a11.from_chunk(interaction.content[0])
        if isinstance(
            content, (anthropic.types.Message, anthropic.types.ParsedMessage)
        ):
            message = content.model_dump()
        elif isinstance(content, dict):
            role = content.get("role")
            content_content = content.get("content")
            if not role or not content_content:
                raise Status(
                    code=StatusCode.INVALID_ARGUMENT,
                    message="Interaction content and role are required.",
                ).to_exception()
            message = {"role": role, "content": content_content}
        elif isinstance(content, str):
            message = {"role": Role.USER, "content": content}
        else:
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message=(
                    "Interaction content must be a string, dict, or Message."
                ),
            ).to_exception()

        for field in ("id", "container", "stop_reason", "stop_details"):
            if value := interaction.message_metadata.get(field):
                if isinstance(value, bytes):
                    value = value.decode()
                message[field] = value

        interaction.content = [a11.to_chunk(message)]

        if self._interactions and not interaction.previous_interaction_id:
            interaction.previous_interaction_id = self._interactions[-1].id

        self._messages.append(message)
        self._interactions.append(interaction)

        return interaction


class ActionCallAdapter:
    def __init__(
        self,
        tool_name: str,
        tool_call_id: str,
        tool_params: dict[str, Any],
        schema: a11.ActionSchema,
    ):
        self._name = tool_name
        self._call_id = tool_call_id
        self._arguments = tool_params
        self._schema = schema

    @property
    def action_message(self) -> a11.ActionMessage:
        return a11.Action(self._schema, self._call_id).get_action_message()

    async def get_action_inputs(
        self,
    ) -> list[a11.NodeFragment]:
        inputs = list()
        for key, value_list in self._arguments.items():
            if not isinstance(value_list, list):
                value_list = [value_list]

            node = a11.AsyncNode.create("node")
            for idx, value in enumerate(value_list):
                await node.put(value, final=idx == len(value_list) - 1)
            await node.drain_and_close()

            final_encountered = False
            fragments = []
            async for fragment in node.iter_fragments():
                if not fragment.continued:
                    final_encountered = True
                fragment.id = key
                fragments.append(fragment)

            if not final_encountered:
                fragments.append(None)

            inputs.extend(fragments)

        return inputs

    @staticmethod
    def _validate_tool_call_integrity(
        tool_call: dict[str, Any],
    ) -> dict[str, Any]:
        for field in ("name", "id", "params"):
            if field not in tool_call:
                raise Status(
                    code=StatusCode.INVALID_ARGUMENT,
                    message=f"Tool call must have a field `{field}`.",
                ).to_exception()

        if len(tool_call) != 3:
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message=(
                    "Tool call must have exactly three fields: name, id,"
                    " params."
                ),
            ).to_exception()

        if not isinstance(tool_call["name"], str):
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message=f"Tool call name must be a string.",
            ).to_exception()

        if not isinstance(tool_call["id"], str):
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message=f"Tool call id must be a string.",
            ).to_exception()

        if not isinstance(tool_call["params"], dict):
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message=f"Tool call params must be a dictionary.",
            ).to_exception()

        for key in tool_call["params"].keys():
            if not isinstance(key, str):
                raise Status(
                    code=StatusCode.INVALID_ARGUMENT,
                    message=f"Tool call parameter names must be strings.",
                ).to_exception()

        return tool_call

    @staticmethod
    def validate_against_schema(
        tool_call: dict[str, Any],
        schema: a11.ActionSchema,
        validate_integrity: bool = True,
    ) -> dict[str, Any]:
        if validate_integrity:
            tool_call = ActionCallAdapter._validate_tool_call_integrity(
                tool_call
            )

        if tool_call["name"] != schema.name:
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message=f"Tool call name must be {schema.name}.",
            ).to_exception()

        for actual_input in tool_call["params"].keys():
            if actual_input not in schema.inputs:
                raise Status(
                    code=StatusCode.INVALID_ARGUMENT,
                    message=f"Tool call has unexpected input {actual_input}.",
                ).to_exception()

        for expected_input_name, expected_input in schema.inputs.items():
            if (
                expected_input.required
                and expected_input_name not in tool_call["params"]
            ):
                raise Status(
                    code=StatusCode.INVALID_ARGUMENT,
                    message=(
                        f"Tool call is missing input {expected_input_name}."
                    ),
                ).to_exception()

        for expected_input_name, expected_input in schema.inputs.items():
            expected_input: a11.ActionPortSchema
            if (
                expected_input.autofills
                and tool_call["params"].get(expected_input_name) is not None
            ):
                raise Status(
                    code=StatusCode.INVALID_ARGUMENT,
                    message="Tool call is trying to fill a prefilled input.",
                ).to_exception()

        return tool_call

    @staticmethod
    def create(tool_call: dict[str, Any], schema: a11.ActionSchema):
        tool_call = ActionCallAdapter._validate_tool_call_integrity(tool_call)
        tool_call = ActionCallAdapter.validate_against_schema(
            tool_call, schema, validate_integrity=False
        )

        return ActionCallAdapter(
            tool_call["name"], tool_call["id"], tool_call["params"], schema
        )


def _decode_action_output_fragments(
    fragments: list[a11.NodeFragment],
) -> Any:
    grouped: dict[str, list[a11.NodeFragment]] = {}
    for fragment in fragments:
        grouped.setdefault(fragment.id, []).append(fragment)

    values: dict[str, Any] = {}
    for field_name, field_fragments in grouped.items():
        decoded = [
            a11.from_chunk(fragment.get_chunk()) for fragment in field_fragments
        ]
        values[field_name] = decoded[0] if len(decoded) == 1 else decoded

    if list(values.keys()) == ["$"]:
        return values["$"]
    return values


async def _build_tool_results_from_outputs(
    outputs: dict[str, list[a11.NodeFragment]],
) -> list[dict[str, Any]]:
    tool_results = []
    for tool_use_id, fragments in outputs.items():
        content = _decode_action_output_fragments(fragments)
        if not isinstance(content, str):
            content = (
                await asyncio.to_thread(pydantic_core.to_json, content)
            ).decode()

        tool_results.append(
            {
                "type": "tool_result",
                "tool_use_id": tool_use_id,
                "content": content,
            }
        )

    return tool_results


async def _add_tool_calls_to_interaction(
    tool_calls: list[dict[str, Any]],
    interaction: Interaction,
    registry: a11.ActionRegistry,
):
    for tool_call in tool_calls:
        adapter = ActionCallAdapter.create(
            tool_call,
            registry.get_schema(tool_call["name"]),
        )

        interaction.action_calls.append(adapter.action_message)
        if tool_call["id"] not in interaction.action_inputs:
            interaction.action_inputs[tool_call["id"]] = []
        interaction.action_inputs[tool_call["id"]].extend(
            await adapter.get_action_inputs()
        )


async def interact_with_claude(action: a11.Action):
    deadline = a11.get_deadline(action)

    def remaining_timeout():
        return max(deadline - a11.now(), a11.zero_duration())

    api_key = action.get_header(LlmHeaders.API_KEY.value, decode=True)
    if api_key is None:
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message="API key is required.",
        ).to_exception()
    api_key: str

    model: str = (
        action.get_header(LlmHeaders.MODEL.value, decode=True) or DEFAULT_MODEL
    )

    config = await action["config"].consume(
        CreateMessageConfig, timeout=remaining_timeout(), allow_none=True
    )
    if config is None:
        config = CreateMessageConfig()

    previous_interaction_id = ""
    conversation = Conversation()
    async for interaction in action["interactions"]:
        interaction = conversation.feed_next_interaction(interaction)
        previous_interaction_id = interaction.id

    client = get_anthropic_client(api_key)

    allowed_actions = get_allowed_action_names(action)
    tools = runner.get_tool_definitions(action.get_registry(), allowed_actions)

    try:
        while True:
            snapshot = None
            try:
                thinking = anthropic.Omit()
                if "haiku" not in model and not tools:
                    thinking = {"type": "adaptive"}

                output_config = {"effort": "medium"}
                if "haiku" in model or "4-5" not in model:
                    output_config = anthropic.Omit()

                messages = [
                    {"role": message["role"], "content": message["content"]}
                    for message in conversation.messages
                ]
                stream = await client.messages.create(
                    max_tokens=config.max_tokens,
                    messages=messages,
                    model=model,
                    cache_control={"type": "ephemeral", "ttl": "1h"},
                    system=conversation.system_prompt or anthropic.Omit(),
                    stream=True,
                    tool_choice=(
                        {"type": "auto"} if tools else anthropic.Omit()
                    ),
                    tools=tools or anthropic.Omit(),
                    thinking=thinking,
                    output_config=output_config,
                )
            except anthropic.APIError as exc:
                raise Status(
                    code=StatusCode.INTERNAL, message=str(exc)
                ).to_exception() from exc

            tool_calls = []
            tool_names = dict()
            tool_use_ids = dict()
            tool_inputs = dict()

            async for event in stream:
                await action["event_stream"].put(event)
                snapshot = accumulate_event(
                    event=event, current_snapshot=snapshot
                )

                if event.type == "content_block_start":
                    if event.content_block.type == "tool_use":
                        tool_names[event.index] = event.content_block.name
                        tool_use_ids[event.index] = event.content_block.id
                        tool_inputs[event.index] = ""

                if event.type == "content_block_delta":
                    delta = event.delta

                    if delta.type == "input_json_delta":
                        tool_inputs[event.index] += delta.partial_json

                if (
                    event.type == "content_block_stop"
                    and event.index in tool_inputs
                ):
                    parsed_tool_input = {}
                    if tool_inputs[event.index]:
                        parsed_tool_input = await asyncio.to_thread(
                            pydantic_core.from_json, tool_inputs[event.index]
                        )
                    tool_calls.append(
                        {
                            "name": tool_names[event.index],
                            "id": tool_use_ids[event.index],
                            "params": parsed_tool_input,
                        }
                    )
                    tool_names.pop(event.index)
                    tool_use_ids.pop(event.index)
                    tool_inputs.pop(event.index)

            if snapshot is None:
                raise Status(
                    code=StatusCode.DATA_LOSS,
                    message="No message could be accumulated.",
                ).to_exception()

            message_metadata = {}
            for field in ("container", "stop_reason", "stop_details"):
                value = getattr(snapshot, field)
                if value is not None:
                    message_metadata[field] = value

            interaction = Interaction(
                previous_interaction_id=previous_interaction_id,
                role=Role.ASSISTANT,
                created_at_millis=a11.now().nanoseconds_since_epoch // 1000000,
                model=snapshot.model,
                content=[
                    await asyncio.to_thread(
                        a11.to_chunk, snapshot.model_dump(exclude_none=True)
                    )
                ],
                message_metadata=message_metadata,
            )
            previous_interaction_id = interaction.id
            await _add_tool_calls_to_interaction(
                tool_calls, interaction, action.get_registry()
            )

            interaction = conversation.feed_next_interaction(interaction)

            await action["new_interactions"].put(interaction)
            if not interaction.action_calls:
                break

            outputs = await runner.execute_actions_from_interaction(
                interaction, action, action.get_registry()
            )

            tool_output_interaction = Interaction(
                previous_interaction_id=previous_interaction_id,
                role=Role.USER,
                created_at_millis=a11.now().nanoseconds_since_epoch // 1000000,
                action_outputs=outputs,
                content=[
                    a11.to_chunk(
                        {
                            "role": "user",
                            "content": await _build_tool_results_from_outputs(
                                outputs
                            ),
                        }
                    )
                ],
            )
            previous_interaction_id = tool_output_interaction.id
            tool_output_interaction = conversation.feed_next_interaction(
                tool_output_interaction
            )

            await action["new_interactions"].put(tool_output_interaction)

    except StatusException:
        raise

    except Exception as e:
        tb = traceback.format_exc()
        raise Status(code=StatusCode.INTERNAL, message=tb).to_exception() from e

    else:
        await action["event_stream"].put_null_final()
        await action["event_stream"].drain_and_close()
        await action["new_interactions"].drain_and_close()

    finally:
        pass
