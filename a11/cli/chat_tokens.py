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
        self._context_estimated = False
        self.begin_turn()

    def begin_turn(self) -> None:
        self._lengths: dict[int, int] = {}
        self._usages: dict[int | str, UsageMetadata] = {}
        self._characters = 0
        self._reported_characters = 0
        self._output = 0
        self._has_output = False
        self._finished = False

    def restore_context(self, usage: UsageMetadata) -> None:
        self._context_estimated = usage.context_source == "request_estimate"
        if usage.context_invalidated:
            self.context = None
        elif usage.context_tokens is not None:
            self.context = usage.context_tokens
        elif (
            usage.usage_scope == "turn"
            or usage.input_tokens_scope == "incremental"
        ):
            self.context = None
        elif usage.total_tokens is not None:
            self.context = usage.total_tokens
        elif usage.input_tokens is not None and usage.output_tokens is not None:
            self.context = (
                usage.input_tokens
                + (usage.cached_input_tokens or 0)
                + (usage.cache_write_tokens or 0)
                + usage.output_tokens
            )
        else:
            self.context = None

    def observe(self, block: PresentationBlock) -> None:
        key = id(block)
        if block.kind in (BlockKind.TEXT, BlockKind.THOUGHT):
            length = len(block.text)
            self._characters += max(0, length - self._lengths.get(key, 0))
            self._lengths[key] = length
        elif block.kind == BlockKind.USAGE and block.usage is not None:
            usage = block.usage
            key = usage.usage_id or block.interaction_id or key
            previous = self._usages.get(key)
            if previous is not None and (
                usage.usage_revision <= previous.usage_revision
            ):
                return
            self._usages[key] = usage.model_copy()
            self.restore_context(usage)
            if (
                not usage.context_invalidated
                and usage.output_tokens is not None
            ):
                self._output += usage.output_tokens - (
                    previous.output_tokens or 0 if previous is not None else 0
                )
                self._has_output = True
                self._reported_characters = self._characters

    def finish(self, *, complete: bool = True) -> None:
        self._finished = complete

    @property
    def output_label(self) -> str:
        pending = max(0, self._characters - self._reported_characters)
        complete = all(
            u.complete
            for u in self._usages.values()
            if not u.context_invalidated and not u.context_only
        )
        if self._finished and self._has_output and complete:
            return format_tokens(self._output)
        return format_tokens(
            self._output + (pending + 3) // 4,
            estimated=bool(pending) or not self._has_output or not complete,
        )

    @property
    def context_label(self) -> str:
        if self.context is None:
            return "— tok"
        return format_tokens(self.context, estimated=self._context_estimated)
