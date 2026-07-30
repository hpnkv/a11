# Copyright 2026 The A11 Authors.

import base64
import enum
import re
import uuid
from typing import Literal

import a11
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
    pass


GLOBAL_WEBRTC_SIGNALLING_ENDPOINT = "wss://a11.services/ice"


class A11Peer(BaseModel):
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

    message_metadata: dict[str, bytes] = Field(
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
        print(content)
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
