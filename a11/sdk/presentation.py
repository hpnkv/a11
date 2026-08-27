# Copyright 2026 The A11 Authors.

"""Turn interactions into renderer-independent presentation units.

This module determines which text an interaction contributes, which tool ran,
where its log belongs, and which tool-result interactions should not be drawn.

The unit is a `PresentationBlock`: a flat, ordered, renderer-agnostic piece of a
turn. A client's job shrinks to a `match` over `BlockKind`, which is what makes
a terminal, a webview and a future web UI feel like the same product without
sharing a line of rendering code.

**One reducer, two feeders.** A live turn arrives as port events (text deltas,
thought deltas, whole interactions); a reopened conversation arrives as stored
`Interaction`s. Both go through `PresentationReducer`, so a client draws
replayed history with the same code that draws a live turn.

**Ordering.** The live feeder sees true interleaving of text, thoughts and tool
runs, because it is watching them happen. A stored `Interaction` has no
timeline, so replay is a stable approximation: text, then tool runs in
``action_calls``
order, then usage. Live order is authoritative; replay is deterministic but not
necessarily match the original live order. Exact replay would require per-part
timestamps on the wire.

These types carry no serial tag on purpose: tags are for values that cross the
wire ([a11.data.serial_tags][a11.data.serial_tags]), and this derivation runs
client-side. The cross-language contract is the golden fixture
``testdata/presentation_events.json``, which pins the field names should a
future gateway ever want to stream blocks directly.
"""

from __future__ import annotations

import enum
import json
from collections.abc import Iterable, Mapping, Sequence
from typing import Any, Protocol

from pydantic import BaseModel, Field

from a11.sdk.llm import (
    TOOL_LOGS_METADATA_KEY,
    Interaction,
    NormalizedContentType,
    Role,
    UsageMetadata,
    normalize_interaction,
)
from a11.status import Status


class BlockKind(enum.StrEnum):
    """What a block is, and therefore how a client should draw it."""

    #: Assistant or user prose. The body is `PresentationBlock.text`.
    TEXT = "text"
    #: Reasoning the model exposed. Clients commonly fold this away.
    THOUGHT = "thought"
    #: Inline image content.
    IMAGE = "image"
    #: A tool call. `PresentationBlock.text` is the tool's own user-facing log,
    #: which is written for the person watching rather than for the model.
    TOOL_RUN = "tool_run"
    #: A tool result that a client may want to show separately from its run.
    TOOL_RESULT = "tool_result"
    #: A failure, carrying `PresentationBlock.status`.
    ERROR = "error"
    #: Token accounting for a turn.
    USAGE = "usage"


class PresentationBlock(BaseModel):
    """One renderable piece of a turn."""

    kind: BlockKind
    #: Tool call id, for `BlockKind.TOOL_RUN` and `BlockKind.TOOL_RESULT`; the
    #: two are matched on it.
    id: str = ""
    #: The body. For a tool run this is its user-facing log, not its result.
    text: str = ""
    tool_name: str = ""
    tool_arguments: dict[str, Any] | None = None
    #: Set on `BlockKind.ERROR`, and on a tool run that failed.
    status: Status | None = None
    #: `BlockKind.IMAGE` only.
    mime_type: str = ""
    data: bytes | None = None
    usage: UsageMetadata | None = None
    #: Still being appended to. Only ever true on the live path, and a client
    #: may use it to draw a cursor or withhold expensive formatting.
    partial: bool = False
    #: Which interaction this came from, for provenance and debugging.
    interaction_id: str = ""
    role: Role = Role.ASSISTANT


class PresentationTurn(BaseModel):
    """The blocks one conversational turn contributes."""

    role: Role = Role.ASSISTANT
    interaction_ids: list[str] = Field(default_factory=list)
    blocks: list[PresentationBlock] = Field(default_factory=list)


class PresentationSink(Protocol):
    """What a renderer implements to be driven incrementally.

    A client that redraws from `PresentationReducer.blocks` wholesale does not
    need this; one that appends to a live view does.
    """

    def on_block_opened(self, block: PresentationBlock) -> None: ...

    def on_block_appended(
        self, block: PresentationBlock, delta: str
    ) -> None: ...

    def on_block_closed(self, block: PresentationBlock) -> None: ...


def tool_logs(interaction: Interaction) -> dict[str, str]:
    """The user-facing tool logs an interaction carries, keyed by call id.

    They ride in ``backend_specific_metadata`` because that is the one part of
    an interaction no backend turns into provider content: the log must never
    reach the model, but a replayed conversation is poorer without it.
    """
    raw = interaction.backend_specific_metadata.get(TOOL_LOGS_METADATA_KEY)
    if not raw:
        return {}
    try:
        decoded = json.loads(
            raw.decode() if isinstance(raw, bytes) else str(raw)
        )
    except (ValueError, UnicodeDecodeError):
        return {}
    if not isinstance(decoded, dict):
        return {}
    return {str(key): str(value) for key, value in decoded.items()}


def plain_text(interaction: Interaction) -> str:
    """Best-effort human-readable text of an interaction's content.

    What a title, a log line, or a search index wants. Tool calls and images
    contribute nothing.
    """
    message = normalize_interaction(interaction)
    return "".join(
        part.text or ""
        for part in message.parts
        if part.type == NormalizedContentType.TEXT
    )


def is_tool_result_carrier(interaction: Interaction) -> bool:
    """Whether this interaction exists only to carry tool results.

    A tool round trip is two interactions: the assistant's call, then a
    user-role interaction holding the outputs. The second is bookkeeping the
    model needs and a reader does not, so clients skip drawing it and fold its
    logs into the call it answers.
    """
    return bool(interaction.action_outputs) and not plain_text(interaction)


def present_interaction(
    interaction: Interaction, logs: Mapping[str, str] | None = None
) -> PresentationTurn:
    """The blocks a single interaction contributes.

    Args:
        interaction: The interaction to read.
        logs: User-facing tool logs by call id, as collected from the
            interaction that carries this one's tool results. Usually supplied
            by `present_conversation`, which has the whole conversation to hand.

    Returns:
        The turn. Empty of blocks when the interaction contributes nothing to
        draw.
    """
    resolved_logs = dict(logs or {})
    message = normalize_interaction(interaction)
    blocks: list[PresentationBlock] = []

    def block(kind: BlockKind, **fields: Any) -> PresentationBlock:
        return PresentationBlock(
            kind=kind,
            interaction_id=interaction.id,
            role=interaction.role,
            **fields,
        )

    text = "".join(
        part.text or ""
        for part in message.parts
        if part.type == NormalizedContentType.TEXT
    )
    if text:
        blocks.append(block(BlockKind.TEXT, text=text))

    for part in message.parts:
        if part.type == NormalizedContentType.IMAGE:
            blocks.append(
                block(
                    BlockKind.IMAGE,
                    mime_type=part.mime_type or "",
                    text=part.text or "",
                )
            )

    # Tool calls come from `action_calls` rather than from the normalized parts:
    # it is the backend-independent record of what ran, and it carries the call
    # ids the logs are keyed by.
    for call in interaction.action_calls:
        blocks.append(
            block(
                BlockKind.TOOL_RUN,
                id=call.id,
                tool_name=call.name,
                text=resolved_logs.get(call.id, ""),
            )
        )

    if interaction.usage_metadata is not None:
        blocks.append(block(BlockKind.USAGE, usage=interaction.usage_metadata))

    if interaction.status is not None and not interaction.status.is_ok():
        blocks.append(block(BlockKind.ERROR, status=interaction.status))

    return PresentationTurn(
        role=interaction.role,
        interaction_ids=[interaction.id],
        blocks=blocks,
    )


def present_conversation(
    interactions: Sequence[Interaction],
) -> list[PresentationTurn]:
    """The turns a stored conversation should be drawn as.

    Collects every interaction's tool logs first, so a call can be shown with
    the log that arrived in the *following* interaction, then skips the
    interactions that exist only to carry results. System interactions are not
    drawn.
    """
    logs: dict[str, str] = {}
    for interaction in interactions:
        logs.update(tool_logs(interaction))

    turns: list[PresentationTurn] = []
    for interaction in interactions:
        if interaction.role == Role.SYSTEM:
            continue
        if is_tool_result_carrier(interaction):
            continue
        turn = present_interaction(interaction, logs)
        if turn.blocks:
            turns.append(turn)
    return turns


class PresentationReducer:
    """Accumulates one turn's blocks, fed live or from storage.

    The live feeder calls `on_text`/`on_thought` as deltas arrive and
    `on_interaction` as whole interactions land on ``new_interactions``; a
    replay feeder calls only `on_interaction`. Either way `blocks` is what a
    client draws, and an optional `PresentationSink` receives incremental
    events.
    """

    def __init__(
        self,
        sink: PresentationSink | None = None,
        *,
        role: Role = Role.ASSISTANT,
    ) -> None:
        self._sink = sink
        self._role = role
        self._blocks: list[PresentationBlock] = []
        #: The block deltas are currently appending to, so consecutive deltas
        #: coalesce into one block instead of one block per token.
        self._open: PresentationBlock | None = None
        self._seen_calls: set[str] = set()
        self._logs: dict[str, str] = {}
        #: Whether prose has arrived as deltas. Only then is the text inside a
        #: later interaction a duplicate; text from a *different* interaction is
        #: not, which is what replaying a whole conversation depends on.
        self._streamed_text = False

    @property
    def blocks(self) -> list[PresentationBlock]:
        """The turn's blocks so far, in order."""
        return list(self._blocks)

    def on_text(self, delta: str) -> None:
        """Append assistant prose."""
        if delta:
            self._streamed_text = True
        self._append(BlockKind.TEXT, delta)

    def on_thought(self, delta: str) -> None:
        """Append exposed reasoning."""
        self._append(BlockKind.THOUGHT, delta)

    def on_interaction(self, interaction: Interaction) -> None:
        """Fold in a whole interaction, live or replayed.

        Text already streamed as deltas is not added again: on the live path the
        same prose arrives twice, once on ``text_output`` and once inside the
        interaction that lands on ``new_interactions``.
        """
        self._logs.update(tool_logs(interaction))
        # A late-arriving log belongs to the run block already drawn for it.
        for block in self._blocks:
            if block.kind == BlockKind.TOOL_RUN and not block.text:
                block.text = self._logs.get(block.id, "")

        if is_tool_result_carrier(interaction):
            return

        turn = present_interaction(interaction, self._logs)
        for block in turn.blocks:
            # Only deltas make an interaction's text a duplicate: on the live
            # path the same prose arrives twice, once on `text_output` and once
            # inside the interaction that lands on `new_interactions`.
            if block.kind == BlockKind.TEXT and self._streamed_text:
                continue
            if block.kind == BlockKind.TOOL_RUN:
                if block.id in self._seen_calls:
                    continue
                self._seen_calls.add(block.id)
            self._close_open()
            self._blocks.append(block)
            self._emit_opened(block)
            self._emit_closed(block)

    def on_error(self, status: Status) -> None:
        """Record a failure as the turn's last block."""
        self._close_open()
        block = PresentationBlock(
            kind=BlockKind.ERROR, status=status, role=self._role
        )
        self._blocks.append(block)
        self._emit_opened(block)
        self._emit_closed(block)

    def end_turn(self) -> None:
        """Mark the turn complete, closing anything still streaming."""
        self._close_open()

    # -- internals ---------------------------------------------------------

    def _append(self, kind: BlockKind, delta: str) -> None:
        if not delta:
            return
        if self._open is None or self._open.kind != kind:
            self._close_open()
            self._open = PresentationBlock(
                kind=kind, text="", partial=True, role=self._role
            )
            self._blocks.append(self._open)
            self._emit_opened(self._open)
        self._open.text += delta
        if self._sink is not None:
            self._sink.on_block_appended(self._open, delta)

    def _close_open(self) -> None:
        if self._open is None:
            return
        closing, self._open = self._open, None
        closing.partial = False
        self._emit_closed(closing)

    def _emit_opened(self, block: PresentationBlock) -> None:
        if self._sink is not None:
            self._sink.on_block_opened(block)

    def _emit_closed(self, block: PresentationBlock) -> None:
        if self._sink is not None:
            self._sink.on_block_closed(block)


__all__ = [
    "BlockKind",
    "PresentationBlock",
    "PresentationReducer",
    "PresentationSink",
    "PresentationTurn",
    "is_tool_result_carrier",
    "plain_text",
    "present_conversation",
    "present_interaction",
    "tool_logs",
]
