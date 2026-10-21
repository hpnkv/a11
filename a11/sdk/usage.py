"""Reconcile reported usage independently of assistant content."""

from __future__ import annotations

import uuid
import inspect
import asyncio
from contextlib import asynccontextmanager
from contextvars import ContextVar
from typing import Any, Literal

from pydantic import BaseModel, ConfigDict, Field, TypeAdapter, model_validator
from pydantic_core import to_json

from a11.sdk.llm import UsageMetadata

EVENT_TYPE = "a11.usage"
METADATA_KEY = "a11.usage"
usage_observer: ContextVar[Any] = ContextVar("a11_usage_observer", default=None)
COUNTERS = (
    "input_tokens",
    "cached_input_tokens",
    "cache_write_tokens",
    "output_tokens",
    "reasoning_tokens",
    "total_tokens",
)


class UsageFigure(BaseModel):
    """Quality of one reported counter, independent of other figures."""

    value: int | None = None
    quality: Literal["reported", "partial", "unknown"] = "unknown"


class UsageScope(BaseModel):
    """Identify the upstream operation and its counter lifecycle."""

    session_id: str | None = None
    turn_id: str | None = None
    request_id: str | None = None
    agent_id: str | None = None
    parent_agent_id: str | None = None
    epoch: int = Field(default=0, ge=0)
    reporting: Literal["snapshot", "cumulative", "delta"] = "snapshot"


class ProviderUsageSnapshot(BaseModel):
    """Preserve provider-owned usage fields as opaque source evidence."""

    model_config = ConfigDict(extra="allow")


class UsageUpdate(BaseModel):
    """One replacement snapshot for a stable usage identity.

    Source usage is evidence, not another contribution. An update contains
    either consumption or a context invalidation. Revisions replace earlier
    snapshots of the same operation.
    """

    type: Literal["a11.usage"] = EVENT_TYPE
    version: Literal[1] = 1
    id: str
    provider: str
    model: str = ""
    revision: int = Field(ge=1)
    usage: UsageMetadata
    source_usage: ProviderUsageSnapshot | None = None
    source_id: str | None = None
    operation: str = "generation"
    coverage: str = "reported"
    contribution: Literal["consumption", "reconciliation"] = "consumption"
    scope: UsageScope = Field(default_factory=UsageScope)
    figures: dict[str, UsageFigure] = Field(default_factory=dict)

    @model_validator(mode="after")
    def validate_identity(self) -> UsageUpdate:
        if any(
            getattr(self.usage, name) is not None
            and getattr(self.usage, name) < 0
            for name in COUNTERS
        ):
            raise ValueError("Reported usage counters must be non-negative")
        if (
            self.usage.usage_id != self.id
            or self.usage.usage_revision != self.revision
        ):
            raise ValueError(
                "Usage identity and revision must match the envelope"
            )
        return self


_updates_adapter = TypeAdapter(list[UsageUpdate])


def provider_event(value: Any) -> Any:
    """Carry SDK evidence as opaque JSON without its validation contract."""
    return (
        value.model_dump(mode="json") if hasattr(value, "model_dump") else value
    )


@asynccontextmanager
async def closing_stream(stream: Any):
    """Release a provider response on completion, failure, or cancellation."""
    try:
        yield stream
    finally:
        close = getattr(stream, "aclose", None) or getattr(
            stream, "close", None
        )
        if close is not None:
            try:
                result = close()
                if inspect.isawaitable(result):
                    await asyncio.wait_for(result, timeout=5)
            except Exception:
                pass


def parse_update(event: Any) -> UsageUpdate | None:
    """Read usage envelopes while leaving provider events untouched."""
    if isinstance(event, UsageUpdate):
        return event
    if not isinstance(event, dict) or event.get("type") != EVENT_TYPE:
        return None
    if event.get("version") != 1:
        return None
    return UsageUpdate.model_validate(event)


def interaction_updates(interaction: Any) -> list[UsageUpdate]:
    """Read usage observations stored on an interaction."""
    raw = interaction.backend_specific_metadata.get(METADATA_KEY)
    return _updates_adapter.validate_json(raw) if raw else []


def merge_updates(previous: Any, incoming: Any) -> Any:
    """Preserve usage revisions when conversational history is replayed."""
    records = {
        (event.id, event.revision): event
        for interaction in (previous, incoming)
        for event in interaction_updates(interaction)
    }
    metadata = dict(incoming.backend_specific_metadata)
    if records:
        metadata[METADATA_KEY] = to_json(list(records.values()))
    return incoming.model_copy(update={"backend_specific_metadata": metadata})


class UsageReporter:
    """Publish replacement usage snapshots on a provider's event stream."""

    def __init__(self, action: Any, provider: str, model: str = "") -> None:
        self.action = action
        self.provider = provider
        self.model = model
        self.id = uuid.uuid4().hex
        self.revision = 0
        self.usage: UsageMetadata | None = None
        self.scope = UsageScope()
        self.operation = "generation"

    async def report(
        self,
        usage: UsageMetadata | None,
        source: Any = None,
        *,
        complete: bool = True,
        source_id: str | None = None,
        coverage: str = "reported",
    ) -> UsageMetadata | None:
        """Publish counts supplied by the source, including partial reports."""
        if usage is None:
            return None
        self.revision += 1
        self.usage = usage.model_copy(
            update={
                "usage_id": self.id,
                "usage_revision": self.revision,
                "complete": complete and usage.complete,
            }
        )
        if hasattr(source, "model_dump"):
            source = source.model_dump(mode="json")
        update = UsageUpdate(
            id=self.id,
            provider=self.provider,
            model=self.model,
            revision=self.revision,
            usage=self.usage,
            source_usage=source,
            source_id=source_id,
            coverage=coverage,
            operation=self.operation,
            scope=self.scope.model_copy(
                update=(
                    {"request_id": source_id or self.scope.request_id}
                    if self.usage.usage_scope == "request"
                    else {}
                )
            ),
            figures={
                name: UsageFigure(
                    value=getattr(self.usage, name),
                    quality=(
                        "unknown"
                        if getattr(self.usage, name) is None
                        else "reported" if self.usage.complete else "partial"
                    ),
                )
                for name in COUNTERS
            },
        )
        await self._publish(update)
        return self.usage

    async def _publish(self, update: UsageUpdate) -> None:
        event = update.model_dump(mode="json")
        if observer := usage_observer.get():
            await observer(event)
        flushed = await self.action["event_stream"].put(event)
        if inspect.isawaitable(flushed):
            await flushed

    async def invalidate_context(
        self, source: Any, *, operation: str, context_tokens: int | None = None
    ) -> None:
        """Publish a context transition without another token contribution."""
        identity = uuid.uuid4().hex
        if hasattr(source, "model_dump"):
            source = source.model_dump(mode="json")
        update = UsageUpdate(
            id=identity,
            provider=self.provider,
            model=self.model,
            revision=1,
            usage=UsageMetadata(
                usage_id=identity,
                usage_revision=1,
                complete=False,
                context_invalidated=context_tokens is None,
                context_only=True,
                context_tokens=context_tokens,
                context_source=(
                    "reported" if context_tokens is not None else "unknown"
                ),
            ),
            source_usage=source,
            operation=operation,
            scope=self.scope,
        )
        await self._publish(update)


class UsageLedger:
    """Replace revisions and total only available reported consumption."""

    def __init__(self) -> None:
        self.records: dict[str, UsageUpdate] = {}

    def observe(self, update: UsageUpdate) -> bool:
        previous = self.records.get(update.id)
        if previous is not None and previous.revision >= update.revision:
            return False
        self.records.pop(update.id, None)
        self.records[update.id] = update
        return True

    def totals(self) -> UsageMetadata:
        total = summarize_usage(
            [
                record.usage
                for record in self.records.values()
                if record.contribution == "consumption"
            ]
        )
        primary = [
            record.usage
            for record in self.records.values()
            if record.contribution == "consumption"
            and record.scope.parent_agent_id is None
        ]
        context = summarize_usage(primary)
        total.context_tokens = context.context_tokens
        total.context_source = context.context_source
        return total


def summarize_usage(values: list[UsageMetadata]) -> UsageMetadata:
    """Sum consumption snapshots for a completed turn's presentation."""
    usages = [
        u for u in values if not u.context_only and not u.context_invalidated
    ]
    counts = {}
    for name in COUNTERS:
        counters = [getattr(usage, name) for usage in usages]
        counts[name] = (
            sum(counters)
            if counters and all(v is not None for v in counters)
            else None
        )
    last = values[-1] if values else UsageMetadata()
    context = last.context_tokens
    source = last.context_source
    if (
        context is None
        and last.complete
        and last.usage_scope == "request"
        and last.input_tokens_scope == "context"
        and not last.context_invalidated
    ):
        context = last.total_tokens
        if (
            context is None
            and last.input_tokens is not None
            and last.output_tokens is not None
        ):
            context = (
                last.input_tokens
                + (last.cached_input_tokens or 0)
                + (last.cache_write_tokens or 0)
                + last.output_tokens
            )
        source = "request_estimate" if context is not None else "unknown"
    return UsageMetadata(
        **counts,
        usage_scope="turn",
        complete=bool(usages) and all(u.complete for u in usages),
        context_tokens=context,
        context_source=source,
    )


def usage_delta(
    current: UsageMetadata, baseline: UsageMetadata | None
) -> UsageMetadata:
    """Subtract a verified cumulative baseline without inventing counts."""
    values = {}
    complete = current.complete and baseline is not None
    for field in COUNTERS:
        value = getattr(current, field)
        before = getattr(baseline, field) if baseline is not None else None
        values[field] = (
            value - before
            if value is not None and before is not None and value >= before
            else None
        )
        if value is not None and values[field] is None:
            complete = False
    return current.model_copy(update={**values, "complete": complete})
