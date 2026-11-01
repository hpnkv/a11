"""Reconcile reported usage independently of assistant content."""

from __future__ import annotations

import asyncio
import inspect
import uuid
from contextlib import asynccontextmanager
from contextvars import ContextVar
from typing import Any, Literal

from pydantic import BaseModel, ConfigDict, Field, TypeAdapter, model_validator
from pydantic_core import to_json

import a11
from a11.status import StatusException
from a11.sdk.llm import LlmHeaders, UsageMetadata
from a11.sdk.llm_context.config import config_for, services_for

EVENT_TYPE = "a11.usage"
METADATA_KEY = "a11.usage"
USAGE_PURPOSE = "llm_usage"
USAGE_PARENT_HEADER = "x-a11-usage-producer"
usage_observer: ContextVar[Any] = ContextVar("a11_usage_observer", default=None)
COUNTERS = (
    "input_tokens",
    "cached_input_tokens",
    "cache_write_tokens",
    "output_tokens",
    "reasoning_tokens",
    "total_tokens",
)


def usage_tracking_enabled(action: a11.Action) -> bool:
    """Read the inherited opt-in for dedicated usage streams."""
    value = action.get_header(LlmHeaders.USAGE_TRACKING, decode=True)
    if value not in (None, "on", "off"):
        raise a11.Status(
            code=a11.StatusCode.INVALID_ARGUMENT,
            message="x-a11-usage-tracking must be 'on' or 'off'.",
        ).to_exception()
    return value == "on"


def enable_usage_tracking(
    action: a11.Action, enabled: bool = True
) -> a11.Action:
    """Request usage sources for an action subtree, or suppress them with False.

    Configure before running or calling the action. Absence defaults to off;
    an explicit off overrides an inherited request for nested producers.
    Existing interaction and provider-event usage remains available.
    """
    if action.has_been_run() or action.has_been_called():
        raise a11.Status(
            code=a11.StatusCode.FAILED_PRECONDITION,
            message="Configure usage tracking before starting the action.",
        ).to_exception()
    action.set_header(LlmHeaders.USAGE_TRACKING, "on" if enabled else "off")
    return action


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
    producer_id: str | None = Field(
        default=None, exclude_if=lambda value: value is None
    )
    covers: list[str] = Field(
        default_factory=list, exclude_if=lambda value: not value
    )
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

    def __init__(
        self,
        action: Any,
        provider: str,
        model: str = "",
        *,
        usage_source: dict[str, Any] | None = None,
    ) -> None:
        self.action = action
        self.provider = provider
        self.model = model
        self.id = uuid.uuid4().hex
        self.revision = 0
        self.usage: UsageMetadata | None = None
        self.scope = UsageScope()
        self.usage_source = usage_source
        self.stream_reference: dict[str, Any] | None = None
        self._stream = None
        get_header = getattr(action, "get_header", None)
        self.operation = (
            get_header("x-a11-llm-context-operation", decode=True)
            if get_header is not None
            else None
        ) or "generation"

    async def report(
        self,
        usage: UsageMetadata | None,
        source: Any = None,
        *,
        complete: bool = True,
        source_id: str | None = None,
        coverage: str = "reported",
        covers: list[str] | None = None,
    ) -> UsageMetadata | None:
        """Publish counts supplied by the source, including partial reports."""
        self._ensure_stream()
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
            covers=covers or [],
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

    def _ensure_stream(self) -> None:
        if self.stream_reference is not None or not isinstance(
            self.action, a11.Action
        ):
            return
        if not usage_tracking_enabled(self.action):
            return
        parent = self.action.get_header(USAGE_PARENT_HEADER, decode=True) or ""
        source = self.usage_source or {
            "type": "a11_action",
            "id": self.action.id,
            "name": self.action.schema.name,
        }
        if self.scope.parent_agent_id is not None and self.usage_source is None:
            source = {
                "type": "provider_agent",
                "provider": self.provider,
                "id": self.scope.agent_id,
                "action_id": self.action.id,
            }
        if self.scope.parent_agent_id is None:
            owner_header = "x-a11-usage-producer-action"
            parent_header = "x-a11-usage-parent-producer"
            if (
                self.action.get_header(owner_header, decode=True)
                == self.action.id
            ):
                parent = (
                    self.action.get_header(parent_header, decode=True) or ""
                )
            else:
                self.action.set_header(owner_header, self.action.id)
                self.action.set_header(parent_header, parent)
        source = dict(source)
        if self.action.trace_id:
            source["trace_id"] = self.action.trace_id
        if self.action.span_id:
            source["span_id"] = self.action.span_id
        self.stream_reference = self.action.register_stream(
            USAGE_PURPOSE,
            source,
            producer_id=self.id,
            parent_producer_id=parent,
        )
        self._stream = self.action.get_node_map().resolve(
            a11.NodeRef.model_validate(self.stream_reference["node"])
        )
        if self.scope.parent_agent_id is None:
            self.action.set_header(USAGE_PARENT_HEADER, self.id)

    async def _publish(self, update: UsageUpdate) -> None:
        self._ensure_stream()
        if self._stream is not None:
            update = update.model_copy(update={"producer_id": self.id})
            await self._stream.put(update.model_dump(mode="json"))
        event = update.model_dump(mode="json")
        services = services_for(self.action)
        if services.recorder is not None:
            config = config_for(self.action)
            await asyncio.to_thread(
                services.recorder.record,
                {
                    **event,
                    "run_id": config.measurement.run_id,
                    "arm": config.measurement.arm,
                },
            )
        if observer := usage_observer.get():
            await observer(event)
        if (
            not isinstance(self.action, a11.Action)
            or "event_stream" in self.action
        ):
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
        records = self.contributions()
        total = summarize_usage([record.usage for record in records])
        primary = [
            record.usage
            for record in records
            if record.scope.parent_agent_id is None
        ]
        context = summarize_usage(primary)
        total.context_tokens = context.context_tokens
        total.context_source = context.context_source
        if any(record.scope.reporting == "cumulative" for record in records):
            for name in COUNTERS:
                setattr(total, name, None)
            total.complete = False
        return total

    def contributions(self) -> list[UsageUpdate]:
        """Select consumption once, retaining covered reports as evidence."""
        records = {
            key: record
            for key, record in self.records.items()
            if record.contribution == "consumption"
            and not record.usage.context_only
            and not record.usage.context_invalidated
        }
        covered: set[str] = set()
        for key, record in records.items():
            pending = list(record.covers)
            visited: set[str] = set()
            while pending:
                target = pending.pop()
                if target == key:
                    raise ValueError("Usage coverage contains a cycle")
                if target in visited:
                    continue
                visited.add(target)
                covered.add(target)
                if target in records:
                    pending.extend(records[target].covers)
        aggregates = {
            (r.scope.agent_id, r.scope.turn_id)
            for r in records.values()
            if r.usage.usage_scope == "turn"
            and r.scope.agent_id
            and r.scope.turn_id
        }
        return [
            r
            for key, r in records.items()
            if key not in covered
            and (
                r.usage.usage_scope == "turn"
                or (r.scope.agent_id, r.scope.turn_id) not in aggregates
            )
        ]


class UsageSourceReference(BaseModel):
    """Locate one usage producer independently of its usage observations."""

    model_config = ConfigDict(extra="ignore")
    producer_id: str
    parent_producer_id: str = ""
    node: a11.NodeRef
    purpose: Literal["llm_usage"] = USAGE_PURPOSE
    source: dict[str, Any]

    @property
    def node_id(self) -> str:
        """Return the id carried by the native node reference."""
        return self.node.id

    @property
    def usage_source(self) -> dict[str, Any]:
        """Describe the action, agent, or tool producing usage."""
        return self.source


class UsageAccounting:
    """Collect direct and nested usage without consuming provider event streams.

    Construct before starting the action to request tracking. Start ``collect``
    alongside it, or read retained usage after an explicitly tracked action
    finishes. Each collection opens independent read cursors. Totals cover
    registered producers; tools without instrumentation remain unobserved.
    """

    def __init__(self, action: a11.Action) -> None:
        self.action = action
        self.sources: dict[str, UsageSourceReference] = {}
        self.ledgers: dict[str, UsageLedger] = {}
        self.errors: dict[str, a11.Status] = {}
        self.finished = False
        self._collecting = False
        started = action.has_been_run() or action.has_been_called()
        explicit_off = (
            action.get_header(LlmHeaders.USAGE_TRACKING, decode=True) == "off"
        )
        if not started and not explicit_off:
            enable_usage_tracking(action)
        self.enabled = usage_tracking_enabled(action)
        self._sources_node = (
            action.get_stream_sources_node() if self.enabled else None
        )

    async def collect(self) -> UsageAccounting:
        """Discover producers and drain their usage streams concurrently."""
        if self._collecting or self.finished:
            raise ValueError("A usage collection may run only once")
        if not self.enabled:
            self.finished = True
            return self
        self._collecting = True
        tasks: list[asyncio.Task] = []
        node = self._sources_node
        try:
            async for value in self._values(node):
                if (
                    not isinstance(value, dict)
                    or value.get("purpose") != USAGE_PURPOSE
                ):
                    continue
                reference = UsageSourceReference.model_validate(value)
                previous = self.sources.get(reference.producer_id)
                if previous is not None:
                    if previous != reference:
                        raise ValueError("Conflicting usage producer identity")
                    continue
                self.sources[reference.producer_id] = reference
                self.ledgers[reference.producer_id] = UsageLedger()
                tasks.append(
                    asyncio.create_task(self._collect_source(reference))
                )
            await asyncio.gather(*tasks)
        except StatusException as error:
            self.errors["scope"] = error.status
            await asyncio.gather(*tasks)
        except asyncio.CancelledError:
            self.errors["scope"] = a11.Status(
                code=a11.StatusCode.CANCELLED,
                message="Usage collection was cancelled.",
            )
            raise
        except ValueError as error:
            self.errors["scope"] = a11.Status(
                code=a11.StatusCode.INVALID_ARGUMENT, message=str(error)
            )
            raise
        finally:
            for task in tasks:
                if not task.done():
                    task.cancel()
            await asyncio.gather(*tasks, return_exceptions=True)
            self.finished = True
            self._collecting = False
        return self

    @staticmethod
    async def _values(node):
        reader = a11.ChunkStoreReader(node.get_chunk_store())
        try:
            async for fragment in reader:
                chunk = fragment.get_chunk()
                if not chunk.is_null():
                    yield a11.from_chunk(chunk)
        finally:
            reader.cancel()

    async def _collect_source(self, reference: UsageSourceReference) -> None:
        try:
            node = self.action.get_node_map().resolve(reference.node)
            async for value in self._values(node):
                update = parse_update(value)
                if update is None:
                    raise ValueError(
                        "A usage stream requires UsageUpdate values"
                    )
                if update.producer_id not in (None, reference.producer_id):
                    raise ValueError(
                        "Usage update belongs to a different producer"
                    )
                self.ledgers[reference.producer_id].observe(update)
        except StatusException as error:
            self.errors[reference.producer_id] = error.status

    def _ledger(self, producers) -> UsageLedger:
        ledger = UsageLedger()
        for producer in producers:
            for record in self.ledgers[producer].records.values():
                ledger.observe(record)
        return ledger

    def for_source(self, producer_id: str) -> UsageMetadata:
        """Return the latest available consumption for one producer."""
        return self.ledgers[producer_id].totals()

    @property
    def direct_usage(self) -> UsageMetadata:
        """Return consumption from producers rooted in this collection."""
        return self._ledger(
            key
            for key, source in self.sources.items()
            if source.parent_producer_id not in self.sources
        ).totals()

    @property
    def descendant_usage(self) -> UsageMetadata:
        """Return nested consumption with overlapping reports reconciled."""
        return self._ledger(
            key
            for key, source in self.sources.items()
            if source.parent_producer_id in self.sources
        ).totals()

    @property
    def total_usage(self) -> UsageMetadata:
        """Return consumption once across all discovered producers."""
        total = self._ledger(self.sources).totals()
        direct = self.direct_usage
        total.context_tokens = direct.context_tokens
        total.context_source = direct.context_source
        if (
            not self.enabled
            or not self.finished
            or self.errors
            or any(not ledger.records for ledger in self.ledgers.values())
        ):
            total.complete = False
        return total

    @property
    def coverage(
        self,
    ) -> Literal["disabled", "collecting", "incomplete", "reported_sources"]:
        """Describe collection coverage for registered sources."""
        if not self.enabled:
            return "disabled"
        if not self.finished:
            return "collecting"
        return "reported_sources" if self.total_usage.complete else "incomplete"


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


def provider_usage(profile: str, usage: Any) -> UsageMetadata | None:
    """Normalize provider wire counts for SDK clients and passive observers.

    Args:
        profile: chat, responses, or anthropic API shape.
        usage: Mapping or provider SDK object, including partial snapshots.

    Returns:
        The shared usage model with unknown counters preserved. Input excludes
        separately reported cache reads/writes; output includes reasoning.
    """
    if usage is None:
        return None

    def field(source, name):
        return (
            source.get(name)
            if isinstance(source, dict)
            else getattr(source, name, None)
        )

    if profile in ("chat", "responses"):
        details = field(
            usage,
            (
                "prompt_tokens_details"
                if profile == "chat"
                else "input_tokens_details"
            ),
        )
        output_details = field(
            usage,
            (
                "completion_tokens_details"
                if profile == "chat"
                else "output_tokens_details"
            ),
        )
        cached, written = field(details, "cached_tokens"), field(
            details, "cache_write_tokens"
        )
        prompt = field(
            usage, "prompt_tokens" if profile == "chat" else "input_tokens"
        )
        return UsageMetadata(
            input_tokens=(
                prompt - (cached or 0) - (written or 0)
                if prompt is not None
                else None
            ),
            output_tokens=field(
                usage,
                "completion_tokens" if profile == "chat" else "output_tokens",
            ),
            total_tokens=field(usage, "total_tokens"),
            cached_input_tokens=cached,
            cache_write_tokens=written,
            reasoning_tokens=field(output_details, "reasoning_tokens"),
        )
    if profile != "anthropic":
        raise ValueError(f"Unknown provider usage profile: {profile}")
    fields = {
        "input_tokens": "input_tokens",
        "output_tokens": "output_tokens",
        "cached_input_tokens": "cache_read_input_tokens",
        "cache_write_tokens": "cache_creation_input_tokens",
    }
    counts = {target: field(usage, source) for target, source in fields.items()}
    total = (
        sum(v or 0 for v in counts.values())
        if counts["input_tokens"] is not None
        and counts["output_tokens"] is not None
        else None
    )
    result = UsageMetadata(
        **counts,
        total_tokens=total,
        reasoning_tokens=field(
            field(usage, "output_tokens_details"), "thinking_tokens"
        ),
    )
    iterations = field(usage, "iterations")
    if iterations:
        for target, source in fields.items():
            values = [field(item, source) for item in iterations]
            setattr(
                result,
                target,
                sum(values) if all(v is not None for v in values) else None,
            )
        result.total_tokens = (
            sum(getattr(result, name) or 0 for name in fields)
            if result.input_tokens is not None
            and result.output_tokens is not None
            else None
        )
        last = [field(iterations[-1], source) for source in fields.values()]
        result.context_tokens = (
            sum(v or 0 for v in last)
            if last[0] is not None and last[1] is not None
            else None
        )
        result.context_source = "request_estimate"
    return result


def chat_usage(usage: Any) -> UsageMetadata | None:
    """Normalize OpenAI-compatible Chat token usage."""
    return provider_usage("chat", usage)


def responses_usage(usage: Any) -> UsageMetadata | None:
    """Normalize Responses token usage."""
    return provider_usage("responses", usage)


def anthropic_usage(usage: Any) -> UsageMetadata | None:
    """Normalize Anthropic usage, including per-iteration consumption."""
    return provider_usage("anthropic", usage)
