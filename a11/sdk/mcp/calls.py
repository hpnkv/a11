# Copyright 2026 The A11 Authors.

"""One `tools/call`, answered by running an action.

The mirror of [a11.sdk.mcp.handlers][a11.sdk.mcp.handlers], and like it free of
the MCP SDK: arguments arrive as the plain mapping the wire carried, and the
result goes back as the `CallToolResult` document
[a11.sdk.mcp.server][a11.sdk.mcp.server] validates into the SDK's model. That
keeps the whole path testable without a server, and lets the same translation
answer a call that arrived some other way.

Each half of the crossing reuses what the LLM tool runner already does with a
model's tool call, because it is the same crossing:

* **Arguments become input fragments** through
  [ActionCallAdapter][a11.sdk.llm.ActionCallAdapter], which validates them
  against the schema -- an argument the action does not declare, a required one
  that is missing, an autofilled input a caller may not write -- and encodes a
  list argument as one fragment per element, which is what a streaming port
  reads back as a sequence.
* **Outputs become the result** through
  [collect_action_outputs][a11.sdk.llm_tools.runner.collect_action_outputs] and
  [decode_action_output_fragments][a11.sdk.llm.decode_action_output_fragments],
  so what an MCP client reads and what a model reads through
  `interact_with_llm` are the same rendition of the same run.

Three translations are what a caller observes:

* **Narration becomes progress.** What an action writes to
  [Action.log][a11.actions.action.Action.log] is relayed as it happens, which
  MCP carries as `notifications/progress` with a message, and comes back whole
  on the result's `_meta` for a client that asked for no notifications.
* **MCP's `_meta` becomes headers.** See
  [headers_from_meta][a11.sdk.mcp.calls.headers_from_meta].
* **A failing action fails the tool.** A non-OK status becomes `isError` with
  the failure text, which is what MCP gives a model to correct itself with, and
  the inverse of what `handlers` does with `isError` coming the other way.
"""

from __future__ import annotations

import asyncio
import base64
import contextlib
import json
from collections.abc import Awaitable, Callable, Mapping
from typing import Any

from absl import logging

import a11
from a11.sdk.llm import (
    ActionCallAdapter,
    ToolCall,
    decode_action_output_fragments,
    decoded_output_text,
)
from a11.sdk.llm_tools.runner import (
    DRAIN_AFTER_COMPLETION,
    collect_action_outputs,
    feed_action_inputs,
    user_facing_log_entries,
)
from a11.sdk.mcp.schemas import McpHeaders, McpMeta
from a11.sdk.mcp.tools import ActionTool
from a11.status import Status, StatusCode, StatusException

#: `_meta` keys the SDK owns, which never become headers.
RESERVED_META_PREFIX = "io.modelcontextprotocol/"

#: `_meta` keys MCP itself defines on a request.
RESERVED_META_KEYS = frozenset({"progressToken", "progress_token"})

#: What a call is given when the server states no deadline of its own.
DEFAULT_DEADLINE = a11.Duration.seconds(300)

#: Relayed narration, for a caller that asked for no progress notifications.
LOG_META = "to.a11/log"


def headers_from_meta(meta: Mapping[str, Any] | None) -> dict[str, str]:
    """The action headers one call's `_meta` asks for.

    MCP has no header on a `tools/call`; it has `_meta`, an open map. A11 has
    no per-call bag; it has headers, which flow into nested actions. So `_meta`
    is where a client puts what A11 carries in a header, three ways:

    * a `to.a11/headers` object of header name to value, for anything at all;
    * any top-level `x-a11-*` or `x-otel-*` key, spelled as the header itself;
    * the whole of `_meta`, as the `x-a11-mcp-meta` header -- which is the same
      JSON object [a11.sdk.mcp.handlers][a11.sdk.mcp.handlers] sends *out* as
      `_meta`, so metadata survives an A11 -> MCP -> A11 round trip.

    MCP's own reserved keys are left out of all three: the progress token is
    the transport's, and `io.modelcontextprotocol/*` is the SDK's.

    Returns:
        Header name to value. What of it is actually applied is the server's
        decision -- see
        [McpActionServer][a11.sdk.mcp.server.McpActionServer].
    """
    if not meta:
        return {}
    carried = {
        key: value
        for key, value in meta.items()
        if key not in RESERVED_META_KEYS
        and not key.startswith(RESERVED_META_PREFIX)
        and key != McpMeta.HEADERS.value
    }

    headers: dict[str, str] = {}
    if carried:
        headers[McpHeaders.META.value] = json.dumps(carried, sort_keys=True)
    for key, value in carried.items():
        if key.startswith("x-a11-") or key.startswith("x-otel-"):
            headers[key] = (
                value if isinstance(value, str) else json.dumps(value)
            )

    asked = meta.get(McpMeta.HEADERS.value)
    if isinstance(asked, Mapping):
        for key, value in asked.items():
            headers[str(key)] = (
                value if isinstance(value, str) else json.dumps(value)
            )
    return headers


def _failure_text(status: Status) -> str:
    """A failed call as the line a model reads, the way the runner spells it."""
    if not status.message:
        return status.code.name
    return f"{status.code.name}: {status.message}"


def _status_of(error: BaseException) -> Status:
    """The status an action failed with, however the failure reached us."""
    if isinstance(error, StatusException):
        return error.status
    if isinstance(error, asyncio.TimeoutError):
        return Status(
            code=StatusCode.DEADLINE_EXCEEDED, message="The action timed out."
        )
    return Status(code=StatusCode.UNKNOWN, message=str(error))


def _media_block(port: str, mimetype: str, value: Any) -> dict[str, Any]:
    """One picture or sound as the content block MCP has for it."""
    if isinstance(value, (bytes, bytearray)):
        data = base64.b64encode(bytes(value)).decode()
    else:
        data = base64.b64encode(str(value).encode()).decode()
    kind = "image" if mimetype.startswith("image/") else "audio"
    return {
        "type": kind,
        "data": data,
        "mimeType": mimetype,
        # Which port wrote it, so an A11 client can put the bytes back where
        # they came from rather than reading a base64 string.
        "_meta": {McpMeta.PORT.value: port},
    }


def _as_declared(tool: ActionTool, payload: Any) -> Any:
    """The decoded result, with each declared sequence spelled as one.

    A streaming port that carried a single value decodes to that value, which
    is not what the tool's output schema promised for it.
    """
    if not tool.sequences or not isinstance(payload, Mapping):
        return payload
    return {
        field: (
            value
            if field not in tool.sequences or isinstance(value, list)
            else [value]
        )
        for field, value in payload.items()
    }


def _split_media(
    tool: ActionTool, fragments: list[a11.NodeFragment]
) -> tuple[list[dict[str, Any]], list[a11.NodeFragment]]:
    """The media fragments as content blocks, and everything else untouched."""
    if not tool.media:
        return [], fragments
    blocks: list[dict[str, Any]] = []
    rest: list[a11.NodeFragment] = []
    for fragment in fragments:
        mimetype = tool.media.get(fragment.id)
        if mimetype is None:
            rest.append(fragment)
            continue
        chunk = fragment.get_chunk()
        if chunk.is_null():
            continue
        blocks.append(
            _media_block(fragment.id, mimetype, a11.from_chunk(chunk))
        )
    return blocks, rest


async def _relay_narration(
    node: a11.AsyncNode,
    timeout: a11.Duration,
    on_progress: Callable[[str], Awaitable[None]] | None,
    parts: list[str],
) -> None:
    """Send each log entry on as it lands, collecting it into ``parts``.

    The caller owns ``parts`` so that giving up on a log port that never ends
    still returns what arrived on it.
    """
    async for text in user_facing_log_entries(node, timeout):
        parts.append(text)
        if on_progress is None:
            continue
        try:
            await on_progress(text)
        except Exception:
            # Narration is worth nothing next to the result, and the request
            # may already be finishing when a late line lands.
            logging.debug("could not report MCP progress", exc_info=True)


async def _finish_narrating(relay: Any, until: a11.Time) -> None:
    """Give the log port a bounded moment to end after the action has.

    A handler in this process closes its log port by finishing. A proxy for one
    somewhere else is under no such obligation, so the wait is capped the same
    way the tool runner caps its own drain.
    """
    remaining = max(until - a11.now(), a11.zero_duration())
    grace = min(remaining, DRAIN_AFTER_COMPLETION)
    try:
        await asyncio.wait_for(relay, grace.float_seconds())
    except Exception:
        relay.cancel()
        with contextlib.suppress(asyncio.CancelledError, Exception):
            await relay


async def call_action(
    registry: a11.ActionRegistry,
    tool: ActionTool,
    arguments: Mapping[str, Any] | None = None,
    *,
    call_id: str = "",
    deadline: a11.Duration = DEFAULT_DEADLINE,
    headers: Mapping[str, str] | None = None,
    on_progress: Callable[[str], Awaitable[None]] | None = None,
) -> dict[str, Any]:
    """Run one action for a `tools/call`, and answer with its result.

    Args:
        registry: Where the action and its handler live.
        tool: The declaration the client is calling, from
            [tools_from_registry][a11.sdk.mcp.tools.tools_from_registry].
        arguments: The call's `arguments` object.
        call_id: The action's id, normally the JSON-RPC request id, so a log
            line and a trace name the call the client made.
        deadline: How long the action has. It becomes `x-a11-deadline`, so one
            policy bounds the handler, everything it calls, and the drain of
            what it wrote.
        headers: Applied to the action before it runs, on top of the deadline.
        on_progress: Called with each line the action narrates, while it runs.

    Returns:
        A `CallToolResult` document. A failing action is `isError` with the
        failure text rather than a raised exception: the client asked for a
        tool result, and MCP's way of saying a tool failed is this.
    """
    until = a11.now() + deadline
    try:
        adapter = ActionCallAdapter.create(
            ToolCall(
                name=tool.action_name,
                id=call_id,
                params=dict(arguments or {}),
            ),
            registry.get_schema(tool.action_name),
        )
        fragments = await adapter.get_action_inputs()
    except Exception as error:
        # A malformed call is the model's to fix, so it is a tool result rather
        # than a protocol error.
        return {
            "content": [
                {"type": "text", "text": _failure_text(_status_of(error))}
            ],
            "isError": True,
        }

    action = registry.make_action(tool.action_name, call_id)
    a11.set_deadline_header(action, until)
    for name, value in (headers or {}).items():
        action.set_header(name, value.encode())
    # Claimed before the run, for the reason the tool runner claims it: the
    # first line a handler writes has to arrive here rather than on the process
    # sink.
    log_node = action.get_log_node()

    failure: Status | None = None
    narrated: list[str] = []
    try:
        action.run()
        await feed_action_inputs(action, fragments)
        relay = asyncio.ensure_future(
            _relay_narration(log_node, deadline, on_progress, narrated)
        )
        try:
            await action.wait(max(until - a11.now(), a11.zero_duration()))
        except Exception as error:
            failure = _status_of(error)
        await _finish_narrating(relay, until)
    except Exception as error:
        failure = failure or _status_of(error)

    fragments_out, drained = await collect_action_outputs(action, until)
    failure = failure or drained

    if failure is not None:
        return {
            "content": [{"type": "text", "text": _failure_text(failure)}],
            "isError": True,
        }

    blocks, rest = _split_media(tool, fragments_out)
    payload = _as_declared(tool, decode_action_output_fragments(rest))
    content: list[dict[str, Any]] = []
    if rest:
        # The serialized result alongside the structured one, which is what a
        # client that reads only content blocks needs.
        content.append(
            {"type": "text", "text": await decoded_output_text(rest)}
        )
    content.extend(blocks)

    result: dict[str, Any] = {"content": content}
    if tool.structured and isinstance(payload, Mapping):
        result["structuredContent"] = dict(payload)
    if narration := "".join(narrated):
        result["_meta"] = {LOG_META: narration}
    return result


__all__ = [
    "DEFAULT_DEADLINE",
    "LOG_META",
    "RESERVED_META_KEYS",
    "RESERVED_META_PREFIX",
    "call_action",
    "headers_from_meta",
]
