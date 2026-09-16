"""Token counters for the chat composer and streaming activity line."""

from a11.sdk.llm import UsageMetadata
from a11.sdk.presentation import BlockKind, PresentationBlock


def format_tokens(count: int, *, estimated: bool = False) -> str:
    """Format tokens with decimal SI units and an optional estimate marker."""
    prefix = "~" if estimated else ""
    if count >= 1_000_000:
        return f"{prefix}{count / 1_000_000:.1f}M tok"
    if count >= 1_000:
        return f"{prefix}{count / 1_000:.1f}K tok"
    return f"{prefix}{count} tok"


class ChatTokens:
    """Track last reported context and this turn's output, not lifetime usage.

    Between usage reports, visible text and reasoning use a four-character
    token estimate. Reported output includes tokens absent from those streams,
    such as tool arguments and hidden reasoning.
    """

    def __init__(self) -> None:
        self.context: int | None = None
        self.begin_turn()

    def begin_turn(self) -> None:
        self._lengths: dict[int, int] = {}
        self._usage_ids: set[int] = set()
        self._characters = 0
        self._reported_characters = 0
        self._output = 0
        self._has_output = False
        self._finished = False

    def restore_context(self, usage: UsageMetadata) -> None:
        if usage.total_tokens is not None:
            self.context = usage.total_tokens
        elif usage.input_tokens is not None:
            self.context = usage.input_tokens + (usage.output_tokens or 0)

    def observe(self, block: PresentationBlock) -> None:
        key = id(block)
        if block.kind in (BlockKind.TEXT, BlockKind.THOUGHT):
            length = len(block.text)
            self._characters += max(0, length - self._lengths.get(key, 0))
            self._lengths[key] = length
        elif block.kind == BlockKind.USAGE and block.usage is not None:
            if key in self._usage_ids:
                return
            self._usage_ids.add(key)
            self.restore_context(block.usage)
            if block.usage.output_tokens is not None:
                self._output += block.usage.output_tokens
                self._has_output = True
                self._reported_characters = self._characters

    def finish(self, *, complete: bool = True) -> None:
        self._finished = complete

    @property
    def output_label(self) -> str:
        pending = max(0, self._characters - self._reported_characters)
        if self._finished and self._has_output:
            return format_tokens(self._output)
        return format_tokens(
            self._output + (pending + 3) // 4,
            estimated=bool(pending) or not self._has_output,
        )

    @property
    def context_label(self) -> str:
        if self.context is None:
            return "— tok"
        return format_tokens(self.context)
