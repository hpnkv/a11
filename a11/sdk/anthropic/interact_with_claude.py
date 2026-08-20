# Copyright 2026 The A11 Authors.

import asyncio
import dataclasses
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
from a11.sdk import llm
from a11.sdk.llm_tools import runner


def _stringify_content(content: Any) -> str:
    """Flatten a tool-result content payload into plain text."""
    if content is None:
        return ""
    if isinstance(content, str):
        return content
    if isinstance(content, list):
        texts = [
            block.get("text", "")
            for block in content
            if isinstance(block, dict) and block.get("type") == "text"
        ]
        if texts:
            return "".join(texts)
    return pydantic_core.to_json(content).decode()


def _claude_to_normalized(
    interaction: llm.Interaction,
) -> llm.NormalizedMessage:
    """Produce the normalized view of a Claude-native interaction."""
    content = a11.from_chunk(interaction.content[0])
    if isinstance(
        content, (anthropic.types.Message, anthropic.types.ParsedMessage)
    ):
        content = content.model_dump()

    if isinstance(content, str):
        return llm.NormalizedMessage(
            role=llm.Role.USER,
            parts=[
                llm.NormalizedPart(
                    type=llm.NormalizedContentType.TEXT, text=content
                )
            ],
        )

    if not isinstance(content, dict):
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message="Unrecognized Claude interaction content.",
        ).to_exception()

    role = (
        llm.Role.ASSISTANT
        if content.get("role") == "assistant"
        else llm.Role.USER
    )
    blocks = content.get("content")
    parts: list[llm.NormalizedPart] = []
    if isinstance(blocks, str):
        parts.append(
            llm.NormalizedPart(type=llm.NormalizedContentType.TEXT, text=blocks)
        )
        blocks = []

    for block in blocks or []:
        if isinstance(block, str):
            parts.append(
                llm.NormalizedPart(
                    type=llm.NormalizedContentType.TEXT, text=block
                )
            )
            continue

        block_type = block.get("type")
        if block_type == "text":
            parts.append(
                llm.NormalizedPart(
                    type=llm.NormalizedContentType.TEXT,
                    text=block.get("text", ""),
                )
            )
        elif block_type == "tool_use":
            parts.append(
                llm.NormalizedPart(
                    type=llm.NormalizedContentType.TOOL_CALL,
                    id=block.get("id"),
                    name=block.get("name"),
                    arguments=block.get("input") or {},
                )
            )
        elif block_type == "tool_result":
            parts.append(
                llm.NormalizedPart(
                    type=llm.NormalizedContentType.TOOL_RESULT,
                    call_id=block.get("tool_use_id"),
                    content=_stringify_content(block.get("content")),
                )
            )
        elif block_type == "image":
            source = block.get("source") or {}
            parts.append(
                llm.NormalizedPart(
                    type=llm.NormalizedContentType.IMAGE,
                    data=source.get("data"),
                    mime_type=source.get("media_type"),
                )
            )
        # `thinking`, `redacted_thinking`, and server tool blocks carry no
        # portable content and are intentionally dropped.

    return llm.NormalizedMessage(role=role, parts=parts)


def _claude_from_normalized(message: llm.NormalizedMessage) -> dict[str, Any]:
    """Translate a normalized message into a Claude-native message dict."""
    role = "assistant" if message.role == llm.Role.ASSISTANT else "user"
    blocks: list[dict[str, Any]] = []
    for part in message.parts:
        if part.type == llm.NormalizedContentType.TEXT:
            blocks.append({"type": "text", "text": part.text or ""})
        elif part.type == llm.NormalizedContentType.IMAGE:
            blocks.append(
                {
                    "type": "image",
                    "source": {
                        "type": "base64",
                        "media_type": (
                            part.mime_type or "application/octet-stream"
                        ),
                        "data": part.data or "",
                    },
                }
            )
        elif part.type == llm.NormalizedContentType.TOOL_CALL:
            blocks.append(
                {
                    "type": "tool_use",
                    "id": part.id or "",
                    "name": part.name or "",
                    "input": part.arguments or {},
                }
            )
        elif part.type == llm.NormalizedContentType.TOOL_RESULT:
            blocks.append(
                {
                    "type": "tool_result",
                    "tool_use_id": part.call_id or "",
                    "content": part.content or "",
                }
            )
    return {"role": role, "content": blocks}


llm.register_interaction_normalizer(llm.Backend.CLAUDE, _claude_to_normalized)


class Conversation:
    _interactions: list[llm.Interaction]
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

    def feed_next_interaction(
        self, interaction: llm.Interaction
    ) -> llm.Interaction:
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

        backend = llm.interaction_backend(interaction)
        if backend is not None and backend != llm.Backend.CLAUDE:
            # Produced by another backend: bridge it through the normalised
            # representation and leave the interaction's own content untouched.
            message = _claude_from_normalized(
                llm.normalize_interaction(interaction)
            )
        else:
            # Tagged as ours, or untagged (optimistically treated as native).
            message = self._native_message(interaction)

        if self._interactions and not interaction.previous_interaction_id:
            interaction.previous_interaction_id = self._interactions[-1].id

        self._messages.append(message)
        self._interactions.append(interaction)

        return interaction

    @staticmethod
    def _native_message(interaction: llm.Interaction) -> dict[str, Any]:
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
            message = {"role": llm.Role.USER, "content": content}
        else:
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message=(
                    "Interaction content must be a string, dict, or Message."
                ),
            ).to_exception()

        for field in ("id", "container", "stop_reason", "stop_details"):
            if value := interaction.backend_specific_metadata.get(field):
                if isinstance(value, bytes):
                    value = value.decode()
                message[field] = value

        interaction.content = [a11.to_chunk(message)]
        return message


@dataclasses.dataclass
class _ToolCall:
    """A tool call aggregated from a model's streamed content block."""

    name: str
    id: str
    partial_json: str = ""
    params: dict[str, Any] = dataclasses.field(default_factory=dict)

    def apply_input_delta(self, partial_json: str) -> None:
        self.partial_json += partial_json

    async def finalize_params(self) -> None:
        if self.partial_json:
            self.params = await asyncio.to_thread(
                pydantic_core.from_json, self.partial_json
            )


class ActionCallAdapter:
    def __init__(
        self,
        tool_call: _ToolCall,
        schema: a11.ActionSchema,
    ):
        self._name = tool_call.name
        self._call_id = tool_call.id
        self._arguments = tool_call.params
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
            # Closed, not finalized: the last put above already marked finality
            # (and an empty argument list has nothing to mark).
            await node.close()

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
        tool_call: _ToolCall,
    ) -> _ToolCall:
        if not isinstance(tool_call.name, str):
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message=f"Tool call name must be a string.",
            ).to_exception()

        if not isinstance(tool_call.id, str):
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message=f"Tool call id must be a string.",
            ).to_exception()

        if not isinstance(tool_call.params, dict):
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message=f"Tool call params must be a dictionary.",
            ).to_exception()

        for key in tool_call.params.keys():
            if not isinstance(key, str):
                raise Status(
                    code=StatusCode.INVALID_ARGUMENT,
                    message=f"Tool call parameter names must be strings.",
                ).to_exception()

        return tool_call

    @staticmethod
    def validate_against_schema(
        tool_call: _ToolCall,
        schema: a11.ActionSchema,
        validate_integrity: bool = True,
    ) -> _ToolCall:
        if validate_integrity:
            tool_call = ActionCallAdapter._validate_tool_call_integrity(
                tool_call
            )

        if tool_call.name != schema.name:
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message=f"Tool call name must be {schema.name}.",
            ).to_exception()

        for actual_input in tool_call.params.keys():
            if actual_input not in schema.inputs:
                raise Status(
                    code=StatusCode.INVALID_ARGUMENT,
                    message=f"Tool call has unexpected input {actual_input}.",
                ).to_exception()

        for expected_input_name, expected_input in schema.inputs.items():
            if (
                expected_input.required
                and expected_input_name not in tool_call.params
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
                and tool_call.params.get(expected_input_name) is not None
            ):
                raise Status(
                    code=StatusCode.INVALID_ARGUMENT,
                    message="Tool call is trying to fill a prefilled input.",
                ).to_exception()

        return tool_call

    @staticmethod
    def create(tool_call: _ToolCall, schema: a11.ActionSchema):
        tool_call = ActionCallAdapter._validate_tool_call_integrity(tool_call)
        tool_call = ActionCallAdapter.validate_against_schema(
            tool_call, schema, validate_integrity=False
        )

        return ActionCallAdapter(tool_call, schema)


def _decode_action_output_fragments(
    fragments: list[a11.NodeFragment],
) -> Any:
    grouped: dict[str, list[a11.NodeFragment]] = {}
    for fragment in fragments:
        grouped.setdefault(fragment.id, []).append(fragment)

    values: dict[str, Any] = {}
    for field_name, field_fragments in grouped.items():
        # A null chunk is an end-of-stream marker, not a value: an action that
        # ends an output with `finalize()` (or reports "nothing here" on an
        # optional port) must not make the whole tool result undecodable.
        chunks = [fragment.get_chunk() for fragment in field_fragments]
        decoded = [
            a11.from_chunk(chunk) for chunk in chunks if not chunk.is_null()
        ]
        if not decoded:
            continue
        values[field_name] = decoded[0] if len(decoded) == 1 else decoded

    if list(values.keys()) == ["$"]:
        return values["$"]
    return values


async def _build_tool_results_from_outputs(
    executed: runner.ExecutedActions,
) -> list[dict[str, Any]]:
    """One `tool_result` per call: its outputs, or why it failed."""
    tool_results = []
    for tool_use_id, fragments in executed.outputs.items():
        failure = executed.error_message(tool_use_id)
        if failure is not None:
            # Told to the model rather than raised past it: a failed tool
            # is something it can react to — retry differently, or say
            # what went wrong — and the calls that worked still deserve
            # their answers.
            tool_results.append(
                {
                    "type": "tool_result",
                    "tool_use_id": tool_use_id,
                    "content": failure,
                    "is_error": True,
                }
            )
            continue

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
    tool_calls: list[_ToolCall],
    interaction: llm.Interaction,
    registry: a11.ActionRegistry,
):
    for tool_call in tool_calls:
        adapter = ActionCallAdapter.create(
            tool_call,
            registry.get_schema(tool_call.name),
        )

        interaction.action_calls.append(adapter.action_message)
        if tool_call.id not in interaction.action_inputs:
            interaction.action_inputs[tool_call.id] = []
        interaction.action_inputs[tool_call.id].extend(
            await adapter.get_action_inputs()
        )


def _build_usage_metadata(
    usage: anthropic.types.Usage | None,
) -> llm.UsageMetadata | None:
    """Map Anthropic's `Usage` onto the provider-independent `UsageMetadata`."""
    if usage is None:
        return None

    reasoning_tokens = None
    if usage.output_tokens_details is not None:
        reasoning_tokens = usage.output_tokens_details.thinking_tokens

    cache_read = usage.cache_read_input_tokens or 0
    cache_write = usage.cache_creation_input_tokens or 0
    # Anthropic reports `input_tokens` as the uncached remainder, so the full
    # token count for the interaction adds the cached-read and cache-write
    # input tokens on top of the (uncached) input and the output tokens.
    total_tokens = (
        usage.input_tokens + usage.output_tokens + cache_read + cache_write
    )

    return llm.UsageMetadata(
        input_tokens=usage.input_tokens,
        output_tokens=usage.output_tokens,
        total_tokens=total_tokens,
        cached_input_tokens=usage.cache_read_input_tokens,
        cache_write_tokens=usage.cache_creation_input_tokens,
        reasoning_tokens=reasoning_tokens,
    )


def _encode_backend_value(value: Any) -> bytes:
    """Encode a backend-specific metadata value as bytes.

    `Interaction.backend_specific_metadata` is a `dict[str, bytes]`; scalars are
    stored as their UTF-8 encoding and structured values (SDK models, dicts) as
    their JSON encoding.
    """
    if isinstance(value, bytes):
        return value
    if isinstance(value, str):
        return value.encode()
    if hasattr(value, "model_dump"):
        value = value.model_dump(exclude_none=True)
    return pydantic_core.to_json(value)


def _build_backend_specific_metadata(
    snapshot: anthropic.types.Message,
) -> dict[str, bytes]:
    """Collect Anthropic-specific fields that don't map onto shared models."""
    metadata: dict[str, bytes] = {
        llm.BACKEND_METADATA_KEY: str(llm.Backend.CLAUDE).encode()
    }

    for field in ("container", "stop_reason", "stop_details"):
        value = getattr(snapshot, field, None)
        if value is not None:
            metadata[field] = _encode_backend_value(value)

    # Usage counters that are specific to Anthropic and have no place in the
    # provider-independent `UsageMetadata`.
    usage = snapshot.usage
    if usage is not None:
        for field in (
            "service_tier",
            "inference_geo",
            "server_tool_use",
            "cache_creation",
        ):
            value = getattr(usage, field, None)
            if value is not None:
                metadata[field] = _encode_backend_value(value)

    return metadata


def _build_thinking(
    config: CreateMessageConfig, model: str, has_tools: bool
) -> Any:
    """Adaptive thinking config, guarded by model and tool constraints."""
    if not config.thinking or "haiku" in model or has_tools:
        return anthropic.Omit()
    thinking: dict[str, Any] = {"type": "adaptive"}
    if config.thinking_summaries:
        thinking["display"] = "summarized"
    return thinking


def _build_output_config(config: CreateMessageConfig, model: str) -> Any:
    """Output effort config, honoured only where the model supports it."""
    if config.effort is None or "haiku" in model:
        return anthropic.Omit()
    return {"effort": config.effort}


def _build_server_tools(config: CreateMessageConfig) -> list[dict[str, Any]]:
    """Claude's built-in, server-side tools enabled via config toggles."""
    tools: list[dict[str, Any]] = []
    if config.web_search:
        tools.append({"type": "web_search_20260209", "name": "web_search"})
    if config.web_fetch:
        tools.append({"type": "web_fetch_20260209", "name": "web_fetch"})
    if config.code_execution:
        tools.append(
            {"type": "code_execution_20260521", "name": "code_execution"}
        )
    return tools


async def interact_with_claude(action: a11.Action):
    deadline = a11.get_deadline(action)

    def remaining_timeout():
        return max(deadline - a11.now(), a11.zero_duration())

    api_key = action.get_header(llm.LlmHeaders.API_KEY.value, decode=True)
    if api_key is None:
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message="API key is required.",
        ).to_exception()
    api_key: str

    model: str = (
        action.get_header(llm.LlmHeaders.MODEL.value, decode=True)
        or DEFAULT_MODEL
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

    # Record the LLM span's model and input for tracing backends (e.g.
    # Langfuse). Guarded: tracing must never affect the interaction.
    if action.trace_id:
        try:
            action.set_span_name("Claude interaction")
            action.set_span_attribute("gen_ai.system", "anthropic")
            action.set_span_attribute("gen_ai.request.model", model)
            action.set_span_input(
                [
                    {"role": message["role"], "content": message["content"]}
                    for message in conversation.messages
                ]
            )
        except Exception:
            logging.debug("failed to record LLM span input", exc_info=True)

    client = get_anthropic_client(api_key)

    tools = await runner.collect_tools(action, deadline)
    tools.extend(_build_server_tools(config))

    thinking = _build_thinking(config, model, bool(tools))
    output_config = _build_output_config(config, model)

    try:
        while True:
            snapshot = None
            try:
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

            tool_calls: list[_ToolCall] = []
            pending_tool_calls: dict[int, _ToolCall] = {}

            async for event in stream:
                await action["event_stream"].put(event)
                snapshot = accumulate_event(
                    event=event, current_snapshot=snapshot
                )

                if event.type == "content_block_start":
                    if event.content_block.type == "tool_use":
                        pending_tool_calls[event.index] = _ToolCall(
                            name=event.content_block.name,
                            id=event.content_block.id,
                        )

                if event.type == "content_block_delta":
                    delta = event.delta

                    if delta.type == "input_json_delta":
                        pending_tool_calls[event.index].apply_input_delta(
                            delta.partial_json
                        )
                    elif delta.type == "text_delta":
                        if delta.text:
                            await action["text_output"].put(delta.text)
                    elif delta.type == "thinking_delta":
                        if delta.thinking:
                            await action["thoughts"].put(delta.thinking)

                if (
                    event.type == "content_block_stop"
                    and event.index in pending_tool_calls
                ):
                    tool_call = pending_tool_calls.pop(event.index)
                    await tool_call.finalize_params()
                    tool_calls.append(tool_call)

            if snapshot is None:
                raise Status(
                    code=StatusCode.DATA_LOSS,
                    message="No message could be accumulated.",
                ).to_exception()

            interaction = llm.Interaction(
                previous_interaction_id=previous_interaction_id,
                role=llm.Role.ASSISTANT,
                created_at_millis=a11.now().nanoseconds_since_epoch // 1000000,
                model=snapshot.model,
                content=[
                    await asyncio.to_thread(
                        a11.to_chunk, snapshot.model_dump(exclude_none=True)
                    )
                ],
                backend_specific_metadata=_build_backend_specific_metadata(
                    snapshot
                ),
                usage_metadata=_build_usage_metadata(snapshot.usage),
            )
            previous_interaction_id = interaction.id
            await _add_tool_calls_to_interaction(
                tool_calls, interaction, action.get_registry()
            )

            interaction = conversation.feed_next_interaction(interaction)

            await action["new_interactions"].put(interaction)
            if not interaction.action_calls:
                if action.trace_id:
                    try:
                        action.set_span_output(
                            snapshot.model_dump(exclude_none=True)
                        )
                    except Exception:
                        logging.debug(
                            "failed to record LLM span output", exc_info=True
                        )
                break

            executed = await runner.execute_actions_from_interaction(
                interaction, action, action.get_registry()
            )

            tool_output_interaction = llm.Interaction(
                previous_interaction_id=previous_interaction_id,
                role=llm.Role.USER,
                created_at_millis=a11.now().nanoseconds_since_epoch // 1000000,
                action_outputs=executed.outputs,
                backend_specific_metadata={
                    llm.BACKEND_METADATA_KEY: str(llm.Backend.CLAUDE).encode(),
                    # What the tools said to the user, kept beside their
                    # results rather than in them: metadata is the one part of
                    # an interaction no backend turns into provider content.
                    **executed.log_metadata(),
                },
                content=[
                    a11.to_chunk(
                        {
                            "role": "user",
                            "content": await _build_tool_results_from_outputs(
                                executed
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
        await action["event_stream"].finalize()
        await action["text_output"].finalize()
        await action["thoughts"].finalize()
        await action["new_interactions"].finalize()

    finally:
        pass
