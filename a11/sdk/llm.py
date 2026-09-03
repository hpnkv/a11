# Copyright 2026 The A11 Authors.

import abc
import asyncio
import base64
import dataclasses
import enum
import functools
import logging
import re
import uuid
from typing import Any, Callable, ClassVar, Literal

import a11
import pydantic_core

from a11.data import serial_tags
from a11.data.serialization import get_global_serialization_registry
from a11.status import Status, StatusCode
from pydantic import (
    BaseModel,
    Field,
    model_validator,
    field_serializer,
)


class LlmHeaders(enum.StrEnum):
    API_KEY = "x-a11-llm-api-key"
    MODEL = "x-a11-llm-model"
    PROVIDER = "x-a11-llm-provider"
    BASE_URL = "x-a11-llm-base-url"
    ALLOWED_LLM_ACTIONS = "x-a11-allowed-llm-actions"


def get_allowed_llm_action_patterns(action: a11.Action) -> list[str]:
    """Regex patterns for the actions the LLM may invoke as tools.

    This is a tool-call-time restriction: it constrains which registered
    actions are surfaced to (and callable by) the model, not which nested
    actions the handler code may run.
    """
    try:
        header = action.get_header(
            LlmHeaders.ALLOWED_LLM_ACTIONS.value, decode=True
        )
    except Exception as exc:
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message=(
                f"The header {LlmHeaders.ALLOWED_LLM_ACTIONS.value} is not a"
                f" valid utf-8 string: {str(exc)}"
            ),
        ).to_exception() from exc

    if header is None:
        return []

    header: str
    return [pattern.strip() for pattern in header.split(",") if pattern.strip()]


def action_name_matches_allowed(name: str, patterns: list[str]) -> bool:
    """Whether an action name is fully matched by any allowed regex pattern."""
    for pattern in patterns:
        try:
            if re.fullmatch(pattern, name) is not None:
                return True
        except re.error as exc:
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message=(
                    f"Allowed LLM action pattern {pattern!r} is not a valid"
                    f" regular expression: {str(exc)}"
                ),
            ).to_exception() from exc
    return False


class Role(enum.Enum):
    SYSTEM = "system"
    ASSISTANT = "model"
    USER = "user"

    DEFAULT = "user"


class UsageMetadata(BaseModel):
    """Provider-independent token accounting for a single interaction.

    The fields are the common denominator across the major LLM provider APIs
    (Anthropic, Gemini, Ollama, OpenAI). Each field documents the per-provider
    source it maps from. Counters that are specific to one backend and do not
    generalize (e.g. Anthropic's `server_tool_use`, `service_tier`, or Ollama's
    timing durations) do not belong here — they go into an interaction's
    `backend_specific_metadata`.
    """

    A11_SERIAL_TAG: ClassVar[str] = serial_tags.USAGE_METADATA

    input_tokens: int | None = Field(
        default=None,
        description=(
            "Prompt/input tokens consumed. Anthropic `input_tokens`, OpenAI"
            " `prompt_tokens`, Gemini `promptTokenCount`, Ollama"
            " `prompt_eval_count`."
        ),
        exclude_if=lambda x: x is None,
    )
    output_tokens: int | None = Field(
        default=None,
        description=(
            "Completion/output tokens generated. Anthropic `output_tokens`,"
            " OpenAI `completion_tokens`, Gemini `candidatesTokenCount`, Ollama"
            " `eval_count`."
        ),
        exclude_if=lambda x: x is None,
    )
    total_tokens: int | None = Field(
        default=None,
        description=(
            "Total tokens attributed to the interaction. Provider-supplied"
            " where available (OpenAI `total_tokens`, Gemini"
            " `totalTokenCount`); otherwise the sum of the input, output, and"
            " cache token counts."
        ),
        exclude_if=lambda x: x is None,
    )
    cached_input_tokens: int | None = Field(
        default=None,
        description=(
            "Input tokens served from a prompt cache (billed at a reduced"
            " rate). Anthropic `cache_read_input_tokens`, OpenAI"
            " `prompt_tokens_details.cached_tokens`, Gemini"
            " `cachedContentTokenCount`."
        ),
        exclude_if=lambda x: x is None,
    )
    cache_write_tokens: int | None = Field(
        default=None,
        description=(
            "Input tokens written to a prompt cache. Currently only reported by"
            " Anthropic (`cache_creation_input_tokens`)."
        ),
        exclude_if=lambda x: x is None,
    )
    reasoning_tokens: int | None = Field(
        default=None,
        description=(
            "Output tokens spent on internal reasoning/thinking. Anthropic"
            " `output_tokens_details.thinking_tokens`, OpenAI"
            " `completion_tokens_details.reasoning_tokens`, Gemini"
            " `thoughtsTokenCount`."
        ),
        exclude_if=lambda x: x is None,
    )


GLOBAL_WEBRTC_SIGNALLING_ENDPOINT = "wss://a11.services/ice"


class A11Peer(BaseModel):
    A11_SERIAL_TAG: ClassVar[str] = serial_tags.PEER

    protocol: Literal["a11", "mcp"] = Field(
        default="a11",
        description="The protocol to use for the A11 peer.",
        exclude_if=lambda x: x == "a11",
    )
    scheme: Literal["session", "ws", "wss", "http", "https", "rtc"] = Field(
        default="session",
        description="The scheme to use for the A11 action party.",
        exclude_if=lambda x: not x,
    )
    identity: str = Field(
        default="",
        description=(
            "The peer's identity. For MCP, it is empty. For A11 session, it may"
            " be $sender, $receiver, or an ID of a stream that is attached to"
            " the session. For the `ws` scheme, it is empty. For `rtc`, it is"
            " the signalling identity."
        ),
        exclude_if=lambda x: not x,
    )
    endpoint: str = Field(
        default="",
        description="The endpoint to use for the peer.",
        exclude_if=lambda x: not x,
    )

    def __str__(self):
        identity_endpoint_parts = []
        if self.identity:
            identity_endpoint_parts.append(self.identity)
        if self.endpoint:
            identity_endpoint_parts.append(self.endpoint)

        if self.protocol == "a11":
            protocol_scheme = self.protocol
            if self.scheme != "session":
                protocol_scheme += "+" + self.scheme
        else:
            protocol_scheme = self.protocol

        identity_endpoint = "@".join(identity_endpoint_parts)
        return f"{protocol_scheme}://{identity_endpoint}"

    @staticmethod
    def from_string(peer: str):
        parts = peer.split("://", 1)
        if len(parts) != 2:
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message="Peer URL must include a scheme.",
            ).to_exception()

        protocol_scheme, identity_endpoint = parts
        protocol_scheme_parts = protocol_scheme.split("+")
        if len(protocol_scheme_parts) > 2:
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message=(
                    "Peer URL must include a single protocol and a single"
                    " scheme."
                ),
            ).to_exception()

        protocol = protocol_scheme_parts[0]
        if not protocol:
            if protocol_scheme_parts[1]:
                raise Status(
                    code=StatusCode.INVALID_ARGUMENT,
                    message="Cannot include a scheme without protocol.",
                ).to_exception()
            protocol = "a11"

        if protocol not in ("a11", "mcp"):
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message=(
                    f"Peer URL must include a valid protocol. Found: {protocol}"
                ),
            ).to_exception()

        scheme = ""
        if len(protocol_scheme_parts) == 2:
            scheme = protocol_scheme_parts[1]

        if not scheme:
            match protocol:
                case "a11":
                    scheme = "session"
                case "mcp":
                    scheme = "http"
                case _:
                    pass

        identity_endpoint_parts = identity_endpoint.split("@", 1)
        if protocol == "a11":
            identity = identity_endpoint_parts[0]
            endpoint = ""
        else:
            identity = ""
            endpoint = identity_endpoint_parts[0]

        if len(identity_endpoint_parts) == 2:
            identity = identity_endpoint_parts[0]
            endpoint = identity_endpoint_parts[1]

        if not endpoint:
            if protocol == "a11" and scheme == "rtc":
                endpoint = GLOBAL_WEBRTC_SIGNALLING_ENDPOINT

        return A11Peer(
            protocol=protocol,
            scheme=scheme,
            identity=identity,
            endpoint=endpoint,
        )

    @model_validator(mode="after")
    def validate(self):
        if self.protocol == "mcp" and self.identity:
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message="MCP peer identity must be empty.",
            ).to_exception()

        if self.protocol == "mcp" and self.scheme not in ("http", "https"):
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message="MCP peer scheme must be http or https.",
            ).to_exception()

        if self.protocol == "a11" and self.scheme == "session":
            if self.endpoint:
                raise Status(
                    code=StatusCode.INVALID_ARGUMENT,
                    message=(
                        "A11 peer endpoint must be empty for `session` scheme."
                    ),
                ).to_exception()

            if not self.identity:
                self.identity = "$sender"

            if self.identity.startswith("$") and self.identity not in (
                "$sender",
                "$receiver",
            ):
                raise Status(
                    code=StatusCode.INVALID_ARGUMENT,
                    message=(
                        "A11 peer identity must be `$sender`, `$receiver` or a"
                        " stream ID."
                    ),
                ).to_exception()

        return self


class A11ActionConfig(BaseModel):
    A11_SERIAL_TAG: ClassVar[str] = serial_tags.ACTION_CONFIG

    peer: str | A11Peer = Field(
        default="a11://$sender",
        description="Who should run the action.",
        exclude_if=lambda x: str(x) == "a11://$sender",
    )

    header_autofills: dict[str, bytes] = Field(
        default_factory=dict,
        description="A map of header names to autofill values.",
        exclude_if=lambda x: not x,
    )

    @field_serializer("peer")
    def serialize_peer(self, value: str | A11Peer):
        return str(value)

    @field_serializer("header_autofills")
    def serialize_header_autofills(self, value: dict[str, bytes]):
        return {k: base64.b64encode(v).decode() for k, v in value.items()}

    @model_validator(mode="after")
    def validate_peer(self):
        if isinstance(self.peer, str):
            self.peer = A11Peer.from_string(self.peer)

        return self


class Interaction(BaseModel):
    A11_SERIAL_TAG: ClassVar[str] = serial_tags.INTERACTION

    id: str = Field(
        default_factory=lambda: str(uuid.uuid4()),
        description="The completion ID of this interaction",
        exclude_if=lambda x: not x,
    )
    role: Role = Field(
        default=Role.DEFAULT,
        description="The role of the interaction.",
    )
    created_at_millis: int | None = Field(
        default=None,
        description="The millisecond-since-epoch of the interaction.",
        exclude_if=lambda x: x is None,
    )
    previous_interaction_id: str = Field(
        default="",
        description="The ID of the previous interaction.",
        exclude_if=lambda x: not x,
    )

    model: str = Field(
        default="",
        description="The model that produced the interaction.",
        exclude_if=lambda x: not x,
    )

    status: Status | None = Field(
        default=Status.ok(),
        description="The status of the interaction.",
        exclude_if=lambda x: (
            x is None or (x.is_ok() and x.message in ("", "OK"))
        ),
    )

    system_instructions: list[a11.Chunk] = Field(
        default_factory=list,
        description="The system instructions of the interaction.",
        exclude_if=lambda x: not x,
    )
    action_configs: dict[str, A11ActionConfig] = Field(
        default_factory=dict,
        description="The action configs of the interaction.",
        exclude_if=lambda x: not x,
    )
    content: list[a11.Chunk] = Field(
        default_factory=list,
        description="The content of the interaction.",
        exclude_if=lambda x: not x,
    )

    action_calls: list[a11.ActionMessage] = Field(
        default_factory=list,
        description="The action calls of the interaction.",
        exclude_if=lambda x: not x,
    )
    action_inputs: dict[str, list[a11.NodeFragment]] = Field(
        default_factory=dict,
        description="The inputs of the interaction.",
        exclude_if=lambda x: not x,
    )
    action_outputs: dict[str, list[a11.NodeFragment]] = Field(
        default_factory=dict,
        description="The outputs of the interaction.",
        exclude_if=lambda x: not x,
    )

    backend_specific_metadata: dict[str, bytes] = Field(
        default_factory=dict,
        description=(
            "The backend-specific metadata of the interaction. For example,"
            " Anthropic messages can have `container`, `stop_reason`, etc."
        ),
        exclude_if=lambda x: not x,
    )

    usage_metadata: UsageMetadata | None = Field(
        default=None,
        description="The usage metadata of the interaction.",
        exclude_if=lambda x: x is None,
    )

    @field_serializer("content")
    def serialize_content(self, content: list[a11.Chunk]):
        return [chunk.model_dump() for chunk in content]

    @model_validator(mode="after")
    def sticky_mimetype(self):
        current_mimetype: str = ""
        chunk: a11.Chunk
        for chunk in self.content:
            if chunk.get_mimetype() == current_mimetype:
                if chunk.metadata:
                    chunk.metadata.mimetype = ""
                if (
                    not chunk.metadata.timestamp
                    and not chunk.metadata.attributes
                ):
                    chunk.metadata = None
            else:
                current_mimetype = chunk.get_mimetype()

        return self


class InteractionAdapter(metaclass=abc.ABCMeta):
    @abc.abstractmethod
    def __init__(self, _interaction: Interaction): ...

    @staticmethod
    @abc.abstractmethod
    def make_text_message_interaction(
        text: str, system_prompt: str = "", role: Role = Role.USER
    ) -> Interaction: ...

    @abc.abstractmethod
    def get_message_text(self) -> str: ...


# --- Cross-backend interaction normalization ---------------------------------
#
# Different LLM backends persist an interaction's `content` in their own native
# shape (Anthropic message blocks, Gemini interaction steps, ...). When a
# conversation is handed from one backend to another mid-flight, the receiving
# backend cannot read the other's native content directly. To bridge them each
# interaction is tagged (in `backend_specific_metadata`) with the backend that
# produced it, and each backend contributes a normalizer that turns its own
# native content into the provider-independent `NormalizedMessage` below. A
# consumer that meets a foreign interaction calls `normalize_interaction` (which
# dispatches to the producer's normalizer by tag) and then translates the
# `NormalizedMessage` into its own native shape.


BACKEND_METADATA_KEY = "backend"

#: Where a turn's user-facing tool logs ride, in the
#: ``backend_specific_metadata`` of the interaction carrying that turn's tool
#: results: JSON bytes of ``{tool call id: log}``.
#:
#: They live there rather than on a port because that is the one part of an
#: interaction no backend turns into provider content -- the log must never
#: reach the model, but a conversation replayed from storage is poorer for it.
TOOL_LOGS_METADATA_KEY = "tool_logs"


class Backend(enum.StrEnum):
    CLAUDE = "claude"
    CLAUDE_CODE = "claude_code"
    GEMINI = "gemini"
    OLLAMA = "ollama"
    VLLM = "vllm"


class NormalizedContentType(enum.StrEnum):
    TEXT = "text"
    IMAGE = "image"
    TOOL_CALL = "tool_call"
    TOOL_RESULT = "tool_result"


class NormalizedPart(BaseModel):
    """A single, backend-independent piece of an interaction's content."""

    type: NormalizedContentType
    # TEXT
    text: str | None = Field(default=None, exclude_if=lambda x: x is None)
    # IMAGE (inline, base64-encoded bytes)
    data: str | None = Field(default=None, exclude_if=lambda x: x is None)
    mime_type: str | None = Field(default=None, exclude_if=lambda x: x is None)
    # TOOL_CALL
    id: str | None = Field(default=None, exclude_if=lambda x: x is None)
    name: str | None = Field(default=None, exclude_if=lambda x: x is None)
    arguments: dict[str, Any] | None = Field(
        default=None, exclude_if=lambda x: x is None
    )
    # TOOL_RESULT
    call_id: str | None = Field(default=None, exclude_if=lambda x: x is None)
    content: str | None = Field(default=None, exclude_if=lambda x: x is None)


class NormalizedMessage(BaseModel):
    """Backend-independent view of one interaction's content."""

    role: Role = Role.USER
    parts: list[NormalizedPart] = Field(default_factory=list)


_INTERACTION_NORMALIZERS: dict[
    str, Callable[[Interaction], NormalizedMessage]
] = {}


def register_interaction_normalizer(
    backend: str, normalizer: Callable[[Interaction], NormalizedMessage]
) -> None:
    """Register a backend's native-content -> `NormalizedMessage` producer."""
    _INTERACTION_NORMALIZERS[str(backend)] = normalizer


def interaction_backend(interaction: Interaction) -> str | None:
    """The backend that produced `interaction`, or None if untagged."""
    value = interaction.backend_specific_metadata.get(BACKEND_METADATA_KEY)
    if isinstance(value, bytes):
        value = value.decode()
    return value or None


def _shape_text(value: Any) -> str:
    """Best-effort text of one decoded content chunk, read by shape.

    Every backend wraps its provider payload in here, so this reads the shapes
    rather than the backend: a bare string, ``{"text": ...}``, or the
    ``{"role": ..., "content": [{"type": "text", "text": ...}]}`` envelope that
    the clients and the Claude/Gemini backends all produce.
    """
    if isinstance(value, str):
        return value
    if not isinstance(value, dict):
        return ""
    blocks = value.get("content")
    if isinstance(blocks, str):
        return blocks
    if isinstance(blocks, list):
        return "".join(
            block["text"]
            for block in blocks
            if isinstance(block, dict) and isinstance(block.get("text"), str)
        )
    text = value.get("text")
    return text if isinstance(text, str) else ""


def _shape_parts(value: Any) -> list[NormalizedPart]:
    """Portable text and image parts found in one decoded content value."""
    if isinstance(value, str):
        return [NormalizedPart(type=NormalizedContentType.TEXT, text=value)]
    if isinstance(value, list):
        blocks = value
    elif isinstance(value, dict):
        steps = value.get("steps")
        if isinstance(steps, list):
            return [
                part
                for step in steps
                if isinstance(step, dict)
                for part in _shape_parts(step)
            ]
        blocks = value.get("content")
        if not isinstance(blocks, list):
            text = _shape_text(value)
            parts = (
                [NormalizedPart(type=NormalizedContentType.TEXT, text=text)]
                if text
                else []
            )
            for image in value.get("images") or []:
                if isinstance(image, str):
                    parts.append(
                        NormalizedPart(
                            type=NormalizedContentType.IMAGE, data=image
                        )
                    )
            return parts
    else:
        return []

    parts: list[NormalizedPart] = []
    for block in blocks:
        if isinstance(block, str):
            parts.append(
                NormalizedPart(type=NormalizedContentType.TEXT, text=block)
            )
            continue
        if not isinstance(block, dict):
            continue
        block_type = block.get("type")
        if block_type == "text" and isinstance(block.get("text"), str):
            parts.append(
                NormalizedPart(
                    type=NormalizedContentType.TEXT, text=block["text"]
                )
            )
        elif block_type == "image" and isinstance(block.get("data"), str):
            parts.append(
                NormalizedPart(
                    type=NormalizedContentType.IMAGE,
                    data=block["data"],
                    mime_type=block.get("mime_type"),
                )
            )
        elif block_type == "image":
            source = block.get("source")
            if isinstance(source, dict) and isinstance(source.get("data"), str):
                parts.append(
                    NormalizedPart(
                        type=NormalizedContentType.IMAGE,
                        data=source["data"],
                        mime_type=source.get("media_type"),
                    )
                )
        elif block_type == "image_url":
            image_url = block.get("image_url")
            url = image_url.get("url") if isinstance(image_url, dict) else None
            if isinstance(url, str) and url.startswith("data:"):
                header, separator, data = url.partition(",")
                if separator and ";base64" in header:
                    parts.append(
                        NormalizedPart(
                            type=NormalizedContentType.IMAGE,
                            data=data,
                            mime_type=header[5:].split(";", 1)[0] or None,
                        )
                    )
    return parts


def normalize_by_shape(interaction: Interaction) -> NormalizedMessage:
    """Normalize an interaction whose producer is unknown, by reading shapes.

    The backend-tagged normalizers are the principled route when a conversation
    is handed between providers, because only the producer can read its own
    native content faithfully. But an interaction minted by a *client* carries
    no backend tag at all, and every consumer that merely wants to display a
    conversation needs to cope with that. This does the shape-based read those
    consumers were each hand-rolling, and adds the parts that live outside
    ``content`` and so need no backend knowledge: the tool calls in
    `Interaction.action_calls` and the results in `Interaction.action_outputs`.
    """
    parts: list[NormalizedPart] = []
    current_mimetype = ""
    for chunk in interaction.content:
        mimetype = chunk.get_mimetype() or current_mimetype
        if chunk.get_mimetype():
            current_mimetype = chunk.get_mimetype()
        media_type = mimetype.split(";", 1)[0].strip().lower()
        if media_type.startswith("image/"):
            parts.append(
                NormalizedPart(
                    type=NormalizedContentType.IMAGE,
                    data=base64.b64encode(chunk.data).decode("ascii"),
                    mime_type=media_type,
                )
            )
            continue
        try:
            selector = "" if chunk.get_mimetype() else mimetype
            decoded = get_global_serialization_registry().from_chunk(
                chunk, selector
            )
        except Exception:  # noqa: BLE001 - an undecodable chunk is not fatal
            logging.debug("undecodable content chunk", exc_info=True)
            continue
        parts.extend(_shape_parts(decoded))
    for call in interaction.action_calls:
        parts.append(
            NormalizedPart(
                type=NormalizedContentType.TOOL_CALL,
                id=call.id,
                name=call.name,
            )
        )
    for call_id in interaction.action_outputs:
        parts.append(
            NormalizedPart(
                type=NormalizedContentType.TOOL_RESULT, call_id=call_id
            )
        )
    return NormalizedMessage(role=interaction.role, parts=parts)


def has_image_chunks(interaction: Interaction) -> bool:
    """Whether `interaction.content` includes a raw image chunk."""
    current_mimetype = ""
    for chunk in interaction.content:
        if chunk.get_mimetype():
            current_mimetype = chunk.get_mimetype()
        media_type = current_mimetype.split(";", 1)[0].strip().lower()
        if media_type.startswith("image/"):
            return True
    return False


def normalize_interaction(
    interaction: Interaction, *, strict: bool = False
) -> NormalizedMessage:
    """Build the normalized view of an interaction.

    Dispatches to the normalizer registered by the backend that produced the
    interaction. When the interaction carries no backend tag, or its producer's
    normalizer is not registered in this process, falls back to
    `normalize_by_shape` -- unless ``strict``.

    Args:
        interaction: The interaction to normalize.
        strict: Require a registered normalizer for the producing backend.
            Use this on the backend-to-backend handoff path, where a
            shape-based approximation would silently drop native content the
            receiving backend needed. Display paths want the default.

    Returns:
        The normalized view.

    Raises:
        StatusException: When ``strict`` and the interaction is untagged or its
            backend has no registered normalizer.
    """
    backend = interaction_backend(interaction)
    if backend is None:
        if strict:
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message="Cannot normalize an interaction with no backend tag.",
            ).to_exception()
        return normalize_by_shape(interaction)

    normalizer = _INTERACTION_NORMALIZERS.get(backend)
    if normalizer is None:
        if strict:
            raise Status(
                code=StatusCode.FAILED_PRECONDITION,
                message=(
                    "No interaction normalizer is registered for backend"
                    f" {backend!r}; its module must be imported to consume its"
                    " interactions."
                ),
            ).to_exception()
        return normalize_by_shape(interaction)

    return normalizer(interaction)


# --- The action-call bridge --------------------------------------------------
#
# Everything below is backend-independent: it turns a tool call a model asked
# for into A11 action inputs, and action outputs back into text a model can
# read. Each SDK client reconstructs a `ToolCall` from its own streaming shape
# and then shares all of this.


#: Valid wire id for an output mapped to ``ActionSchema.WHOLE_JSON``. The
#: schema sentinel ``$`` is not a valid A11 fragment name.
WHOLE_JSON_FRAGMENT_ID = "_"


@dataclasses.dataclass
class ToolCall:
    """A tool call a model asked for, however its SDK spelled it."""

    name: str
    id: str
    params: dict[str, Any] = dataclasses.field(default_factory=dict)


def stringify_content(content: Any) -> str:
    """Flatten a tool-result payload into plain text."""
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


def encode_backend_value(value: Any) -> bytes:
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


def decode_action_output_fragments(fragments: list[a11.NodeFragment]) -> Any:
    """Decode one action's output fragments into a value per output port.

    A single port mapped to the whole JSON result is unwrapped rather than
    reported as a one-key mapping. Its fragment uses
    [WHOLE_JSON_FRAGMENT_ID][a11.sdk.llm.WHOLE_JSON_FRAGMENT_ID] because the
    schema sentinel ``$`` is not a valid fragment id.
    """
    grouped: dict[str, list[a11.NodeFragment]] = {}
    for fragment in fragments:
        grouped.setdefault(fragment.id, []).append(fragment)

    values: dict[str, Any] = {}
    for field_name, field_fragments in grouped.items():
        # Null chunks close a stream and do not contribute a result value.
        chunks = [fragment.get_chunk() for fragment in field_fragments]
        decoded = [
            a11.from_chunk(chunk) for chunk in chunks if not chunk.is_null()
        ]
        if not decoded:
            continue
        values[field_name] = decoded[0] if len(decoded) == 1 else decoded

    if list(values.keys()) == [WHOLE_JSON_FRAGMENT_ID]:
        return values[WHOLE_JSON_FRAGMENT_ID]
    return values


async def decoded_output_text(fragments: list[a11.NodeFragment]) -> str:
    """One action's outputs as text, JSON-encoding anything that is not.

    An output port carries whatever its media type says: a packed grid, a
    msgpack model holding raw frames, a signature. UTF-8 encoding refuses those
    bytes, so they go out as ``bytes_mode="base64"`` writes them -- base64url,
    unpadded -- and a model calling the action reads a result rather than a
    failed call.
    """
    content = decode_action_output_fragments(fragments)
    if isinstance(content, str):
        return content
    encoded = await asyncio.to_thread(
        functools.partial(pydantic_core.to_json, bytes_mode="base64"), content
    )
    return encoded.decode()


def split_output_images(
    fragments: list[a11.NodeFragment],
) -> tuple[list[a11.NodeFragment], list[NormalizedPart]]:
    """The fragments that carry no encoded image, and the images among them.

    An output port declaring an `image/*` media type carries the encoding
    itself, which is not JSON and not UTF-8. Splitting those fragments out
    leaves the remaining ports to
    [decode_action_output_fragments][a11.sdk.llm.decode_action_output_fragments]
    and hands each frame to a backend as a `NormalizedContentType.IMAGE` part.

    A mimetype is sticky per port, so only a stream's first chunk names one.

    Returns:
        The non-image fragments in order, and one part per image chunk.
    """
    rest: list[a11.NodeFragment] = []
    images: list[NormalizedPart] = []
    sticky: dict[str, str] = {}
    for fragment in fragments:
        chunk = fragment.get_chunk()
        if chunk.get_mimetype():
            sticky[fragment.id] = chunk.get_mimetype()
        declared = sticky.get(fragment.id, "")
        media_type = declared.split(";", 1)[0].strip().lower()
        if not media_type.startswith("image/"):
            rest.append(fragment)
            continue
        if chunk.is_null():
            continue
        images.append(
            NormalizedPart(
                type=NormalizedContentType.IMAGE,
                data=base64.b64encode(chunk.data).decode("ascii"),
                mime_type=media_type,
            )
        )
    return rest, images


async def decoded_output_content(
    fragments: list[a11.NodeFragment],
) -> tuple[str, list[NormalizedPart]]:
    """One action's outputs as text, and the encoded images among them.

    An action with no image output answers as
    [decoded_output_text][a11.sdk.llm.decoded_output_text] does. An action
    whose only output is an image answers with empty text, so a result carries
    the frame alone rather than an empty JSON object beside it.
    """
    rest, images = split_output_images(fragments)
    if not images:
        return await decoded_output_text(fragments), []
    return (await decoded_output_text(rest) if rest else ""), images


class ActionCallAdapter:
    """One model tool call, validated against the action schema it names."""

    def __init__(self, tool_call: ToolCall, schema: a11.ActionSchema):
        self._name = tool_call.name
        self._call_id = tool_call.id
        self._arguments = tool_call.params
        self._schema = schema

    @property
    def action_message(self) -> a11.ActionMessage:
        return a11.Action(self._schema, self._call_id).get_action_message()

    async def get_action_inputs(self) -> list[a11.NodeFragment]:
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
    def _validate_tool_call_integrity(tool_call: ToolCall) -> ToolCall:
        if not isinstance(tool_call.name, str):
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message="Tool call name must be a string.",
            ).to_exception()

        if not isinstance(tool_call.id, str):
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message="Tool call id must be a string.",
            ).to_exception()

        if not isinstance(tool_call.params, dict):
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message="Tool call params must be a dictionary.",
            ).to_exception()

        for key in tool_call.params.keys():
            if not isinstance(key, str):
                raise Status(
                    code=StatusCode.INVALID_ARGUMENT,
                    message="Tool call parameter names must be strings.",
                ).to_exception()

        return tool_call

    @staticmethod
    def validate_against_schema(
        tool_call: ToolCall,
        schema: a11.ActionSchema,
        validate_integrity: bool = True,
    ) -> ToolCall:
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
            # An autofilled input is absent from the tool definition. The loop
            # below rejects one supplied by the caller.
            if (
                expected_input.required
                and not expected_input.autofills
                and expected_input_name not in tool_call.params
            ):
                raise Status(
                    code=StatusCode.INVALID_ARGUMENT,
                    message=(
                        f"Tool call is missing input {expected_input_name}."
                    ),
                ).to_exception()

        for expected_input_name, expected_input in schema.inputs.items():
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
    def create(tool_call: ToolCall, schema: a11.ActionSchema):
        tool_call = ActionCallAdapter._validate_tool_call_integrity(tool_call)
        tool_call = ActionCallAdapter.validate_against_schema(
            tool_call, schema, validate_integrity=False
        )

        return ActionCallAdapter(tool_call, schema)


async def add_tool_calls_to_interaction(
    tool_calls: list[ToolCall],
    interaction: Interaction,
    registry: a11.ActionRegistry,
) -> None:
    """Record each tool call, and its encoded inputs, on the interaction."""
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


async def build_tool_results(
    executed: Any,
    format_result: Callable[
        [str, str, str | None, list[NormalizedPart]],
        dict[str, Any] | list[dict[str, Any]],
    ],
) -> list[dict[str, Any]]:
    """One or more provider-shaped result messages per executed action call.

    Decoding an action's outputs, and reporting a failure to the model
    alongside the calls that succeeded, are the same job for every backend; only
    the shape of the resulting message differs. ``format_result`` receives the
    call id, the content as text, the failure message when there was one, and
    the encoded images the action wrote on its `image/*` output ports.

    A provider whose tool-result shape holds an image block puts the frames
    there. One whose shape is text-only returns a list: the tool result, then a
    user message carrying the frames.

    Args:
        executed: The `llm_tools.runner.ExecutedActions` to report on. Untyped
            here because `runner` imports this module.
        format_result: Builds one provider-shaped message, or several.

    Returns:
        The messages, in the order the calls were run.
    """
    results: list[dict[str, Any]] = []
    for call_id, fragments in executed.outputs.items():
        failure = executed.error_message(call_id)
        images: list[NormalizedPart] = []
        if failure is not None:
            content = failure
        else:
            content, images = await decoded_output_content(fragments)
        formatted = format_result(call_id, content, failure, images)
        results.extend(
            formatted if isinstance(formatted, list) else [formatted]
        )
    return results
