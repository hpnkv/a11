# Copyright 2026 The A11 Authors.

"""Drive one conversational turn against a vLLM deployment.

vLLM serves the OpenAI-compatible ``/v1/chat/completions`` route, so a
transcript is a flat list of chat messages, a tool call is an entry in an
assistant message's ``tool_calls``, and its result is a ``role: "tool"``
message naming the call it answers.
"""

import asyncio
import traceback
import uuid
from typing import Any

from absl import logging

import a11
import openai
import pydantic_core

from a11.status import Status, StatusCode, StatusException

from a11.sdk.vllm.client import get_vllm_client
from a11.sdk.vllm.interact_with_vllm_schema import (
    CreateChatCompletionConfig,
    DEFAULT_MODEL,
)
from a11.sdk import llm
from a11.sdk.llm_tools import runner


DEFAULT_IMAGE_MIME_TYPE = "image/png"


def _image_part(data: str, mime_type: str | None) -> dict[str, Any]:
    """One inline image as the ``image_url`` content part the route takes."""
    mime_type = mime_type or DEFAULT_IMAGE_MIME_TYPE
    return {
        "type": "image_url",
        "image_url": {"url": f"data:{mime_type};base64,{data}"},
    }


#: What a text-only tool message says about the frames the user message beside
#: it carries.
_IMAGE_RESULT_NOTE = "The images this tool returned follow in the next message."


def _split_data_url(url: str) -> tuple[str, str | None]:
    """The base64 payload and media type of a ``data:`` URL.

    A URL of any other scheme is returned as the payload with no media type,
    which keeps a remote image reference readable to a consumer that can fetch
    it.
    """
    if not url.startswith("data:"):
        return url, None
    header, _, payload = url.partition(",")
    mime_type = header[len("data:") :].split(";", 1)[0]
    return payload, mime_type or None


def _collapse_content(parts: list[dict[str, Any]]) -> Any:
    """A lone text part as a plain string, any other part list as it is."""
    if not parts:
        return ""
    if len(parts) == 1 and parts[0].get("type") == "text":
        return parts[0].get("text", "")
    return parts


def _message_from_neutral_parts(role: str, parts: list[Any]) -> dict[str, Any]:
    """Fold the backend-neutral ``{"type": ...}`` part list into a message.

    The portable text/image envelope other handlers produce carries content as
    a list of parts. Text parts are already the shape this route reads; an
    image part carries raw base64 and becomes an ``image_url`` data URL.
    """
    content: list[dict[str, Any]] = []
    for part in parts or []:
        if not isinstance(part, dict):
            continue
        part_type = part.get("type")
        if part_type == "text":
            content.append({"type": "text", "text": part.get("text", "")})
        elif part_type == "image":
            if data := part.get("data"):
                content.append(_image_part(data, part.get("mime_type")))
        elif part_type == "image_url":
            content.append(part)

    return {"role": role, "content": _collapse_content(content)}


#: The fields the chat route accepts on an inbound message. Everything else a
#: reconstructed message carries -- ``reasoning_content``, most of all -- stays
#: in the stored interaction and out of the request.
_INBOUND_MESSAGE_FIELDS = ("content", "tool_calls", "tool_call_id", "name")


def _clean_native_message(message: dict[str, Any]) -> dict[str, Any]:
    """Keep only the fields the chat route accepts on an inbound message."""
    cleaned: dict[str, Any] = {"role": message.get("role", "user")}
    for field in _INBOUND_MESSAGE_FIELDS:
        value = message.get(field)
        if value is not None:
            cleaned[field] = value
    cleaned.setdefault("content", "")
    return cleaned


def _decode_arguments(name: str, arguments: Any) -> dict[str, Any]:
    """Decode a tool call's arguments, which arrive as a JSON string."""
    if isinstance(arguments, dict):
        return arguments
    if not arguments:
        return {}
    try:
        decoded = pydantic_core.from_json(arguments)
    except ValueError as exc:
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message=(
                f"The model's arguments for tool {name!r} are not valid JSON:"
                f" {exc}"
            ),
        ).to_exception() from exc
    if not isinstance(decoded, dict):
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message=(
                f"The model's arguments for tool {name!r} decode to"
                f" {type(decoded).__name__}, not an object."
            ),
        ).to_exception()
    return decoded


def _encode_arguments(params: dict[str, Any]) -> str:
    """A tool call's arguments as the JSON string a message carries."""
    return pydantic_core.to_json(params or {}).decode()


def _normalized_content_parts(content: Any) -> list[llm.NormalizedPart]:
    """Normalize the text and images in one chat message's content."""
    if isinstance(content, str):
        return (
            [
                llm.NormalizedPart(
                    type=llm.NormalizedContentType.TEXT, text=content
                )
            ]
            if content
            else []
        )
    if not isinstance(content, list):
        return []

    parts: list[llm.NormalizedPart] = []
    for part in content:
        if not isinstance(part, dict):
            continue
        if part.get("type") == "text" and part.get("text"):
            parts.append(
                llm.NormalizedPart(
                    type=llm.NormalizedContentType.TEXT,
                    text=part.get("text"),
                )
            )
        elif part.get("type") == "image_url":
            url = (part.get("image_url") or {}).get("url") or ""
            data, mime_type = _split_data_url(url)
            if data:
                parts.append(
                    llm.NormalizedPart(
                        type=llm.NormalizedContentType.IMAGE,
                        data=data,
                        mime_type=mime_type,
                    )
                )
    return parts


def _vllm_to_normalized(
    interaction: llm.Interaction,
) -> llm.NormalizedMessage:
    """Produce the normalized view of a vLLM-native interaction.

    Tool calls and their results are keyed by the call id, which is the
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
            message="Unrecognized vLLM interaction content.",
        ).to_exception()

    # A persisted tool-result batch includes user messages carrying any images
    # that its text-only tool messages describe.
    if "messages" in content:
        parts: list[llm.NormalizedPart] = []
        for message in content.get("messages") or []:
            if not isinstance(message, dict):
                continue
            if message.get("role") == "tool":
                parts.append(
                    llm.NormalizedPart(
                        type=llm.NormalizedContentType.TOOL_RESULT,
                        call_id=message.get("tool_call_id"),
                        content=llm.stringify_content(message.get("content")),
                    )
                )
            else:
                parts.extend(_normalized_content_parts(message.get("content")))
        return llm.NormalizedMessage(role=llm.Role.USER, parts=parts)

    role = (
        llm.Role.ASSISTANT
        if content.get("role") == "assistant"
        else llm.Role.USER
    )
    parts = _normalized_content_parts(content.get("content"))
    for tool_call in content.get("tool_calls") or []:
        if not isinstance(tool_call, dict):
            continue
        function = tool_call.get("function") or {}
        name = function.get("name") or ""
        parts.append(
            llm.NormalizedPart(
                type=llm.NormalizedContentType.TOOL_CALL,
                id=tool_call.get("id") or name,
                name=name,
                arguments=_decode_arguments(name, function.get("arguments")),
            )
        )
    return llm.NormalizedMessage(role=role, parts=parts)


def _vllm_from_normalized(
    message: llm.NormalizedMessage,
) -> list[dict[str, Any]]:
    """Translate a normalized message into one or more chat messages.

    Text, images and tool calls fold into a single message; each tool result
    becomes its own ``role: "tool"`` message, which is the shape this route
    reads. A result identifies the call it answers by id, as the tool calls of
    every other id-keyed backend already do; a call bridged from Ollama is
    keyed by its function name on both sides and stays matched.
    """
    role = "assistant" if message.role == llm.Role.ASSISTANT else "user"
    content: list[dict[str, Any]] = []
    tool_calls: list[dict[str, Any]] = []
    tool_results: list[dict[str, Any]] = []

    for part in message.parts:
        if part.type == llm.NormalizedContentType.TEXT:
            content.append({"type": "text", "text": part.text or ""})
        elif part.type == llm.NormalizedContentType.IMAGE:
            if part.data:
                content.append(_image_part(part.data, part.mime_type))
        elif part.type == llm.NormalizedContentType.TOOL_CALL:
            tool_calls.append({
                "id": part.id or part.name or "",
                "type": "function",
                "function": {
                    "name": part.name or "",
                    "arguments": _encode_arguments(part.arguments or {}),
                },
            })
        elif part.type == llm.NormalizedContentType.TOOL_RESULT:
            tool_results.append({
                "role": "tool",
                "tool_call_id": part.call_id or part.id or "",
                "content": part.content or "",
            })

    messages: list[dict[str, Any]] = []
    if content or tool_calls:
        message_dict: dict[str, Any] = {
            "role": role,
            "content": _collapse_content(content),
        }
        if tool_calls:
            message_dict["tool_calls"] = tool_calls
        messages.append(message_dict)
    messages.extend(tool_results)
    return messages


llm.register_interaction_normalizer(llm.Backend.VLLM, _vllm_to_normalized)


class Conversation:
    """Accumulates the flat message list vLLM replays on every turn.

    The chat route is stateless, so the whole transcript is kept as an ordered
    list of messages and re-sent with each request. A single interaction can
    expand into several messages: a batch of tool results becomes one
    ``role: "tool"`` message apiece.
    """

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

        backend = llm.interaction_backend(interaction)
        chunk_content = len(interaction.content) != 1 or llm.has_image_chunks(
            interaction
        )
        if chunk_content:
            messages = _vllm_from_normalized(
                llm.normalize_by_shape(interaction)
            )
        elif backend is not None and backend != llm.Backend.VLLM:
            # Produced by another backend: bridge it through the normalized
            # representation and leave the interaction's own content untouched.
            messages = _vllm_from_normalized(
                llm.normalize_interaction(interaction)
            )
        else:
            # Tagged and untagged interactions use the native message shape.
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

        # A tool-result batch we persisted: a ready list of chat messages.
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
        if isinstance(inner, list) and not content.get("tool_calls"):
            # Backend-neutral ``{"role", "content": [parts]}`` envelope, whose
            # text parts are also the route's own multimodal shape.
            return [_message_from_neutral_parts(role, inner)]

        message = dict(content)
        message["role"] = role
        return [_clean_native_message(message)]


#: Field names a vLLM reasoning parser may use for separated thinking.
_REASONING_FIELDS = ("reasoning_content", "reasoning")


def _delta_reasoning(delta: Any) -> str:
    """The thinking carried by one delta, under either of its field names."""
    for field in _REASONING_FIELDS:
        if value := getattr(delta, field, None):
            return value
    return ""


class _StreamAccumulator:
    """Reconstructs the assistant message from streamed completion chunks.

    Each chunk carries a delta of `content`, of the model's thinking on a
    deployment started with a reasoning parser, and of `tool_calls`. One tool
    call spans several chunks: the first names the function and the arguments
    follow as JSON fragments, both keyed by the call's index within the choice.

    A call's id is minted here, from a ``prefix`` drawn once per turn and a
    counter continued across the rounds within that turn. The id has to be
    unique for the life of the session: the runner gives a call's nested action
    that id verbatim, and an action's node ids are ``"<id>#<port>"`` in the
    session's node map.
    """

    def __init__(self, prefix: str, base_id: int = 0):
        self._prefix = prefix
        self._base_id = base_id
        self._content = ""
        self._reasoning = ""
        self._pending: dict[int, dict[str, str]] = {}
        self._tool_calls: list[llm.ToolCall] = []
        self._response_id = ""
        self._model = ""
        self._finish_reason = ""
        self._usage: Any = None

    def add(self, chunk: Any) -> tuple[str, str]:
        """Fold one chunk in, and return its text and reasoning additions."""
        if response_id := getattr(chunk, "id", None):
            self._response_id = response_id
        if model := getattr(chunk, "model", None):
            self._model = model
        # Requested through `stream_options`, and carried by the final chunk.
        if (usage := getattr(chunk, "usage", None)) is not None:
            self._usage = usage

        text = ""
        reasoning = ""
        for choice in getattr(chunk, "choices", None) or []:
            if finish_reason := getattr(choice, "finish_reason", None):
                self._finish_reason = finish_reason
            delta = getattr(choice, "delta", None)
            if delta is None:
                continue
            text += getattr(delta, "content", None) or ""
            reasoning += _delta_reasoning(delta)
            for tool_call in getattr(delta, "tool_calls", None) or []:
                self._add_tool_call_delta(tool_call)

        self._content += text
        self._reasoning += reasoning
        return text, reasoning

    def _add_tool_call_delta(self, tool_call: Any) -> None:
        index = getattr(tool_call, "index", None)
        if index is None:
            index = len(self._pending)
        entry = self._pending.setdefault(index, {"name": "", "arguments": ""})
        function = getattr(tool_call, "function", None)
        if function is None:
            return
        if name := getattr(function, "name", None):
            entry["name"] = name
        if arguments := getattr(function, "arguments", None):
            entry["arguments"] += arguments

    async def finalize(self) -> list[llm.ToolCall]:
        """Decode the accumulated tool calls, in the order made."""
        self._tool_calls = []
        for offset, index in enumerate(sorted(self._pending)):
            entry = self._pending[index]
            params = await asyncio.to_thread(
                _decode_arguments, entry["name"], entry["arguments"]
            )
            self._tool_calls.append(
                llm.ToolCall(
                    name=entry["name"],
                    id=f"{self._prefix}_{self._base_id + offset}",
                    params=params,
                )
            )
        return self._tool_calls

    @property
    def content(self) -> str:
        return self._content

    @property
    def tool_calls(self) -> list[llm.ToolCall]:
        return self._tool_calls

    @property
    def model(self) -> str:
        return self._model

    @property
    def usage(self) -> Any:
        return self._usage

    def message_dict(self) -> dict[str, Any]:
        """The reconstructed assistant message, persisted as native content."""
        message: dict[str, Any] = {
            "role": "assistant",
            "content": self._content,
        }
        if self._reasoning:
            message["reasoning_content"] = self._reasoning
        if self._tool_calls:
            message["tool_calls"] = [
                {
                    "id": call.id,
                    "type": "function",
                    "function": {
                        "name": call.name,
                        "arguments": _encode_arguments(call.params),
                    },
                }
                for call in self._tool_calls
            ]
        return message

    def backend_specific_metadata(self) -> dict[str, bytes]:
        """Fields of the completion that don't map onto the shared models."""
        metadata: dict[str, bytes] = {
            llm.BACKEND_METADATA_KEY: str(llm.Backend.VLLM).encode()
        }
        for field, value in (
            ("response_id", self._response_id),
            ("finish_reason", self._finish_reason),
        ):
            if value:
                metadata[field] = llm.encode_backend_value(value)
        return metadata


async def _build_tool_results_from_outputs(
    executed: runner.ExecutedActions,
) -> list[dict[str, Any]]:
    """Turn each nested-action output or failure into a tool message.

    A ``role: "tool"`` message on this route carries text alone, so a call that
    wrote an `image/*` output is followed by a user message holding the frames
    as ``image_url`` parts.
    """

    def as_tool_message(
        call_id: str,
        content: str,
        failure: str | None,
        images: list[llm.NormalizedPart],
    ) -> list[dict[str, Any]]:
        text = content if failure is None else f"Error: {failure}"
        messages: list[dict[str, Any]] = [
            {
                "role": "tool",
                "tool_call_id": call_id,
                "content": text or _IMAGE_RESULT_NOTE,
            }
        ]
        if images:
            messages.append({
                "role": "user",
                "content": [
                    {"type": "text", "text": _IMAGE_RESULT_NOTE},
                    *(
                        _image_part(image.data or "", image.mime_type)
                        for image in images
                    ),
                ],
            })
        return messages

    return await llm.build_tool_results(executed, as_tool_message)


def _build_usage_metadata(usage: Any | None) -> llm.UsageMetadata | None:
    """Map the completion's token counts onto the shared usage metadata."""
    if usage is None:
        return None

    prompt_details = getattr(usage, "prompt_tokens_details", None)
    completion_details = getattr(usage, "completion_tokens_details", None)
    return llm.UsageMetadata(
        input_tokens=getattr(usage, "prompt_tokens", None),
        output_tokens=getattr(usage, "completion_tokens", None),
        total_tokens=getattr(usage, "total_tokens", None),
        cached_input_tokens=getattr(prompt_details, "cached_tokens", None),
        reasoning_tokens=getattr(completion_details, "reasoning_tokens", None),
    )


def _build_request_options(
    config: CreateChatCompletionConfig,
) -> dict[str, Any]:
    """The OpenAI-compatible request fields set by the config."""
    options: dict[str, Any] = {}
    if config.max_tokens >= 0:
        options["max_tokens"] = config.max_tokens
    for field in (
        "temperature",
        "top_p",
        "presence_penalty",
        "frequency_penalty",
        "seed",
    ):
        value = getattr(config, field)
        if value is not None:
            options[field] = value
    if config.stop:
        options["stop"] = list(config.stop)
    if config.json_schema is not None:
        options["response_format"] = {
            "type": "json_schema",
            "json_schema": {"name": "response", "schema": config.json_schema},
        }
    elif config.json_output:
        options["response_format"] = {"type": "json_object"}
    return options


def _build_extra_body(config: CreateChatCompletionConfig) -> dict[str, Any]:
    """vLLM's own request fields, which ride outside the OpenAI schema."""
    extra_body: dict[str, Any] = {}
    for field in ("top_k", "min_p", "repetition_penalty"):
        value = getattr(config, field)
        if value is not None:
            extra_body[field] = value
    if config.chat_template_kwargs:
        extra_body["chat_template_kwargs"] = dict(config.chat_template_kwargs)
    extra_body.update(config.extra_body)
    return extra_body


def _build_tools(
    requested_tools: list[dict[str, Any]],
) -> list[dict[str, Any]]:
    """Convert A11 tool definitions into OpenAI function tools.

    The mappings are sent as written, so an action whose input port takes an
    object reaches the model with that object's own properties and required
    fields intact.
    """
    return [
        {
            "type": "function",
            "function": {
                "name": tool["name"],
                "description": tool.get("description", ""),
                "parameters": tool.get("input_schema", {}),
            },
        }
        for tool in requested_tools
    ]


async def _resolve_model(client: Any, model: str) -> str:
    """The model to request: the one named, or the deployment's first served.

    A vLLM deployment serves the models it was started with, so an unset model
    header resolves against ``/v1/models``.
    """
    if model:
        return model

    try:
        listed = await client.models.list()
    except openai.APIConnectionError as exc:
        raise Status(
            code=StatusCode.UNAVAILABLE,
            message=(
                f"The vLLM deployment at {client.base_url} is unreachable:"
                f" {exc}"
            ),
        ).to_exception() from exc
    except openai.APIError as exc:
        raise Status(
            code=StatusCode.FAILED_PRECONDITION,
            message=(
                "No model was named, and the vLLM deployment at"
                f" {client.base_url} could not be asked for one: {exc}"
            ),
        ).to_exception() from exc

    for served in getattr(listed, "data", None) or []:
        if served_id := getattr(served, "id", ""):
            return served_id

    raise Status(
        code=StatusCode.FAILED_PRECONDITION,
        message=(
            f"The vLLM deployment at {client.base_url} serves no models; set"
            f" the {llm.LlmHeaders.MODEL.value} header."
        ),
    ).to_exception()


async def interact_with_vllm(action: a11.Action):
    deadline = a11.get_deadline(action)

    def remaining_timeout():
        return max(deadline - a11.now(), a11.zero_duration())

    # A self-hosted deployment runs without credentials unless it was started
    # with --api-key, so the key is optional.
    api_key = action.get_header(llm.LlmHeaders.API_KEY.value, decode=True)
    base_url = action.get_header(llm.LlmHeaders.BASE_URL.value, decode=True)

    model: str = (
        action.get_header(llm.LlmHeaders.MODEL.value, decode=True)
        or DEFAULT_MODEL
    )

    config = await action["config"].consume(
        CreateChatCompletionConfig, timeout=remaining_timeout(), allow_none=True
    )
    if config is None:
        config = CreateChatCompletionConfig()

    previous_interaction_id = ""
    conversation = Conversation()
    async for interaction in action["interactions"]:
        interaction = conversation.feed_next_interaction(interaction)
        previous_interaction_id = interaction.id

    client = get_vllm_client(base_url, api_key)
    model = await _resolve_model(client, model)

    # Record the model and input for tracing backends such as Langfuse.
    # Tracing failures do not fail the interaction.
    if action.trace_id:
        try:
            action.set_span_name("vLLM interaction")
            action.set_span_attribute("gen_ai.system", "vllm")
            action.set_span_attribute("gen_ai.request.model", model)
            action.set_span_input(conversation.messages)
        except Exception:
            logging.debug("failed to record LLM span input", exc_info=True)

    tools = _build_tools(await runner.collect_tools(action, deadline))
    options = _build_request_options(config)
    extra_body = _build_extra_body(config)

    # Tool-call ids span every round and turn in a session. The per-turn prefix
    # and per-round counter form the unique id.
    call_id_prefix = f"call_{uuid.uuid4().hex[:12]}"
    next_tool_call_id = 0
    try:
        failed_rounds = llm.FailedToolRounds()
        while True:
            messages: list[dict[str, Any]] = []
            if conversation.system_prompt:
                messages.append({
                    "role": "system",
                    "content": conversation.system_prompt,
                })
            messages.extend(conversation.messages)

            request: dict[str, Any] = {
                "model": model,
                "messages": messages,
                "stream": True,
                "stream_options": {"include_usage": True},
                **options,
            }
            if tools:
                request["tools"] = tools
                request["tool_choice"] = "auto"
            if extra_body:
                request["extra_body"] = extra_body

            try:
                stream = await client.chat.completions.create(**request)
            except openai.APIConnectionError as exc:
                raise Status(
                    code=StatusCode.UNAVAILABLE,
                    message=(
                        f"The vLLM deployment at {client.base_url} is"
                        f" unreachable: {exc}"
                    ),
                ).to_exception() from exc
            except openai.APIError as exc:
                raise Status(
                    code=StatusCode.INTERNAL, message=str(exc)
                ).to_exception() from exc

            accumulator = _StreamAccumulator(call_id_prefix, next_tool_call_id)

            async for chunk in stream:
                await action["event_stream"].put(chunk)
                text, reasoning = accumulator.add(chunk)
                if text:
                    await action["text_output"].put(text)
                if reasoning:
                    await action["thoughts"].put(reasoning)

            tool_calls = await accumulator.finalize()
            next_tool_call_id += len(tool_calls)
            message_dict = accumulator.message_dict()

            interaction = llm.Interaction(
                previous_interaction_id=previous_interaction_id,
                role=llm.Role.ASSISTANT,
                created_at_millis=a11.now().nanoseconds_since_epoch // 1000000,
                model=accumulator.model or model,
                content=[await asyncio.to_thread(a11.to_chunk, message_dict)],
                backend_specific_metadata=(
                    accumulator.backend_specific_metadata()
                ),
                usage_metadata=_build_usage_metadata(accumulator.usage),
            )
            previous_interaction_id = interaction.id
            rejected = await llm.add_tool_calls_to_interaction(
                tool_calls, interaction, action.get_registry()
            )

            interaction = conversation.feed_next_interaction(interaction)

            await action["new_interactions"].put(interaction)
            if not interaction.action_calls and not rejected:
                if action.trace_id:
                    try:
                        action.set_span_output(message_dict)
                    except Exception:
                        logging.debug(
                            "failed to record LLM span output", exc_info=True
                        )
                break

            executed = await runner.execute_actions_from_interaction(
                interaction, action, action.get_registry(), rejected=rejected
            )

            tool_output_interaction = llm.Interaction(
                previous_interaction_id=previous_interaction_id,
                role=llm.Role.USER,
                created_at_millis=a11.now().nanoseconds_since_epoch // 1000000,
                action_outputs=executed.outputs,
                backend_specific_metadata={
                    llm.BACKEND_METADATA_KEY: str(llm.Backend.VLLM).encode(),
                    # Tool narration remains in metadata. Provider content
                    # contains the declared results.
                    **executed.log_metadata(),
                },
                content=[
                    a11.to_chunk({
                        "messages": await _build_tool_results_from_outputs(
                            executed
                        )
                    })
                ],
            )
            previous_interaction_id = tool_output_interaction.id
            tool_output_interaction = conversation.feed_next_interaction(
                tool_output_interaction
            )

            await action["new_interactions"].put(tool_output_interaction)

            if not failed_rounds.record(executed):
                logging.warning(
                    "ending the conversation after %d rounds in which every"
                    " tool call failed",
                    failed_rounds.rounds,
                )
                break

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
