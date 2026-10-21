"""Reconcile live usage with independently captured provider counters."""

from collections.abc import Iterable
from typing import Any
from pydantic import BaseModel, Field

from a11.sdk.llm import UsageMetadata
from a11.sdk.usage import COUNTERS, ProviderUsageSnapshot, parse_update


class UsageComparison(BaseModel):
    """Compare source reports with normalized usage for one operation."""

    source: ProviderUsageSnapshot
    expected: UsageMetadata
    actual: UsageMetadata
    differences: dict[str, int]


class ScenarioTriggers(BaseModel):
    """Record observed scenario evidence separately from requested setup."""

    compaction: list[ProviderUsageSnapshot] = Field(default_factory=list)
    subagents: list[ProviderUsageSnapshot] = Field(default_factory=list)
    truncation: list[ProviderUsageSnapshot] = Field(default_factory=list)
    failure: list[str] = Field(default_factory=list)
    clearing: list[ProviderUsageSnapshot] = Field(default_factory=list)


class CapturedToolOutput(BaseModel):
    call_id: str | None = None
    characters: int
    truncated: bool
    head: str
    tail: str


class CapturedRequest(BaseModel):
    path: str = ""
    model: str | None = None
    tool_outputs: list[CapturedToolOutput] = Field(default_factory=list)
    usage: list[ProviderUsageSnapshot] = Field(default_factory=list)
    error: str | None = None
    status_code: int | None = None
    injected: bool = False


class RecordedScenario(BaseModel):
    """A reduced live trace and its independently reconciled usage."""

    name: str
    provider: str
    model: str
    sdk_versions: dict[str, str] = Field(default_factory=dict)
    baseline: ProviderUsageSnapshot | None = None
    original_tool_output_bytes: list[int] = Field(default_factory=list)
    events: list[dict[str, Any]]
    comparison: list[UsageComparison]
    triggers: ScenarioTriggers
    captured_requests: list[CapturedRequest] = Field(default_factory=list)


class UsageEvidenceBundle(BaseModel):
    recorded_on: str
    cli_versions: dict[str, str]
    cases: list[RecordedScenario]


def source_counts(provider: str, raw: dict[str, Any]) -> UsageMetadata:
    """Apply explicit source-field inclusion rules for verification."""
    if provider == "codex":
        prompt, cached, written, output, reasoning, total = (
            raw.get(name)
            for name in (
                "inputTokens",
                "cachedInputTokens",
                "cacheWriteInputTokens",
                "outputTokens",
                "reasoningOutputTokens",
                "totalTokens",
            )
        )
    elif provider == "gemini":
        prompt, cached, written = (
            raw.get("total_input_tokens"),
            raw.get("total_cached_tokens"),
            None,
        )
        reasoning = raw.get("total_thought_tokens")
        output = raw.get("total_output_tokens")
        if output is not None:
            output += reasoning or 0
        total = raw.get("total_tokens")
    elif provider == "ollama":
        prompt, cached, written = (
            raw.get("prompt_eval_count"),
            raw.get("prompt_eval_cached_count"),
            None,
        )
        output, reasoning = raw.get("eval_count"), None
        total = (
            prompt + output
            if prompt is not None and output is not None
            else None
        )
    elif provider in {"claude", "claude_code"}:
        if provider == "claude" and raw.get("iterations"):
            rows = raw["iterations"]
            raw = {
                **raw,
                **{
                    field: (
                        sum(row[field] for row in rows)
                        if all(row.get(field) is not None for row in rows)
                        else None
                    )
                    for field in (
                        "input_tokens",
                        "output_tokens",
                        "cache_read_input_tokens",
                        "cache_creation_input_tokens",
                    )
                },
            }
        prompt, cached, written = (
            raw.get(n)
            for n in (
                "input_tokens",
                "cache_read_input_tokens",
                "cache_creation_input_tokens",
            )
        )
        output = raw.get("output_tokens")
        reasoning = (raw.get("output_tokens_details") or {}).get(
            "thinking_tokens"
        )
        total = (
            prompt + (cached or 0) + (written or 0) + output
            if prompt is not None and output is not None
            else None
        )
        # These sources already report disjoint input.
        prompt = (
            prompt + (cached or 0) + (written or 0)
            if prompt is not None
            else None
        )
    else:
        responses = "input_tokens" in raw
        prompt = raw.get("input_tokens" if responses else "prompt_tokens")
        details = (
            raw.get(
                "input_tokens_details" if responses else "prompt_tokens_details"
            )
            or {}
        )
        cached, written = details.get("cached_tokens"), details.get(
            "cache_write_tokens"
        )
        output = raw.get("output_tokens" if responses else "completion_tokens")
        reasoning = (
            raw.get(
                "output_tokens_details"
                if responses
                else "completion_tokens_details"
            )
            or {}
        ).get("reasoning_tokens")
        total = raw.get("total_tokens")
    return UsageMetadata(
        **dict(
            zip(
                COUNTERS,
                (
                    (
                        prompt - (cached or 0) - (written or 0)
                        if prompt is not None
                        else None
                    ),
                    cached,
                    written,
                    output,
                    reasoning,
                    total,
                ),
            )
        )
    )


def _model_counts(models: dict) -> dict:
    names = {
        "input_tokens": "inputTokens",
        "output_tokens": "outputTokens",
        "cache_read_input_tokens": "cacheReadInputTokens",
        "cache_creation_input_tokens": "cacheCreationInputTokens",
    }
    result = {
        dest: sum(m[source] for m in models.values())
        for dest, source in names.items()
    }
    if all("thinkingTokens" in m for m in models.values()):
        result["output_tokens_details"] = {
            "thinking_tokens": sum(m["thinkingTokens"] for m in models.values())
        }
    return result


def reconcile(
    provider: str, events: Iterable[dict], baseline: dict | None = None
) -> tuple[list[dict], dict | None]:
    """Require exact agreement with terminal source reports for this probe."""
    events = list(events)
    raw = []
    primary = None
    thread_totals = {}
    for event in events:
        kind = event.get("type", event.get("event_type"))
        if (
            provider == "codex"
            and event.get("method") == "thread/tokenUsage/updated"
        ):
            primary = event["params"]["tokenUsage"]["total"]
            thread_totals[event["params"].get("threadId", "root")] = primary
        elif provider == "claude_code" and kind == "result_message":
            raw = [
                (
                    _model_counts(event["model_usage"])
                    if event.get("model_usage")
                    else event.get("usage") or {}
                )
            ]
        elif provider == "claude":
            if kind == "message_start":
                primary = dict(event["message"]["usage"])
            elif kind == "message_delta" and primary is not None:
                primary.update(event.get("usage") or {})
            elif kind == "message_stop" and primary is not None:
                raw.append(primary)
                primary = None
        elif provider == "gemini" and event.get("event_type") in {
            "interaction.completed",
            "interaction.failed",
        }:
            raw.append(event["interaction"]["usage"])
        elif provider == "ollama" and event.get("done"):
            raw.append(event)
        elif provider in {"gpt", "vllm"}:
            if kind in {
                "response.completed",
                "response.incomplete",
                "response.failed",
            }:
                raw.append(event["response"].get("usage") or {})
            elif event.get("object") == "chat.completion.chunk" and event.get(
                "usage"
            ):
                raw.append(event["usage"])
    next_baseline = baseline
    if provider == "claude_code" and raw:
        version = next(
            (
                (event.get("data") or {}).get("claude_code_version", "")
                for event in events
                if event.get("subtype") == "init"
            ),
            "",
        )
        try:
            cumulative = tuple(int(p) for p in version.split(".")) >= (
                2,
                1,
                277,
            )
        except ValueError:
            cumulative = False
        if cumulative:
            snapshot = raw[-1]
            if baseline is not None:
                before = source_counts(provider, baseline)
                after = source_counts(provider, snapshot)
                differences = {
                    field: (
                        getattr(after, field) - getattr(before, field)
                        if getattr(after, field) is not None
                        and getattr(before, field) is not None
                        else None
                    )
                    for field in COUNTERS
                }
                raw = [
                    {
                        "input_tokens": differences["input_tokens"],
                        "output_tokens": differences["output_tokens"],
                        "cache_read_input_tokens": differences[
                            "cached_input_tokens"
                        ],
                        "cache_creation_input_tokens": differences[
                            "cache_write_tokens"
                        ],
                        "output_tokens_details": {
                            "thinking_tokens": differences["reasoning_tokens"]
                        },
                    }
                ]
            next_baseline = snapshot
    if provider == "codex" and primary is not None:
        previous = (baseline or {}).get("threads", {})
        raw = []
        for index, (thread, total) in enumerate(thread_totals.items()):
            before = previous.get(thread, baseline if index == 0 else {}) or {}
            raw.append({k: v - before.get(k, 0) for k, v in total.items()})
        next_baseline = {"threads": thread_totals}
    latest = {}
    for event in events:
        update = parse_update(event)
        if (
            update is not None
            and not update.usage.context_only
            and not update.usage.context_invalidated
        ):
            previous = latest.get(update.id)
            if previous is None or update.revision > previous.revision:
                latest[update.id] = update
    assert raw, "No terminal harness/provider usage captured"
    assert len(raw) == len(latest), (
        "Source/request coverage differs",
        len(raw),
        len(latest),
    )
    rows = []
    for source, update in zip(raw, latest.values()):
        expected = source_counts(provider, source)
        actual = update.usage
        differences = {
            name: getattr(actual, name) - getattr(expected, name)
            for name in COUNTERS
            if getattr(expected, name) is not None
            and getattr(actual, name) is not None
        }
        assert all(
            getattr(actual, name) == getattr(expected, name)
            for name in COUNTERS
            if getattr(expected, name) is not None
        ), (provider, expected, actual)
        rows.append(
            UsageComparison(
                source=source,
                expected=expected,
                actual=actual,
                differences=differences,
            )
        )
    return rows, next_baseline


def reconcile_partial_claude_code(
    events: Iterable[dict],
) -> list[UsageComparison]:
    """Compare retained input against actual pre-interruption messages."""
    requests = {}
    latest = None
    for event in events:
        if (
            event.get("type") == "assistant_message"
            and event.get("message_id")
            and event.get("usage")
        ):
            requests[event["message_id"]] = event["usage"]
        update = parse_update(event)
        if update is not None and not update.usage.context_only:
            latest = update
    assert requests and latest is not None, "No consumed input captured"
    raw = {
        field: sum(row[field] for row in requests.values())
        for field in (
            "input_tokens",
            "cache_read_input_tokens",
            "cache_creation_input_tokens",
        )
        if all(row.get(field) is not None for row in requests.values())
    }
    expected = source_counts("claude_code", raw)
    differences = {}
    for field in COUNTERS:
        count = getattr(expected, field)
        if count is not None:
            assert getattr(latest.usage, field) == count, field
            differences[field] = 0
    assert not latest.usage.complete
    return [
        UsageComparison(
            source=ProviderUsageSnapshot(**raw),
            expected=expected,
            actual=latest.usage,
            differences=differences,
        )
    ]


def scenario_triggers(events: Iterable[dict]) -> ScenarioTriggers:
    """Identify harness boundaries from observed events."""
    found = {
        "compaction": [],
        "subagents": [],
        "truncation": [],
        "failure": [],
        "clearing": [],
    }
    for event in events:
        params = event.get("params") or {}
        item = params.get("item") or {}
        block = event.get("content_block") or {}
        if block.get("type") == "compaction":
            found["compaction"].append(block)
        for edit in (event.get("context_management") or {}).get(
            "applied_edits", []
        ):
            if edit.get("cleared_input_tokens", 0) > 0:
                found["clearing"].append(edit)
        if (
            item.get("type") == "contextCompaction"
            and event.get("method") == "item/completed"
        ):
            found["compaction"].append(item)
        if event.get("subtype") == "compact_boundary":
            found["compaction"].append(
                (event.get("data") or {}).get("compact_metadata")
            )
        if event.get("parent_tool_use_id"):
            found["subagents"].append(
                {
                    "parent_tool_use_id": event["parent_tool_use_id"],
                    "type": event.get("type"),
                }
            )
        if (
            item.get("type") == "collabAgentToolCall"
            and item.get("receiverThreadIds")
            and event.get("method") == "item/completed"
        ):
            found["subagents"].append(item)
        if event.get("type") == "user_message":
            for block in event.get("content", []):
                if not isinstance(block, dict):
                    continue
                content = str(block.get("content", ""))
                if "exceeds maximum allowed tokens" in content:
                    found["truncation"].append(
                        {
                            "tool_use_id": block.get("tool_use_id"),
                            "message": content,
                        }
                    )
        if event.get("type") == "result_message" and str(
            event.get("subtype", "")
        ).startswith("error"):
            found["failure"].append(event["subtype"])
        if (
            item.get("type") == "commandExecution"
            and "truncated" in str(item.get("aggregatedOutput", "")).lower()
        ):
            found["truncation"].append(
                {"id": item.get("id"), "output": item.get("aggregatedOutput")}
            )
    return ScenarioTriggers.model_validate(found)


class UsageCaptureProxy:
    """Forward bounded Codex probes and inspect model-visible tool outputs."""

    def __init__(
        self, *, anthropic: bool = False, fault_model: str | None = None
    ):
        self.server = None
        self.exchanges: list[CapturedRequest] = []
        self.anthropic = anthropic
        self.fault_model = fault_model
        self.primary_calls = 0

    async def start(self) -> str:
        import asyncio

        self.server = await asyncio.start_server(self._serve, "127.0.0.1", 0)
        port = self.server.sockets[0].getsockname()[1]
        return f"http://127.0.0.1:{port}" + ("" if self.anthropic else "/v1")

    async def close(self) -> None:
        self.server.close()
        await self.server.wait_closed()

    async def _serve(self, reader, writer):
        import httpx
        from pydantic_core import from_json, to_json

        try:
            header = (await reader.readuntil(b"\r\n\r\n")).decode("latin1")
            first, *lines = header.split("\r\n")
            method, path, _ = first.split(" ", 2)
            headers = dict(
                line.split(": ", 1) for line in lines if ": " in line
            )
            lowered = {k.lower(): v for k, v in headers.items()}
            length = int(lowered.get("content-length", "0"))
            if length > 512_000:
                raise ValueError("Probe request exceeds capture budget")
            body = await reader.readexactly(length)
            request = from_json(body) if body else {}
            exchange = CapturedRequest(path=path, model=request.get("model"))
            for item in request.get("input", []):
                if item.get("type") == "function_call_output":
                    output = str(item.get("output", ""))
                    exchange.tool_outputs.append(
                        CapturedToolOutput.model_validate(
                            {
                                "call_id": item.get("call_id"),
                                "characters": len(output),
                                "truncated": "truncated" in output.lower(),
                                "head": output[:500],
                                "tail": output[-200:],
                            }
                        )
                    )
            for message in request.get("messages", []):
                content = message.get("content", [])
                if not isinstance(content, list):
                    continue
                for block in content:
                    if block.get("type") != "tool_result":
                        continue
                    output = str(block.get("content", ""))
                    exchange.tool_outputs.append(
                        CapturedToolOutput(
                            call_id=block.get("tool_use_id"),
                            characters=len(output),
                            truncated=("model-facing limit" in output),
                            head=output[:500],
                            tail=output[-200:],
                        )
                    )
            self.exchanges.append(exchange)
            if self.fault_model == exchange.model and path.partition("?")[
                0
            ].endswith("/messages"):
                self.primary_calls += 1
                if self.primary_calls > 1:
                    exchange.injected = True
                    exchange.status_code = 404
                    writer.write(
                        b"HTTP/1.1 404 Not Found\r\nContent-Type:"
                        b" application/json\r\nConnection:"
                        b" close\r\n\r\n"
                        + to_json(
                            {
                                "type": "error",
                                "error": {
                                    "type": "not_found_error",
                                    "message": (
                                        "Probe-injected model unavailable"
                                    ),
                                },
                            }
                        )
                    )
                    await writer.drain()
                    return
            forwarding = {
                k: v
                for k, v in headers.items()
                if k.lower() not in {"host", "content-length", "connection"}
            }
            async with httpx.AsyncClient(timeout=60) as client:
                async with client.stream(
                    method,
                    (
                        "https://api.anthropic.com"
                        if self.anthropic
                        else "https://api.openai.com"
                    )
                    + path,
                    headers=forwarding,
                    content=body,
                ) as response:
                    exchange.status_code = response.status_code
                    content_type = response.headers.get(
                        "content-type", "application/json"
                    )
                    writer.write(
                        f"HTTP/1.1 {response.status_code} OK\r\nContent-Type:"
                        f" {content_type}\r\nConnection:"
                        " close\r\n\r\n".encode()
                    )
                    pending = b""
                    message_usage = {}
                    async for chunk in response.aiter_bytes():
                        writer.write(chunk)
                        await writer.drain()
                        pending += chunk
                        while b"\n" in pending:
                            line, pending = pending.split(b"\n", 1)
                            if (
                                line.startswith(b"data: ")
                                and line[6:] != b"[DONE]"
                            ):
                                event = from_json(line[6:])
                                if event.get("type") == "message_start":
                                    message_usage = dict(
                                        event["message"]["usage"]
                                    )
                                elif event.get("type") == "message_delta":
                                    message_usage.update(
                                        event.get("usage") or {}
                                    )
                                elif (
                                    event.get("type") == "message_stop"
                                    and message_usage
                                ):
                                    exchange.usage.append(
                                        ProviderUsageSnapshot(**message_usage)
                                    )
                                usage = (event.get("response") or {}).get(
                                    "usage"
                                )
                                if usage:
                                    exchange.usage.append(
                                        ProviderUsageSnapshot.model_validate(
                                            usage
                                        )
                                    )
                    if (
                        pending
                        and "text/event-stream"
                        not in response.headers.get("content-type", "")
                    ):
                        result = from_json(pending)
                        if result.get("usage"):
                            exchange.usage.append(
                                ProviderUsageSnapshot.model_validate(
                                    result["usage"]
                                )
                            )
        except Exception as error:
            self.exchanges.append(CapturedRequest(error=type(error).__name__))
        finally:
            writer.close()
            await writer.wait_closed()
