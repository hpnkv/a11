# Copyright 2026 The A11 Authors.

"""The other half of an MCP tool: the handler that actually calls it.

One handler per discovered tool, built by
[make_handler][a11.sdk.mcp.handlers.make_handler] from the
[McpTool][a11.sdk.mcp.schemas.McpTool] the translation produced. It reads the
action's ports and headers back into a `tools/call`, waits for the result, and
streams it onto the action's outputs -- the same job
[a11.gateway.tool_bridge][a11.gateway.tool_bridge]'s proxy does for a remote A11
peer, against a protocol that is not A11's.

It takes the call as a plain async callable rather than an MCP session, so
nothing here imports the MCP SDK: the SDK's shapes are read by attribute, which
is also what makes the whole path testable against a stub. `client.py` supplies
the real one.

Three translations are worth knowing about, because they are what a caller
observes:

* **The action's deadline is the tool's read timeout.** `x-a11-deadline` bounds
  every port read here and then becomes the MCP call's own timeout, so one
  policy governs the whole nested call rather than each layer inventing its own.
* **Progress notifications become log lines.** MCP's progress notifications are
  narration for whoever is watching, so they go to
  [Action.log][a11.actions.action.Action.log] -- the one output a model can
  never see, because it is not a declared port.
* **A failing tool fails the action.** MCP reports a tool's own failure inside a
  successful response (`isError`) so that a model can self-correct; A11 says the
  same thing with a non-OK status, which is what the LLM tool runner turns back
  into the failure text the model reads
  ([ExecutedActions.error_message][a11.sdk.llm_tools.runner.ExecutedActions.error_message]).
  A call that fails is a call that failed, and reporting it as a successful
  action with sad-looking text in it would hide it from everything but the
  model.
"""

from __future__ import annotations

import asyncio
import json
from collections.abc import Awaitable, Callable, Mapping
from typing import Any, Protocol

from absl import logging

import a11
from a11.actions import ActionHandler
from a11.sdk.mcp.schemas import McpHeaders, McpTool
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
    """The status an MCP failure means, however it reached us.

    An MCP error is duck-typed rather than caught by class, so this module stays
    free of the SDK: what makes an exception an MCP error is that it carries a
    JSON-RPC `code` and a `message`.
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

    An input nobody wrote is left out of the object rather than sent as null: a
    server validates the arguments against its own schema, where an absent
    optional property and an explicit null are rarely the same thing.
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

    # Every port is read concurrently, for the reason
    # [a11.gateway.tool_bridge][a11.gateway.tool_bridge]'s proxy pumps its ports
    # concurrently: a caller writes them in whatever order it likes, and on a
    # transport that applies backpressure, draining one port to the end before
    # starting the next stalls a caller that is busy filling one this side has
    # not begun reading.
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
                # Narration is worth nothing next to the result, and the action
                # may already be finishing when a late notification lands.
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
            # Raised before anything is written: the failure is the result, and
            # the runtime propagates it onto every output port, so a reader
            # learns why rather than reading an empty stream.
            detail = "".join(texts).strip() or "the server gave no detail"
            raise Status(
                code=StatusCode.INTERNAL,
                message=f"The MCP tool {tool_name!r} failed: {detail}",
            ).to_exception()

        await _write_stream(action[tool.text_output], texts)
        await _write_stream(
            action[tool.content_output],
            [_block_document(block) for block in others],
        )
        if tool.structured_output is not None:
            structured = getattr(result, "structured_content", None)
            node = action[tool.structured_output]
            # Closed rather than finalized when the server sent nothing: a null
            # terminator would be a value to anything reading the port whole.
            if structured is None:
                await node.close()
            else:
                await node.finalize(structured)

        await action.log(_log_summary(tool_name, texts, others))

    return handle


__all__ = ["McpCall", "make_handler"]
