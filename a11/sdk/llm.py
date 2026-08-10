# Copyright 2026 The A11 Authors.

import abc
import base64
import enum
import re
import uuid
from typing import Any, Callable, ClassVar, Literal

import a11
from a11.data import serial_tags
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
        exclude_if=lambda x: x is None
        or (x.is_ok() and x.message in ("", "OK")),
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
# shape (Anthropic message blocks, Gemini interaction steps, …). When a
# conversation is handed from one backend to another mid-flight, the receiving
# backend cannot read the other's native content directly. To bridge them each
# interaction is tagged (in `backend_specific_metadata`) with the backend that
# produced it, and each backend contributes a normalizer that turns its own
# native content into the provider-independent `NormalizedMessage` below. A
# consumer that meets a foreign interaction calls `normalize_interaction`
# (which dispatches to the producer's normalizer by tag) and then translates
# the `NormalizedMessage` into its own native shape.


BACKEND_METADATA_KEY = "backend"

#: Output port carrying a tool's narration of its own run, written for the
#: person watching rather than for the model.
#:
#: An action that declares it is saying "this port is not part of the tool
#: contract": the LLM tool runner drains it, keeps it out of the tool result the
#: model is shown, and files it under the call id instead (see
#: [ExecutedActions][a11.sdk.llm_tools.runner.ExecutedActions]).
USER_FACING_LOG_PORT = "user_facing_log"

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
    GEMINI = "gemini"
    OLLAMA = "ollama"


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


def normalize_interaction(interaction: Interaction) -> NormalizedMessage:
    """Build the normalized view of a (foreign) interaction via its producer.

    Dispatches to the normalizer registered by the backend that produced the
    interaction. Callers should only reach for this when the interaction is
    tagged for a backend other than their own.
    """
    backend = interaction_backend(interaction)
    if backend is None:
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message="Cannot normalize an interaction with no backend tag.",
        ).to_exception()

    normalizer = _INTERACTION_NORMALIZERS.get(backend)
    if normalizer is None:
        raise Status(
            code=StatusCode.FAILED_PRECONDITION,
            message=(
                "No interaction normalizer is registered for backend"
                f" {backend!r}; its module must be imported to consume its"
                " interactions."
            ),
        ).to_exception()

    return normalizer(interaction)
