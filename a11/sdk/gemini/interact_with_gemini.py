# Copyright 2026 The A11 Authors.

import asyncio
import dataclasses
import traceback
from typing import Any

from absl import logging

import a11
import pydantic_core

from google.genai import errors as genai_errors

from a11.status import Status, StatusCode, StatusException

from a11.sdk.gemini.client import get_gemini_client
from a11.sdk.gemini.interact_with_gemini_schema import (
    CreateInteractionConfig,
    DEFAULT_MODEL,
)
from a11.sdk import llm
from a11.sdk.llm_tools import runner

# The Gemini server-side interaction id is stashed here on an assistant
# interaction's `backend_specific_metadata` so subsequent turns can resume the
# stored conversation through `previous_interaction_id`.
_GEMINI_INTERACTION_ID = "gemini_interaction_id"


# Discriminators of Interactions API *steps* (as opposed to content parts).
# A content list whose items are all steps is passed through untouched; any
# other list is wrapped in a `user_input` step.
_STEP_TYPES = frozenset(
    {
        "user_input",
        "model_output",
        "thought",
        "function_call",
        "function_result",
        "code_execution_call",
        "code_execution_result",
        "url_context_call",
        "url_context_result",
        "google_search_call",
        "google_search_result",
        "mcp_server_tool_call",
        "mcp_server_tool_result",
        "file_search_call",
        "file_search_result",
        "google_maps_call",
        "google_maps_result",
    }
)

# Steps the model itself produces; these are replayed as history in
# "full-history" mode. Server-tool call/result steps are managed by the server
# and are intentionally left out.
_MODEL_STEP_TYPES = frozenset({"model_output", "function_call", "thought"})


async def _close_stream(node: a11.AsyncNode) -> None:
    """Terminate a streaming output node and flush it to its store."""
    await node.put_null_final()
    await node.drain_and_close()


def _content_to_steps(content: Any) -> list[dict[str, Any]]:
    """Normalize an interaction's content into a list of Interactions steps.

    User content — a bare string, a list of content parts (``{"type": "text",
    ...}``, ``{"type": "image", ...}``, …), or a ``{"role", "content"}``
    envelope à la the Claude handler — becomes a single ``user_input`` step.
    Content that is already a list of steps (e.g. ``function_result`` steps
    produced from tool outputs) is passed through unchanged.
    """
    if isinstance(content, dict):
        inner = content.get("content")
        if isinstance(inner, (list, str)):
            content = inner
        elif content.get("type"):
            content = [content]

    if isinstance(content, str):
        return [
            {
                "type": "user_input",
                "content": [{"type": "text", "text": content}],
            }
        ]

    if isinstance(content, list):
        if content and all(
            isinstance(item, dict) and item.get("type") in _STEP_TYPES
            for item in content
        ):
            return content
        return [{"type": "user_input", "content": content}]

    raise Status(
        code=StatusCode.INVALID_ARGUMENT,
        message=(
            "Interaction content must be a string, a list of parts or steps,"
            " or a mapping with a `content` field."
        ),
    ).to_exception()


def _stringify_result(result: Any) -> str:
    """Flatten a `function_result` payload into plain text."""
    if result is None:
        return ""
    if isinstance(result, str):
        return result
    if isinstance(result, list):
        texts = [
            item.get("text", "")
            for item in result
            if isinstance(item, dict) and item.get("type") == "text"
        ]
        if texts:
            return "".join(texts)
    return pydantic_core.to_json(result).decode()


def _normalized_parts_from_step(
    step: dict[str, Any],
) -> list[llm.NormalizedPart]:
    step_type = step.get("type")
    if step_type in ("user_input", "model_output"):
        parts: list[llm.NormalizedPart] = []
        for part in step.get("content") or []:
            part_type = part.get("type")
            if part_type == "text":
                parts.append(
                    llm.NormalizedPart(
                        type=llm.NormalizedContentType.TEXT,
                        text=part.get("text", ""),
                    )
                )
            elif part_type == "image":
                parts.append(
                    llm.NormalizedPart(
                        type=llm.NormalizedContentType.IMAGE,
                        data=part.get("data"),
                        mime_type=part.get("mime_type"),
                    )
                )
        return parts
    if step_type == "function_call":
        return [
            llm.NormalizedPart(
                type=llm.NormalizedContentType.TOOL_CALL,
                id=step.get("id"),
                name=step.get("name"),
                arguments=step.get("arguments") or {},
            )
        ]
    if step_type == "function_result":
        return [
            llm.NormalizedPart(
                type=llm.NormalizedContentType.TOOL_RESULT,
                call_id=step.get("call_id"),
                content=_stringify_result(step.get("result")),
            )
        ]
    # `thought` and server-tool steps carry no portable content.
    return []


def _gemini_to_normalized(
    interaction: llm.Interaction,
) -> llm.NormalizedMessage:
    """Produce the normalised view of a Gemini-native interaction."""
    content = a11.from_chunk(interaction.content[0])

    if isinstance(content, dict) and "steps" in content:
        # An assistant interaction dump: content lives in its steps.
        parts: list[llm.NormalizedPart] = []
        for step in content.get("steps") or []:
            if isinstance(step, dict):
                parts.extend(_normalized_parts_from_step(step))
        return llm.NormalizedMessage(role=llm.Role.ASSISTANT, parts=parts)

    role = (
        llm.Role.ASSISTANT
        if interaction.role == llm.Role.ASSISTANT
        else llm.Role.USER
    )
    parts = []
    for step in _content_to_steps(content):
        parts.extend(_normalized_parts_from_step(step))
    return llm.NormalizedMessage(role=role, parts=parts)


def _gemini_from_normalized(
    message: llm.NormalizedMessage,
) -> list[dict[str, Any]]:
    """Translate a normalised message into Gemini Interactions steps."""
    steps: list[dict[str, Any]] = []
    content_parts: list[dict[str, Any]] = []
    for part in message.parts:
        if part.type == llm.NormalizedContentType.TEXT:
            content_parts.append({"type": "text", "text": part.text or ""})
        elif part.type == llm.NormalizedContentType.IMAGE:
            content_parts.append(
                {
                    "type": "image",
                    "data": part.data,
                    "mime_type": part.mime_type,
                }
            )
        elif part.type == llm.NormalizedContentType.TOOL_CALL:
            steps.append(
                {
                    "type": "function_call",
                    "id": part.id or "",
                    "name": part.name or "",
                    "arguments": part.arguments or {},
                }
            )
        elif part.type == llm.NormalizedContentType.TOOL_RESULT:
            steps.append(
                {
                    "type": "function_result",
                    "call_id": part.call_id or "",
                    "result": [{"type": "text", "text": part.content or ""}],
                }
            )

    if content_parts:
        step_type = (
            "model_output"
            if message.role == llm.Role.ASSISTANT
            else "user_input"
        )
        steps.insert(0, {"type": step_type, "content": content_parts})
    return steps


llm.register_interaction_normalizer(llm.Backend.GEMINI, _gemini_to_normalized)


class Conversation:
    """Tracks A11 interactions and the Gemini state needed for the next turn.

    The Interactions API is stateful: the server stores each interaction and
    later turns can be resumed by id, so the cheap path sends only the newest
    input (`incremental_input`) plus a `previous_interaction_id`. For servers
    that do not retain interactions, the whole transcript is also kept as a flat
    list of steps (`full_input`) so the handler can replay it instead.
    """

    _interactions: list[llm.Interaction]
    _system_instructions: list[str]
    _history: list[dict[str, Any]]
    _pending: list[dict[str, Any]]
    _previous_server_interaction_id: str

    def __init__(self):
        self._interactions = []
        self._system_instructions = []
        self._history = []
        self._pending = []
        self._previous_server_interaction_id = ""

    @property
    def last_interaction_id(self):
        if not self._interactions:
            return None
        return self._interactions[-1].id

    @property
    def system_prompt(self) -> str | None:
        if not self._system_instructions:
            return None
        return "\n\n".join(self._system_instructions)

    @property
    def incremental_input(self) -> list[dict[str, Any]]:
        """The steps added since the last committed model turn."""
        return self._pending

    @property
    def full_input(self) -> list[dict[str, Any]]:
        """The entire transcript as a flat list of steps."""
        return self._history + self._pending

    @property
    def previous_server_interaction_id(self) -> str:
        return self._previous_server_interaction_id

    @staticmethod
    def _extract_model_steps(snapshot: dict[str, Any]) -> list[dict[str, Any]]:
        return [
            step
            for step in snapshot.get("steps") or []
            if isinstance(step, dict) and step.get("type") in _MODEL_STEP_TYPES
        ]

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

        role, steps, server_id = self._interpret(interaction)

        if role == llm.Role.ASSISTANT:
            # Commit the input we just sent plus the model's produced steps into
            # the transcript. Only a Gemini turn leaves resumable server-side
            # state; a foreign assistant turn (server_id is None) invalidates
            # the last id so the next turn replays the transcript in full
            # rather than resuming from a point that predates it.
            self._history.extend(self._pending)
            self._pending = []
            self._history.extend(steps)
            self._previous_server_interaction_id = server_id or ""
        else:
            self._pending.extend(steps)
            if server_id:
                self._previous_server_interaction_id = server_id

        if self._interactions and not interaction.previous_interaction_id:
            interaction.previous_interaction_id = self._interactions[-1].id

        self._interactions.append(interaction)

        return interaction

    def _interpret(
        self, interaction: llm.Interaction
    ) -> tuple[llm.Role, list[dict[str, Any]], str | None]:
        """Resolve (role, Gemini steps, server id) for an interaction.

        A foreign-tagged interaction is bridged through the normalised
        representation; one tagged as ours — or untagged, optimistically — is
        read as native Gemini content, raising if that turns out incompatible.
        Foreign interactions never carry a Gemini server id, which forces the
        next turn to replay the full transcript rather than resume by id.
        """
        backend = llm.interaction_backend(interaction)
        if backend is not None and backend != llm.Backend.GEMINI:
            normalized = llm.normalize_interaction(interaction)
            return normalized.role, _gemini_from_normalized(normalized), None

        server_id = interaction.backend_specific_metadata.get(
            _GEMINI_INTERACTION_ID
        )
        if isinstance(server_id, bytes):
            server_id = server_id.decode()

        content = a11.from_chunk(interaction.content[0])
        if interaction.role == llm.Role.ASSISTANT:
            steps = (
                self._extract_model_steps(content)
                if isinstance(content, dict)
                else []
            )
            return llm.Role.ASSISTANT, steps, server_id

        return llm.Role.USER, _content_to_steps(content), server_id


@dataclasses.dataclass
class _ToolCall:
    """A function call reconstructed from a Gemini `function_call` step."""

    name: str
    id: str
    params: dict[str, Any] = dataclasses.field(default_factory=dict)


class _StepAccumulator:
    """Reconstructs the model's steps from streamed Interactions events.

    In streaming mode the terminal `interaction.completed` event carries only
    metadata: the model's produced content (text, images, thought signatures,
    function-call arguments) arrives incrementally as `step.delta`s keyed by a
    step index. This rebuilds the ordered list of steps so the assistant turn
    can be persisted and, in `full-history` mode, replayed back verbatim.
    """

    _steps: dict[int, dict[str, Any]]

    def __init__(self):
        self._steps = {}

    def start(self, index: int, step: Any) -> None:
        step_type = getattr(step, "type", None)
        entry: dict[str, Any] = {"type": step_type}
        if step_type == "function_call":
            entry["id"] = step.id
            entry["name"] = step.name
            entry["arguments"] = dict(step.arguments or {})
            entry["_partial"] = ""
        elif step_type == "model_output":
            entry["content"] = []
        self._steps[index] = entry

    def _entry(self, index: int) -> dict[str, Any]:
        return self._steps.setdefault(index, {"type": None})

    def delta(self, index: int, delta: Any) -> None:
        entry = self._entry(index)
        delta_type = getattr(delta, "type", None)

        if delta_type == "text":
            self._append_text(entry, delta.text)
        elif delta_type == "image":
            entry.setdefault("type", "model_output")
            entry.setdefault("content", []).append(
                {
                    "type": "image",
                    "data": delta.data,
                    "mime_type": delta.mime_type,
                }
            )
        elif delta_type == "arguments_delta":
            if delta.arguments:
                entry["_partial"] = entry.get("_partial", "") + delta.arguments
        elif delta_type == "thought_signature":
            entry.setdefault("type", "thought")
            if delta.signature:
                entry["signature"] = delta.signature
        elif delta_type == "thought_summary":
            entry.setdefault("type", "thought")
            content = getattr(delta, "content", None)
            if content is not None and getattr(content, "type", None) == "text":
                entry["summary"] = entry.get("summary", "") + getattr(
                    content, "text", ""
                )

    @staticmethod
    def _append_text(entry: dict[str, Any], text: str | None) -> None:
        if not text:
            return
        entry.setdefault("type", "model_output")
        content = entry.setdefault("content", [])
        if content and content[-1].get("type") == "text":
            content[-1]["text"] += text
        else:
            content.append({"type": "text", "text": text})

    def finalize(self) -> list[dict[str, Any]]:
        steps: list[dict[str, Any]] = []
        for index in sorted(self._steps):
            entry = self._steps[index]
            partial = entry.pop("_partial", None)
            if entry.get("type") == "function_call":
                if partial:
                    entry["arguments"] = pydantic_core.from_json(partial)
                entry.setdefault("arguments", {})
            if entry.get("type") == "thought":
                summary = entry.pop("summary", None)
                if summary:
                    entry["summary"] = [{"type": "text", "text": summary}]
            steps.append(entry)
        return steps

    @staticmethod
    def tool_calls(steps: list[dict[str, Any]]) -> list[_ToolCall]:
        return [
            _ToolCall(
                name=step["name"],
                id=step["id"],
                params=step.get("arguments", {}),
            )
            for step in steps
            if step.get("type") == "function_call"
        ]


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
        # ends an output with `put_null_final` (or reports "nothing here" on an
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
    call_names: dict[str, str],
) -> list[dict[str, Any]]:
    """Turn nested-action outputs into Gemini `function_result` steps.

    A call that failed reports its failure as that call's result: the model can
    react to it, and the calls that succeeded keep their own answers.
    """
    tool_results = []
    for call_id, fragments in executed.outputs.items():
        failure = executed.error_message(call_id)
        if failure is not None:
            content = failure
        else:
            content = _decode_action_output_fragments(fragments)
            if not isinstance(content, str):
                content = (
                    await asyncio.to_thread(pydantic_core.to_json, content)
                ).decode()

        step: dict[str, Any] = {
            "type": "function_result",
            "call_id": call_id,
            "result": [{"type": "text", "text": content}],
        }
        if failure is not None:
            step["is_error"] = True
        if call_names.get(call_id):
            step["name"] = call_names[call_id]

        tool_results.append(step)

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


def _build_usage_metadata(usage: Any | None) -> llm.UsageMetadata | None:
    """Map Gemini's `Usage` onto the provider-independent `UsageMetadata`."""
    if usage is None:
        return None

    return llm.UsageMetadata(
        input_tokens=usage.total_input_tokens,
        output_tokens=usage.total_output_tokens,
        total_tokens=usage.total_tokens,
        cached_input_tokens=usage.total_cached_tokens,
        reasoning_tokens=usage.total_thought_tokens,
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
    snapshot: Any,
    interaction_id: str | None,
) -> dict[str, bytes]:
    """Collect Gemini-specific fields that don't map onto shared models."""
    metadata: dict[str, bytes] = {
        llm.BACKEND_METADATA_KEY: str(llm.Backend.GEMINI).encode()
    }

    server_id = getattr(snapshot, "id", None) or interaction_id
    if server_id:
        metadata[_GEMINI_INTERACTION_ID] = _encode_backend_value(str(server_id))

    for field in ("status", "service_tier", "object"):
        value = getattr(snapshot, field, None)
        if value is not None:
            metadata[field] = _encode_backend_value(str(value))

    return metadata


def _build_generation_config(config: CreateInteractionConfig) -> dict[str, Any]:
    generation_config: dict[str, Any] = {
        "max_output_tokens": config.max_output_tokens,
    }
    if config.thinking_level is not None:
        generation_config["thinking_level"] = config.thinking_level
    if config.thinking_summaries:
        generation_config["thinking_summaries"] = "auto"
    return generation_config


def _build_tools(
    requested_tools: list[dict[str, Any]],
    config: CreateInteractionConfig,
) -> list[dict[str, Any]]:
    """Convert A11 tool definitions and config toggles into Gemini tools."""
    tools: list[dict[str, Any]] = []
    for tool in requested_tools:
        tools.append(
            {
                "type": "function",
                "name": tool["name"],
                "description": tool.get("description", ""),
                "parameters": tool.get("input_schema", {}),
            }
        )

    if config.google_search:
        tools.append({"type": "google_search"})
    if config.code_execution:
        tools.append({"type": "code_execution"})
    if config.url_context:
        tools.append({"type": "url_context"})

    return tools


async def interact_with_gemini(action: a11.Action):
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
        CreateInteractionConfig, timeout=remaining_timeout(), allow_none=True
    )
    if config is None:
        config = CreateInteractionConfig()

    previous_interaction_id = ""
    conversation = Conversation()
    async for interaction in action["interactions"]:
        interaction = conversation.feed_next_interaction(interaction)
        previous_interaction_id = interaction.id

    # Record the LLM span's model and input for tracing backends (e.g.
    # Langfuse). Guarded: tracing must never affect the interaction.
    if action.trace_id:
        try:
            action.set_span_name("Gemini interaction")
            action.set_span_attribute("gen_ai.system", "google")
            action.set_span_attribute("gen_ai.request.model", model)
            action.set_span_input(conversation.full_input)
        except Exception:
            logging.debug("failed to record LLM span input", exc_info=True)

    client = get_gemini_client(api_key)

    allowed_patterns = llm.get_allowed_llm_action_patterns(action)
    requested_tools = [
        tool async for tool in action["tools"].iter_with_deadline(deadline)
    ]
    tools = []
    for tool in requested_tools:
        if not llm.action_name_matches_allowed(tool["name"], allowed_patterns):
            logging.warning(
                "Tool `%s` was requested, but isn't allowed.", tool["name"]
            )
            continue
        tools.append(tool)
    gemini_tools = _build_tools(tools, config)
    generation_config = _build_generation_config(config)

    state_mode = config.state_mode
    # "full-history" pins to replay; "last-id" always chains by id; "auto"
    # starts by chaining and flips this flag on the first unresolvable id.
    replay_full_history = state_mode == "full-history"

    def build_create_kwargs(full: bool) -> dict[str, Any]:
        kwargs: dict[str, Any] = {
            "model": model,
            "input": (
                conversation.full_input
                if full
                else conversation.incremental_input
            ),
            "stream": True,
            "generation_config": generation_config,
        }
        if conversation.system_prompt:
            kwargs["system_instruction"] = conversation.system_prompt
        if gemini_tools:
            kwargs["tools"] = gemini_tools
        if not full and conversation.previous_server_interaction_id:
            kwargs["previous_interaction_id"] = (
                conversation.previous_server_interaction_id
            )
        return kwargs

    try:
        while True:
            snapshot = None
            gemini_interaction_id = None
            have_prev = bool(conversation.previous_server_interaction_id)
            try:
                if replay_full_history or not have_prev:
                    # Nothing to resume from — a fresh conversation, or the
                    # previous turn was produced by another backend — so the
                    # whole transcript has to be replayed.
                    stream = await client.aio.interactions.create(
                        **build_create_kwargs(full=True)
                    )
                elif state_mode == "auto":
                    try:
                        stream = await client.aio.interactions.create(
                            **build_create_kwargs(full=False)
                        )
                    except genai_errors.APIError as exc:
                        logging.warning(
                            "Could not resume Gemini interaction %s (%s);"
                            " falling back to full-history replay.",
                            conversation.previous_server_interaction_id,
                            exc,
                        )
                        replay_full_history = True
                        stream = await client.aio.interactions.create(
                            **build_create_kwargs(full=True)
                        )
                else:
                    # "last-id" with a resumable server-side interaction.
                    stream = await client.aio.interactions.create(
                        **build_create_kwargs(full=False)
                    )
            except genai_errors.APIError as exc:
                raise Status(
                    code=StatusCode.INTERNAL, message=str(exc)
                ).to_exception() from exc

            accumulator = _StepAccumulator()

            async for event in stream:
                await action["event_stream"].put(event)

                event_type = event.event_type

                if event_type == "interaction.created":
                    gemini_interaction_id = event.interaction.id

                elif event_type == "step.start":
                    accumulator.start(event.index, event.step)

                elif event_type == "step.delta":
                    accumulator.delta(event.index, event.delta)

                    delta = event.delta
                    delta_type = getattr(delta, "type", None)
                    if delta_type == "text":
                        if delta.text:
                            await action["text_output"].put(delta.text)
                    elif delta_type == "thought_summary":
                        content = getattr(delta, "content", None)
                        if (
                            content is not None
                            and getattr(content, "type", None) == "text"
                            and getattr(content, "text", None)
                        ):
                            await action["thoughts"].put(content.text)

                elif event_type == "interaction.completed":
                    snapshot = event.interaction

                elif event_type == "error":
                    message = "Gemini interaction failed."
                    if event.error is not None and event.error.message:
                        message = event.error.message
                    raise Status(
                        code=StatusCode.INTERNAL, message=message
                    ).to_exception()

            # The completed event only carries metadata; the model's produced
            # steps are reconstructed from the streamed deltas.
            reconstructed_steps = accumulator.finalize()
            tool_calls = _StepAccumulator.tool_calls(reconstructed_steps)

            if snapshot is None:
                raise Status(
                    code=StatusCode.DATA_LOSS,
                    message="No interaction could be accumulated.",
                ).to_exception()

            if str(snapshot.status) == "failed":
                raise Status(
                    code=StatusCode.INTERNAL,
                    message="Gemini reported a failed interaction.",
                ).to_exception()

            snapshot_model = str(snapshot.model) if snapshot.model else model
            content_dict = snapshot.model_dump(exclude_none=True)
            content_dict["steps"] = reconstructed_steps
            interaction = llm.Interaction(
                previous_interaction_id=previous_interaction_id,
                role=llm.Role.ASSISTANT,
                created_at_millis=a11.now().nanoseconds_since_epoch // 1000000,
                model=snapshot_model,
                content=[await asyncio.to_thread(a11.to_chunk, content_dict)],
                backend_specific_metadata=_build_backend_specific_metadata(
                    snapshot, gemini_interaction_id
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
                        action.set_span_output(content_dict)
                    except Exception:
                        logging.debug(
                            "failed to record LLM span output", exc_info=True
                        )
                break

            executed = await runner.execute_actions_from_interaction(
                interaction, action, action.get_registry()
            )

            call_names = {call.id: call.name for call in tool_calls}
            tool_output_interaction = llm.Interaction(
                previous_interaction_id=previous_interaction_id,
                role=llm.Role.USER,
                created_at_millis=a11.now().nanoseconds_since_epoch // 1000000,
                action_outputs=executed.outputs,
                backend_specific_metadata={
                    llm.BACKEND_METADATA_KEY: str(llm.Backend.GEMINI).encode()
                },
                content=[
                    a11.to_chunk(
                        {
                            "role": "user",
                            "content": await _build_tool_results_from_outputs(
                                executed, call_names
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
        await _close_stream(action["event_stream"])
        await _close_stream(action["text_output"])
        await _close_stream(action["thoughts"])
        await action["new_interactions"].drain_and_close()

    finally:
        pass
