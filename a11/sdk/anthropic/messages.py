# Copyright 2026 The A11 Authors.

"""Translate between A11 interactions and Anthropic-shaped messages.

The Anthropic API and the Claude Code CLI both speak
``{"role": ..., "content": [block, ...]}`` with the same block types, so both
providers in this package share this module. It depends on no provider SDK: a
pydantic message object is recognised by its ``model_dump``, and everything
else is a plain ``dict`` or ``str``.
"""

from __future__ import annotations

from typing import Any, Iterable

import a11

from a11.sdk import llm
from a11.status import Status, StatusCode


def _as_message_dict(content: Any) -> Any:
    """A provider message object as a dict, or ``content`` unchanged."""
    dump = getattr(content, "model_dump", None)
    if callable(dump):
        return dump()
    return content


def _normalized_image(block: dict[str, Any]) -> llm.NormalizedPart | None:
    """An Anthropic or MCP image block as one normalized image."""
    source = block.get("source") or {}
    data = source.get("data") or block.get("data")
    if not isinstance(data, str):
        return None
    return llm.NormalizedPart(
        type=llm.NormalizedContentType.IMAGE,
        data=data,
        mime_type=(
            source.get("media_type")
            or block.get("mime_type")
            or block.get("mimeType")
        ),
    )


def _normalized_tool_result(block: dict[str, Any]) -> list[llm.NormalizedPart]:
    """A tool result followed by the images carried inside its content."""
    content = block.get("content")
    images: list[llm.NormalizedPart] = []
    if isinstance(content, list):
        images = [
            image
            for item in content
            if isinstance(item, dict) and item.get("type") == "image"
            if (image := _normalized_image(item)) is not None
        ]
    text = (
        "".join(
            item.get("text", "")
            for item in content
            if isinstance(item, dict) and item.get("type") == "text"
        )
        if images and isinstance(content, list)
        else llm.stringify_content(content)
    )
    return [
        llm.NormalizedPart(
            type=llm.NormalizedContentType.TOOL_RESULT,
            call_id=block.get("tool_use_id"),
            content=text,
        ),
        *images,
    ]


def to_normalized(interaction: llm.Interaction) -> llm.NormalizedMessage:
    """Produce the normalized view of an Anthropic-shaped interaction."""
    content = _as_message_dict(a11.from_chunk(interaction.content[0]))

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
            parts.extend(_normalized_tool_result(block))
        elif block_type == "image":
            image = _normalized_image(block)
            if image is not None:
                parts.append(image)
        # `thinking`, `redacted_thinking`, and server tool blocks carry no
        # portable content and are intentionally dropped.

    return llm.NormalizedMessage(role=role, parts=parts)


def from_normalized(message: llm.NormalizedMessage) -> dict[str, Any]:
    """Translate a normalized message into an Anthropic-shaped message dict."""
    role = "assistant" if message.role == llm.Role.ASSISTANT else "user"
    blocks: list[dict[str, Any]] = []
    for part in message.parts:
        if part.type == llm.NormalizedContentType.TEXT:
            blocks.append({"type": "text", "text": part.text or ""})
        elif part.type == llm.NormalizedContentType.IMAGE:
            blocks.append({
                "type": "image",
                "source": {
                    "type": "base64",
                    "media_type": (
                        part.mime_type or "application/octet-stream"
                    ),
                    "data": part.data or "",
                },
            })
        elif part.type == llm.NormalizedContentType.TOOL_CALL:
            blocks.append({
                "type": "tool_use",
                "id": part.id or "",
                "name": part.name or "",
                "input": part.arguments or {},
            })
        elif part.type == llm.NormalizedContentType.TOOL_RESULT:
            blocks.append({
                "type": "tool_result",
                "tool_use_id": part.call_id or "",
                "content": part.content or "",
            })
    return {"role": role, "content": blocks}


class Conversation:
    """The turn's interactions as Anthropic-shaped messages.

    Args:
        native_backends: Backends whose interactions already carry this message
            shape. An interaction tagged with any other backend is bridged
            through the normalized representation; an untagged one is treated
            as native.
    """

    _interactions: list[llm.Interaction]
    _messages: list[dict[str, Any]]
    _system_instructions: list[str]
    _native_backends: frozenset[str]

    def __init__(self, native_backends: Iterable[str] = (llm.Backend.CLAUDE,)):
        self._interactions = []
        self._messages = []
        self._system_instructions = []
        self._native_backends = frozenset(native_backends)

    @property
    def last_interaction_id(self):
        if not self._interactions:
            return None
        return self._interactions[-1].id

    @property
    def interactions(self) -> list[llm.Interaction]:
        return self._interactions

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

        backend = llm.interaction_backend(interaction)
        chunk_content = len(interaction.content) != 1 or llm.has_image_chunks(
            interaction
        )
        if chunk_content:
            message = from_normalized(llm.normalize_by_shape(interaction))
        elif backend is not None and backend not in self._native_backends:
            # Produced by another backend: bridge it through the normalised
            # representation and leave the interaction's own content untouched.
            message = from_normalized(llm.normalize_interaction(interaction))
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
        content = _as_message_dict(a11.from_chunk(interaction.content[0]))
        if isinstance(content, dict):
            role = content.get("role")
            content_content = content.get("content")
            if not role or not content_content:
                raise Status(
                    code=StatusCode.INVALID_ARGUMENT,
                    message="Interaction content and role are required.",
                ).to_exception()
            message = {"role": role, "content": content_content}
        elif isinstance(content, str):
            message = {"role": llm.Role.USER.value, "content": content}
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


__all__ = ["Conversation", "from_normalized", "to_normalized"]
