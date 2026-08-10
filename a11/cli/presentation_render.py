# Copyright 2026 The A11 Authors.

"""Rendering `PresentationBlock`s with `rich`, for the terminal clients.

The whole terminal-specific half of presentation: a `match` over `BlockKind` and
nothing else. Because the blocks come from
[a11.sdk.presentation][a11.sdk.presentation], the same function draws a live turn
and a replayed one, and `a11 chat` gets tool-call rendering it never had.
"""

from __future__ import annotations

from collections.abc import Sequence

from rich.console import Group, RenderableType
from rich.markdown import Markdown
from rich.panel import Panel
from rich.text import Text

from a11.sdk.presentation import BlockKind, PresentationBlock


def render_block(
    block: PresentationBlock, *, verbose: bool = False
) -> RenderableType | None:
    """Render one block, or ``None`` when it should not be drawn.

    Args:
        block: The block to render.
        verbose: Draw blocks that are noise in normal use -- thoughts and token
            usage.

    Returns:
        A `rich` renderable, or ``None`` to omit the block.
    """
    match block.kind:
        case BlockKind.TEXT:
            return Markdown(block.text) if block.text else None
        case BlockKind.THOUGHT:
            if not verbose or not block.text:
                return None
            return Text(block.text.strip(), style="dim italic")
        case BlockKind.TOOL_RUN:
            return _tool_panel(block)
        case BlockKind.TOOL_RESULT:
            # The run panel already carries the tool's own account of itself;
            # the raw result is for the model, not the reader.
            return None
        case BlockKind.IMAGE:
            label = block.mime_type or "image"
            return Text(f"[{label}]", style="dim")
        case BlockKind.ERROR:
            message = block.status.message if block.status else "failed"
            return Text(f"error: {message}", style="red")
        case BlockKind.USAGE:
            if not verbose or block.usage is None:
                return None
            return Text(_usage_line(block), style="dim")
    return None


def render_blocks(
    blocks: Sequence[PresentationBlock], *, verbose: bool = False
) -> RenderableType:
    """Render a whole turn, skipping blocks that should not be drawn."""
    rendered = [
        renderable
        for renderable in (
            render_block(block, verbose=verbose) for block in blocks
        )
        if renderable is not None
    ]
    return Group(*rendered)


def _tool_panel(block: PresentationBlock) -> RenderableType:
    """A tool run as a titled panel whose body is the tool's own log.

    A run with no log yet is still drawn -- that a tool is running is the thing
    the reader most wants to know while they wait.
    """
    title = block.tool_name or "tool"
    if block.status is not None and not block.status.is_ok():
        return Panel(
            Text(block.status.message or "failed", style="red"),
            title=f"{title} (failed)",
            title_align="left",
            border_style="red",
        )
    body: RenderableType = (
        Text(block.text.rstrip())
        if block.text
        else Text("running…", style="dim italic")
    )
    return Panel(body, title=title, title_align="left", border_style="dim")


def _usage_line(block: PresentationBlock) -> str:
    usage = block.usage
    assert usage is not None  # guarded by the caller
    parts = []
    for label, value in (
        ("in", getattr(usage, "input_tokens", None)),
        ("out", getattr(usage, "output_tokens", None)),
    ):
        if value:
            parts.append(f"{label} {value}")
    return f"tokens: {', '.join(parts)}" if parts else "tokens: n/a"


__all__ = ["render_block", "render_blocks"]
