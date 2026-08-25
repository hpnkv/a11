# Copyright 2026 The A11 Authors.

import asyncio
import contextlib
import traceback
import uuid
import warnings
from collections.abc import Callable
from typing import Any

from absl import logging

import a11
import ollama

from a11.status import Status, StatusCode, StatusException

from a11.sdk.ollama.client import get_ollama_client
from a11.sdk.ollama.interact_with_ollama_schema import (
    CreateChatConfig,
    DEFAULT_MODEL,
)
from a11.sdk import llm
from a11.sdk.llm_tools import runner


def _message_from_neutral_parts(role: str, parts: list[Any]) -> dict[str, Any]:
    """Fold the backend-neutral ``{"type": ...}`` part list into an Ollama
    message.

    The portable text/image envelope produced by other handlers carries content
    as a list of parts; Ollama wants a flat `content` string plus a separate
    `images` list.
    """
    text_chunks: list[str] = []
    images: list[str] = []
    for part in parts or []:
        if not isinstance(part, dict):
            continue
        part_type = part.get("type")
        if part_type == "text":
            text_chunks.append(part.get("text", ""))
        elif part_type == "image":
            data = part.get("data")
            if data:
                images.append(data)

    message: dict[str, Any] = {"role": role, "content": "".join(text_chunks)}
    if images:
        message["images"] = images
    return message


def _clean_native_message(message: dict[str, Any]) -> dict[str, Any]:
    """Keep only the fields Ollama accepts on an inbound chat message."""
    cleaned: dict[str, Any] = {"role": message.get("role", "user")}
    for field in ("content", "thinking", "images", "tool_calls", "tool_name"):
        value = message.get(field)
        if value is not None:
            cleaned[field] = value
    cleaned.setdefault("content", "")
    return cleaned


def _ollama_to_normalized(
    interaction: llm.Interaction,
) -> llm.NormalizedMessage:
    """Produce the normalized view of an Ollama-native interaction.

    Ollama tool calls carry no id of their own, so a call is keyed by its
    function name (results are likewise matched back by name); this is the
    identifier surfaced to another backend that consumes this interaction.
    """
    content = a11.from_chunk(interaction.content[0])

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
            message="Unrecognized Ollama interaction content.",
        ).to_exception()

    # A persisted tool-result batch: one normalized message of tool results.
    if "messages" in content:
        parts: list[llm.NormalizedPart] = []
        for message in content.get("messages") or []:
            if not isinstance(message, dict):
                continue
            parts.append(
                llm.NormalizedPart(
                    type=llm.NormalizedContentType.TOOL_RESULT,
                    call_id=message.get("tool_name"),
                    content=llm.stringify_content(message.get("content")),
                )
            )
        return llm.NormalizedMessage(role=llm.Role.USER, parts=parts)

    role = (
        llm.Role.ASSISTANT
        if content.get("role") == "assistant"
        else llm.Role.USER
    )
    parts = []
    text = content.get("content")
    if isinstance(text, str) and text:
        parts.append(
            llm.NormalizedPart(type=llm.NormalizedContentType.TEXT, text=text)
        )
    for image in content.get("images") or []:
        parts.append(
            llm.NormalizedPart(type=llm.NormalizedContentType.IMAGE, data=image)
        )
    for tool_call in content.get("tool_calls") or []:
        function = tool_call.get("function") or {}
        name = function.get("name") or ""
        parts.append(
            llm.NormalizedPart(
                type=llm.NormalizedContentType.TOOL_CALL,
                id=name,
                name=name,
                arguments=function.get("arguments") or {},
            )
        )
    return llm.NormalizedMessage(role=role, parts=parts)


def _ollama_from_normalized(
    message: llm.NormalizedMessage,
    resolve_call_name: Callable[[str], str | None] | None = None,
) -> list[dict[str, Any]]:
    """Translate a normalized message into one or more Ollama messages.

    Text and images fold into a single message; each tool result becomes its
    own ``role: "tool"`` message, as Ollama expects.

    Ollama identifies a tool result by *name*, where every other backend
    identifies it by the id of the call it answers — so a result bridged from
    one of them arrives with an id and no name. ``resolve_call_name`` is what
    turns that id back into the name the model called, from the tool calls seen
    earlier in this conversation; without it a Claude conversation continued on
    Ollama sends `tool_name: "toolu_01..."`, a name the model never saw and
    cannot match to anything it asked for.
    """
    role = "assistant" if message.role == llm.Role.ASSISTANT else "user"
    text_chunks: list[str] = []
    images: list[str] = []
    tool_calls: list[dict[str, Any]] = []
    tool_results: list[dict[str, Any]] = []

    for part in message.parts:
        if part.type == llm.NormalizedContentType.TEXT:
            text_chunks.append(part.text or "")
        elif part.type == llm.NormalizedContentType.IMAGE:
            if part.data:
                images.append(part.data)
        elif part.type == llm.NormalizedContentType.TOOL_CALL:
            tool_calls.append(
                {
                    "function": {
                        "name": part.name or "",
                        "arguments": part.arguments or {},
                    }
                }
            )
        elif part.type == llm.NormalizedContentType.TOOL_RESULT:
            name = part.name
            if not name and part.call_id and resolve_call_name is not None:
                name = resolve_call_name(part.call_id)
            tool_results.append(
                {
                    "role": "tool",
                    "content": part.content or "",
                    "tool_name": name or part.call_id or "",
                }
            )

    messages: list[dict[str, Any]] = []
    if text_chunks or images or tool_calls:
        message_dict: dict[str, Any] = {
            "role": role,
            "content": "".join(text_chunks),
        }
        if images:
            message_dict["images"] = images
        if tool_calls:
            message_dict["tool_calls"] = tool_calls
        messages.append(message_dict)
    messages.extend(tool_results)
    return messages


llm.register_interaction_normalizer(llm.Backend.OLLAMA, _ollama_to_normalized)


class Conversation:
    """Accumulates the flat message list Ollama replays on every turn.

    Ollama's chat API is stateless, so — like the Claude handler — the whole
    transcript is kept as an ordered list of messages and re-sent each request.
    A single interaction can expand into several messages (a batch of tool
    results becomes one ``role: "tool"`` message apiece).
    """

    _interactions: list[llm.Interaction]
    _messages: list[dict[str, Any]]
    _system_instructions: list[str]
    _call_names: dict[str, str]

    def __init__(self):
        self._interactions = []
        self._messages = []
        self._system_instructions = []
        # Call id → tool name, learned from the tool calls of every foreign
        # assistant turn fed in, so their results can be named for Ollama.
        self._call_names = {}

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
        if backend is not None and backend != llm.Backend.OLLAMA:
            # Produced by another backend: bridge it through the normalized
            # representation and leave the interaction's own content untouched.
            normalized = llm.normalize_interaction(interaction)
            for part in normalized.parts:
                if (
                    part.type == llm.NormalizedContentType.TOOL_CALL
                    and part.id
                    and part.name
                ):
                    self._call_names[part.id] = part.name
            messages = _ollama_from_normalized(normalized, self._call_names.get)
        else:
            # Tagged as ours, or untagged (optimistically treated as native).
            messages = self._native_messages(interaction)

        if self._interactions and not interaction.previous_interaction_id:
            interaction.previous_interaction_id = self._interactions[-1].id

        self._messages.extend(messages)
        self._interactions.append(interaction)

        return interaction

    @staticmethod
    def _native_messages(
        interaction: llm.Interaction,
    ) -> list[dict[str, Any]]:
        content = a11.from_chunk(interaction.content[0])

        if isinstance(content, str):
            return [{"role": "user", "content": content}]

        if not isinstance(content, dict):
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message="Interaction content must be a string or a mapping.",
            ).to_exception()

        # A tool-result batch we persisted: a ready list of Ollama messages.
        if "messages" in content:
            return [
                _clean_native_message(message)
                for message in content.get("messages") or []
                if isinstance(message, dict)
            ]

        role = content.get("role") or "user"
        # Gemini-style envelopes tag the assistant role as "model".
        if role == "model":
            role = "assistant"

        inner = content.get("content")
        if isinstance(inner, list):
            # Backend-neutral ``{"role", "content": [parts]}`` envelope.
            return [_message_from_neutral_parts(role, inner)]

        # Otherwise a native Ollama message (content is a string or absent).
        message = dict(content)
        message["role"] = role
        return [_clean_native_message(message)]


class _StreamAccumulator:
    """Reconstructs the assistant message from streamed chat chunks.

    Ollama streams the reply as a sequence of `ChatResponse` chunks, each
    carrying a delta of `content` and/or `thinking`. Tool calls arrive whole
    (their arguments already parsed) rather than as a partial-JSON stream, so
    they are simply collected as they appear.

    Ollama tool calls carry no id of their own, so one is synthesised — and a
    synthesised id has to be unique for the life of the *session*, not of this
    turn. The runner gives a call's nested action that id verbatim, and an
    action's node ids are `"<id>#<port>"` in the session's node map, so a
    repeated id resolves to an earlier call's already-closed nodes: feeding the
    new call's inputs then fails with "ChunkStoreWriter is closed", and the model
    is handed that instead of a tool result.

    Hence both halves of an id. ``prefix`` is drawn once per turn, which is what
    keeps the second user message in a conversation from reusing the first's
    ``call_0`` — the counter alone is per-invocation and resets. ``base_id``
    then keeps the rounds *within* a turn apart, each of which runs its own
    accumulator.
    """

    def __init__(self, prefix: str, base_id: int = 0):
        self._prefix = prefix
        self._base_id = base_id
        self._content = ""
        self._thinking = ""
        self._tool_calls: list[llm.ToolCall] = []
        self._raw_tool_calls: list[dict[str, Any]] = []

    def add(self, message: Any) -> None:
        content = getattr(message, "content", None)
        if content:
            self._content += content
        thinking = getattr(message, "thinking", None)
        if thinking:
            self._thinking += thinking
        for tool_call in getattr(message, "tool_calls", None) or []:
            function = tool_call.function
            index = self._base_id + len(self._tool_calls)
            self._tool_calls.append(
                llm.ToolCall(
                    name=function.name,
                    id=f"{self._prefix}_{index}",
                    params=dict(function.arguments or {}),
                )
            )
            self._raw_tool_calls.append(
                {
                    "function": {
                        "name": function.name,
                        "arguments": dict(function.arguments or {}),
                    }
                }
            )

    @property
    def content(self) -> str:
        return self._content

    @property
    def tool_calls(self) -> list[llm.ToolCall]:
        return self._tool_calls

    def message_dict(self) -> dict[str, Any]:
        """The reconstructed assistant message, persisted as native content."""
        message: dict[str, Any] = {
            "role": "assistant",
            "content": self._content,
        }
        if self._thinking:
            message["thinking"] = self._thinking
        if self._raw_tool_calls:
            message["tool_calls"] = self._raw_tool_calls
        return message


async def _build_tool_results_from_outputs(
    executed: runner.ExecutedActions,
    call_names: dict[str, str],
) -> list[dict[str, Any]]:
    """Turn each nested-action output or failure into a tool message."""

    def as_tool_message(
        call_id: str, content: str, failure: str | None
    ) -> dict[str, Any]:
        message: dict[str, Any] = {
            "role": "tool",
            "content": content if failure is None else f"Error: {failure}",
        }
        if call_names.get(call_id):
            message["tool_name"] = call_names[call_id]
        return message

    return await llm.build_tool_results(executed, as_tool_message)


def _build_usage_metadata(snapshot: Any | None) -> llm.UsageMetadata | None:
    """Map Ollama's token counts onto the provider-independent metadata."""
    if snapshot is None:
        return None

    input_tokens = getattr(snapshot, "prompt_eval_count", None)
    output_tokens = getattr(snapshot, "eval_count", None)
    total_tokens = None
    if input_tokens is not None or output_tokens is not None:
        total_tokens = (input_tokens or 0) + (output_tokens or 0)

    return llm.UsageMetadata(
        input_tokens=input_tokens,
        output_tokens=output_tokens,
        total_tokens=total_tokens,
    )


def _build_backend_specific_metadata(snapshot: Any) -> dict[str, bytes]:
    """Collect Ollama-specific fields that don't map onto shared models.

    The timing durations Ollama reports have no place in the shared
    `UsageMetadata`, so they (and `done_reason`) are stashed here.
    """
    metadata: dict[str, bytes] = {
        llm.BACKEND_METADATA_KEY: str(llm.Backend.OLLAMA).encode()
    }
    if snapshot is None:
        return metadata

    for field in (
        "done_reason",
        "total_duration",
        "load_duration",
        "prompt_eval_duration",
        "eval_duration",
    ):
        value = getattr(snapshot, field, None)
        if value is not None:
            metadata[field] = llm.encode_backend_value(str(value))

    return metadata


def _build_options(config: CreateChatConfig) -> dict[str, Any]:
    """Assemble Ollama's `options` bag from the sampling knobs on the config."""
    options: dict[str, Any] = {"num_predict": config.num_predict}
    if config.temperature is not None:
        options["temperature"] = config.temperature
    if config.top_p is not None:
        options["top_p"] = config.top_p
    if config.top_k is not None:
        options["top_k"] = config.top_k
    if config.seed is not None:
        options["seed"] = config.seed
    return options


class _PassthroughTool(ollama.Tool):
    """A tool whose JSON Schema reaches the model the way it was written.

    The Ollama SDK's own `Tool` is not a JSON Schema carrier: a parameter is a
    `Tool.Function.Parameters.Property`, which has `type`, `items`,
    `description` and `enum` and nothing else. Hand it a schema with an object
    parameter and the object's *own* `properties` and `required` are dropped on
    validation, without a word — so an action whose input port takes
    `{"query", "max_results"}` reaches the model as an opaque
    `request: {"type": "object"}`, and the model has to guess at fields it was
    never shown. Every other backend gets the schema `collect_tools` produced.

    Overriding `function` with an unconstrained type is what stops that: the
    client validates each tool with `Tool.model_validate`, which passes a
    subclass instance straight through instead of coercing it, so the schema is
    serialized as given.

    Pydantic still notices that the value is not a `Function` while serializing
    the request, and says so — see [_serializing_a_raw_tool_schema][]. The
    payload is unaffected either way.
    """

    function: Any = None


@contextlib.contextmanager
def _serializing_a_raw_tool_schema():
    """Silence the one warning `_PassthroughTool` is expected to produce.

    Handing pydantic a value that is deliberately not of the declared type earns
    a `PydanticSerializationUnexpectedValue` warning per serialized tool, from a
    stack that varies enough that the interpreter's "once per location" rule does
    not collapse them: a handful of lines of pydantic internals in the log for
    every turn, describing something already known and intended.

    Narrow on purpose — this filter matches that warning's own text, so anything
    else pydantic or the SDK has to say still comes through. The window is the
    `chat` call itself, which builds and serializes the request without ever
    suspending, so no other task can run inside it (`warnings` filters are
    process-global, and this would otherwise not be ours to change).
    """
    with warnings.catch_warnings():
        warnings.filterwarnings(
            "ignore",
            message="Pydantic serializer warnings",
            category=UserWarning,
        )
        yield


def _build_tools(
    requested_tools: list[dict[str, Any]],
) -> list[ollama.Tool]:
    """Convert A11 tool definitions into Ollama function tools."""
    tools: list[ollama.Tool] = []
    for tool in requested_tools:
        tools.append(
            _PassthroughTool(
                type="function",
                function={
                    "name": tool["name"],
                    "description": tool.get("description", ""),
                    "parameters": tool.get("input_schema", {}),
                },
            )
        )
    return tools


async def interact_with_ollama(action: a11.Action):
    deadline = a11.get_deadline(action)

    def remaining_timeout():
        return max(deadline - a11.now(), a11.zero_duration())

    # Ollama commonly runs locally without credentials, so the key is optional.
    api_key = action.get_header(llm.LlmHeaders.API_KEY.value, decode=True)
    base_url = action.get_header(llm.LlmHeaders.BASE_URL.value, decode=True)

    model: str = (
        action.get_header(llm.LlmHeaders.MODEL.value, decode=True)
        or DEFAULT_MODEL
    )

    config = await action["config"].consume(
        CreateChatConfig, timeout=remaining_timeout(), allow_none=True
    )
    if config is None:
        config = CreateChatConfig()

    previous_interaction_id = ""
    conversation = Conversation()
    async for interaction in action["interactions"]:
        interaction = conversation.feed_next_interaction(interaction)
        previous_interaction_id = interaction.id

    # Record the LLM span's model and input for tracing backends (e.g.
    # Langfuse). Guarded: tracing must never affect the interaction.
    if action.trace_id:
        try:
            action.set_span_name("Ollama interaction")
            action.set_span_attribute("gen_ai.system", "ollama")
            action.set_span_attribute("gen_ai.request.model", model)
            action.set_span_input(conversation.messages)
        except Exception:
            logging.debug("failed to record LLM span input", exc_info=True)

    client = get_ollama_client(base_url, api_key)

    tools = await runner.collect_tools(action, deadline)
    ollama_tools = _build_tools(tools)
    options = _build_options(config)

    # Tool-call ids must be unique across the whole session, not just within one
    # round or one turn (see `_StreamAccumulator`). The counter lives out here so
    # it advances by the number of tool calls each round produces, and the prefix
    # is drawn per turn so the *next* user message in this conversation cannot
    # collide with this one's calls — a caller keeps one session for a whole
    # conversation, and every turn's handler starts this counter at zero.
    call_id_prefix = f"call_{uuid.uuid4().hex[:12]}"
    next_tool_call_id = 0
    try:
        while True:
            messages: list[dict[str, Any]] = []
            if conversation.system_prompt:
                messages.append(
                    {"role": "system", "content": conversation.system_prompt}
                )
            messages.extend(conversation.messages)

            try:
                with _serializing_a_raw_tool_schema():
                    stream = await client.chat(
                        model=model,
                        messages=messages,
                        tools=ollama_tools or None,
                        stream=True,
                        think=config.think,
                        format="json" if config.json_output else None,
                        options=options,
                        keep_alive=config.keep_alive,
                    )
            except ollama.ResponseError as exc:
                raise Status(
                    code=StatusCode.INTERNAL, message=str(exc)
                ).to_exception() from exc

            accumulator = _StreamAccumulator(call_id_prefix, next_tool_call_id)
            snapshot = None

            async for chunk in stream:
                await action["event_stream"].put(chunk)

                message = getattr(chunk, "message", None)
                if message is not None:
                    accumulator.add(message)
                    if message.content:
                        await action["text_output"].put(message.content)
                    if message.thinking:
                        await action["thoughts"].put(message.thinking)

                if getattr(chunk, "done", False):
                    snapshot = chunk

            tool_calls = accumulator.tool_calls
            next_tool_call_id += len(tool_calls)
            message_dict = accumulator.message_dict()

            snapshot_model = (
                str(snapshot.model)
                if snapshot is not None and snapshot.model
                else model
            )
            interaction = llm.Interaction(
                previous_interaction_id=previous_interaction_id,
                role=llm.Role.ASSISTANT,
                created_at_millis=a11.now().nanoseconds_since_epoch // 1000000,
                model=snapshot_model,
                content=[await asyncio.to_thread(a11.to_chunk, message_dict)],
                backend_specific_metadata=_build_backend_specific_metadata(
                    snapshot
                ),
                usage_metadata=_build_usage_metadata(snapshot),
            )
            previous_interaction_id = interaction.id
            await llm.add_tool_calls_to_interaction(
                tool_calls, interaction, action.get_registry()
            )

            interaction = conversation.feed_next_interaction(interaction)

            await action["new_interactions"].put(interaction)
            if not interaction.action_calls:
                if action.trace_id:
                    try:
                        action.set_span_output(message_dict)
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
                    llm.BACKEND_METADATA_KEY: str(llm.Backend.OLLAMA).encode(),
                    # What the tools said to the user, kept beside their
                    # results rather than in them: metadata is the one part of
                    # an interaction no backend turns into provider content.
                    **executed.log_metadata(),
                },
                content=[
                    a11.to_chunk(
                        {
                            "messages": await _build_tool_results_from_outputs(
                                executed, call_names
                            )
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
