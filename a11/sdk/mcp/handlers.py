# Copyright 2026 The A11 Authors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Call discovered MCP tools through A11 action handlers.

[make_handler][a11.sdk.mcp.handlers.make_handler] reads action ports and
headers, calls the tool, and writes its result to output ports. Its async call
can be supplied by `client.py` or another MCP client.

The action deadline bounds port reads and the MCP call. Progress notifications
become action log entries. An MCP `isError` result becomes a non-OK action
status.
"""

from __future__ import annotations

import asyncio
import base64
import json
from collections.abc import Awaitable, Callable, Mapping
from typing import Any, Protocol

from absl import logging

import a11
from a11.actions import ActionHandler
from a11.sdk.mcp.schemas import McpHeaders, McpMeta, McpTool
from a11.status import Status, StatusCode, StatusException

# JSON-RPC and MCP error codes, mapped to what A11 calls the same failure. MCP
# transports a protocol error as one of these; a tool's *own* failure arrives as
# `isError` on a successful response and is handled separately. Spelled as
# literals because they are wire constants, not an SDK's names for them: this
# module reads an MCP failure without importing MCP.
_ERROR_CODES = {
    # Standard JSON-RPC.
    -32700: StatusCode.INVALID_ARGUMENT,  # parse error
    -32600: StatusCode.INVALID_ARGUMENT,  # invalid request
    -32601: StatusCode.NOT_FOUND,  # method not found
    -32602: StatusCode.INVALID_ARGUMENT,  # invalid params
    -32603: StatusCode.INTERNAL,  # internal error
    # MCP's own, from the spec's server-error range.
    -32000: StatusCode.UNAVAILABLE,  # connection closed
    -32001: StatusCode.DEADLINE_EXCEEDED,  # request timed out
    -32020: StatusCode.INVALID_ARGUMENT,  # header mismatch
    -32021: StatusCode.FAILED_PRECONDITION,  # missing client capability
    -32022: StatusCode.FAILED_PRECONDITION,  # unsupported protocol version
    -32042: StatusCode.FAILED_PRECONDITION,  # URL elicitation required
}

#: How much of a tool result a log line quotes.
_LOG_EXCERPT_LIMIT = 320


class McpCall(Protocol):
    """Sending one `tools/call` and getting its result.

    The whole of what a handler needs from an MCP session, which is why it is
    spelled as a callable: `client.py` binds a real client to it, and a test
    binds a function.
    """

    async def __call__(
        self,
        name: str,
        arguments: dict[str, Any] | None,
        *,
        read_timeout_seconds: float | None = None,
        progress_callback: (
            Callable[[float, float | None, str | None], Awaitable[None]] | None
        ) = None,
        meta: Mapping[str, Any] | None = None,
    ) -> Any: ...


def _block_type(block: Any) -> str:
    """A content block's `type`, however it arrived."""
    if isinstance(block, Mapping):
        return str(block.get("type", ""))
    return str(getattr(block, "type", ""))


def _block_document(block: Any) -> Any:
    """One content block as JSON, keeping the wire's own field names."""
    dump = getattr(block, "model_dump", None)
    if callable(dump):
        return dump(mode="json", by_alias=True, exclude_none=True)
    return block


def _status_of(error: BaseException, tool_name: str) -> Status:
    """Map an MCP failure to a status by its JSON-RPC code and message.

    Accepts SDK exceptions without importing their classes.
    """
    if isinstance(error, StatusException):
        return error.status
    if isinstance(error, TimeoutError):
        return Status(
            code=StatusCode.DEADLINE_EXCEEDED,
            message=f"The MCP tool {tool_name!r} did not answer in time.",
        )
    code = getattr(error, "code", None)
    message = getattr(error, "message", None)
    if isinstance(code, int) and isinstance(message, str):
        return Status(
            code=_ERROR_CODES.get(code, StatusCode.UNKNOWN),
            message=(
                f"The MCP call to {tool_name!r} failed with error {code}:"
                f" {message}"
            ),
        )
    return Status(
        code=StatusCode.UNKNOWN,
        message=f"Calling the MCP tool {tool_name!r} failed: {error}",
    )


def _request_meta(action: a11.Action) -> dict[str, Any] | None:
    """The `_meta` this call carries, from the header that holds it."""
    raw = action.get_header(McpHeaders.META.value, decode=True)
    if not raw:
        return None
    try:
        meta = json.loads(raw)
    except ValueError as error:
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message=(
                f"The {McpHeaders.META.value} header must be a JSON object:"
                f" {error}"
            ),
        ).to_exception() from error
    if not isinstance(meta, dict):
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message=(
                f"The {McpHeaders.META.value} header must be a JSON object."
            ),
        ).to_exception()
    return meta


def _check_server(action: a11.Action, tool: McpTool) -> None:
    """Refuse a call aimed at a server this handler cannot reach.

    The handler is bound to one connection. A caller that overrides the server
    header has asked for something this action cannot do, and calling the wrong
    server instead is the one outcome nobody wants.
    """
    requested = action.get_header(McpHeaders.SERVER.value, decode=True)
    if not requested or not tool.server or requested == tool.server:
        return
    raise Status(
        code=StatusCode.INVALID_ARGUMENT,
        message=(
            f"This action calls the MCP server {tool.server!r}, but the"
            f" {McpHeaders.SERVER.value} header asks for {requested!r}."
        ),
    ).to_exception()


async def _read_port(
    node: a11.AsyncNode, unary: bool, timeout: a11.Duration
) -> tuple[bool, Any]:
    """One argument's value, and whether it was supplied at all.

    Read every port as a stream because the fragment encoding does not
    distinguish a unary list from several values. `ActionCallAdapter` writes one
    fragment per list element, so this function reassembles unary lists. A
    streaming port always returns a list; a unary port returns one value or the
    reassembled list.
    """
    values = [value async for value in node.iter_values(timeout=timeout)]
    if not values:
        return False, None
    if unary and len(values) == 1:
        return True, values[0]
    return True, values


async def _read_arguments(
    action: a11.Action, tool: McpTool, timeout: Callable[[], a11.Duration]
) -> dict[str, Any] | None:
    """The `arguments` object for the call, read off the action's inputs.

    Omit unwritten optional properties; an explicit null remains a value.
    """
    if tool.whole_arguments is not None:
        supplied, value = await _read_port(
            action[tool.whole_arguments], True, timeout()
        )
        if not supplied:
            return None
        if not isinstance(value, dict):
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message=(
                    f"The {tool.whole_arguments} input of"
                    f" {tool.schema.name} must be a JSON object."
                ),
            ).to_exception()
        return value

    # Read ports concurrently to avoid backpressure between input streams.
    read = await asyncio.gather(
        *[
            _read_port(action[argument.port], argument.unary, timeout())
            for argument in tool.arguments
        ]
    )
    arguments: dict[str, Any] = {}
    for argument, (supplied, value) in zip(tool.arguments, read):
        if supplied:
            arguments[argument.property] = value
    return arguments or None


async def _write_stream(node: a11.AsyncNode, values: list[Any]) -> None:
    """Write a known-complete sequence, then close.

    The last value carries finality, which is what tells a whole-value consumer
    the sequence ran to its end; the node is closed rather than finalized so no
    null terminator is appended, for the reason
    `a11.sdk.bash.handlers._write_all_final` gives.
    """
    for index, value in enumerate(values):
        await node.put(value, final=index == len(values) - 1)
    await node.close()


def _block_port(block: Any) -> str | None:
    """The output port an A11 server said a content block came from."""
    meta = getattr(block, "meta", None)
    if meta is None and isinstance(block, Mapping):
        meta = block.get("_meta")
    if isinstance(meta, Mapping):
        port = meta.get(McpMeta.PORT.value)
        if isinstance(port, str) and port:
            return port
    return None


def _block_payload(block: Any) -> Any:
    """A content block as the value its port carried.

    A picture and a sound travel base64-encoded, so they go back on the port as
    the bytes they were. Anything else stays the block it arrived as.
    """
    data = getattr(block, "data", None)
    if data is None and isinstance(block, Mapping):
        data = block.get("data")
    if isinstance(data, str):
        try:
            return base64.b64decode(data)
        except ValueError:
            logging.debug("undecodable content block data", exc_info=True)
    return _block_document(block)


async def _write_value(action: a11.Action, port: str, value: Any) -> None:
    """One output field onto its port, as a value or as a sequence."""
    schema = action.get_schema().outputs[port]
    node = action[port]
    if schema.unary:
        await node.finalize(value)
        return
    await _write_stream(
        node, list(value) if isinstance(value, list) else [value]
    )


async def _write_described(
    action: a11.Action, tool: McpTool, result: Any, texts: list[str]
) -> None:
    """Put an A11 server's result back on the ports the action declares.

    The server derived this tool from an `ActionSchema` and said so in the
    tool's `_meta`, so the result is that action's outputs rather than MCP's
    three: `structuredContent` holds one field per declared output, and a
    picture or a sound arrived as the content block MCP has for it, tagged with
    the port it came from.
    """
    payload = getattr(result, "structured_content", None)
    written: set[str] = set()

    for block in getattr(result, "content", None) or []:
        port = _block_port(block)
        if port is None or port not in action.get_schema().outputs:
            continue
        await _write_value(action, port, _block_payload(block))
        written.add(port)

    if tool.whole_json_output is not None:
        port = tool.whole_json_output
        if port not in written:
            # A result MCP could not carry as a structured document -- a string,
            # a number -- came back as text.
            value = payload if payload is not None else "".join(texts)
            await _write_value(action, port, value)
            written.add(port)
    elif isinstance(payload, Mapping):
        for field, port in tool.structured_fields:
            if port in written or field not in payload:
                continue
            await _write_value(action, port, payload[field])
            written.add(port)

    for port in action.get_schema().outputs:
        if port not in written:
            # Close unwritten ports so whole-value reads end without a null.
            await action[port].close()


def _log_summary(tool_name: str, texts: list[Any], others: list[Any]) -> str:
    """One line about what the tool returned, for the person watching."""
    parts = []
    if texts:
        text = "".join(str(value) for value in texts)
        excerpt = text.strip()
        if len(excerpt) > _LOG_EXCERPT_LIMIT:
            excerpt = excerpt[: _LOG_EXCERPT_LIMIT - 1] + "…"
        parts.append(f"```\n{excerpt}\n```")
    if others:
        kinds = ", ".join(
            sorted({_block_type(block) or "?" for block in others})
        )
        parts.append(f"{len(others)} non-text block(s): {kinds}.")
    summary = f"Called the MCP tool `{tool_name}`."
    return "\n\n".join([summary, *parts])


def make_handler(tool: McpTool, call: McpCall) -> ActionHandler:
    """Build the Action handler that calls ``tool``.

    Args:
        tool: The translated tool, which says which ports carry which argument
            and where the result goes.
        call: How to send one `tools/call` -- normally
            [McpToolset.call][a11.sdk.mcp.client.McpToolset.call].

    Returns:
        An [ActionHandler][a11.actions.action.ActionHandler] to register under
        ``tool.schema.name``.
    """

    async def handle(action: a11.Action) -> None:
        deadline = a11.get_deadline(action)

        def remaining() -> a11.Duration:
            # Every read is bounded, so a caller that neither writes nor closes
            # an optional input cannot hang the call. The tool runner always
            # closes the inputs it fed, so a present or absent input normally
            # resolves at once.
            return max(deadline - a11.now(), a11.zero_duration())

        _check_server(action, tool)
        tool_name = (
            action.get_header(McpHeaders.TOOL.value, decode=True)
            or tool.tool_name
        )
        meta = _request_meta(action)
        arguments = await _read_arguments(action, tool, remaining)

        left = remaining()
        read_timeout = None if left.is_infinite() else left.float_seconds()

        async def on_progress(
            progress: float, total: float | None, message: str | None
        ) -> None:
            done = f"{progress:g}/{total:g}" if total else f"{progress:g}"
            text = f"{message} ({done})" if message else done
            try:
                await action.log(f"`{tool_name}`: {text}")
            except Exception:
                # A late progress notification must not fail the tool result.
                logging.debug("could not log MCP progress", exc_info=True)

        try:
            result = await call(
                tool_name,
                arguments,
                read_timeout_seconds=read_timeout,
                progress_callback=on_progress,
                meta=meta,
            )
        except Exception as error:
            raise _status_of(error, tool_name).to_exception() from error

        blocks = list(getattr(result, "content", None) or [])
        texts = [
            str(getattr(block, "text", "") or "")
            for block in blocks
            if _block_type(block) == "text"
        ]
        others = [block for block in blocks if _block_type(block) != "text"]

        if getattr(result, "is_error", False):
            # Raise before writing; the runtime propagates the failure to ports.
            detail = "".join(texts).strip() or "the server gave no detail"
            raise Status(
                code=StatusCode.INTERNAL,
                message=f"The MCP tool {tool_name!r} failed: {detail}",
            ).to_exception()

        if tool.described is not None:
            await _write_described(action, tool, result, texts)
        else:
            await _write_stream(action[tool.text_output], texts)
            await _write_stream(
                action[tool.content_output],
                [_block_document(block) for block in others],
            )
            if tool.structured_output is not None:
                structured = getattr(result, "structured_content", None)
                node = action[tool.structured_output]
                # Close absent results so whole-value reads end without a null.
                if structured is None:
                    await node.close()
                else:
                    await node.finalize(structured)

        await action.log(_log_summary(tool_name, texts, others))

    return handle


__all__ = ["McpCall", "make_handler"]
