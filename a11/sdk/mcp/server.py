# Copyright 2026 The A11 Authors.

"""Serving an ActionRegistry to MCP clients.

The other direction from [a11.sdk.mcp.client][a11.sdk.mcp.client]: instead of
reading a server's tools as actions, this publishes actions as tools. Point it
at a registry and every action it holds a handler for becomes a tool an MCP
client can list and call -- so a Claude Desktop, an editor, or any other MCP
host reaches the same actions a model reaches through `interact_with_llm` and a
peer reaches over A11's own wire.

```python
import a11
from a11.sdk import mcp

REGISTRY = a11.ActionRegistry()

@REGISTRY.action
async def summarise(document: str) -> str:
    ...

await mcp.serve_stdio(REGISTRY, name="my-tools")
```

The only module of the serving half that imports the MCP SDK, and it imports it
lazily through [load_mcp][a11.sdk.mcp.client.load_mcp], so
[a11.sdk.mcp.tools][a11.sdk.mcp.tools] and
[a11.sdk.mcp.calls][a11.sdk.mcp.calls] stay importable and testable wherever
A11 runs.

**Which actions.** Everything the registry holds a handler for, narrowed by
full-match patterns in the same shape as `x-a11-allowed-llm-actions`. A11's own
reserved actions are protocol operations rather than tools and are never
served; neither is an action registered for its schema alone, which lives on
some further peer.

**What a client may set.** A `tools/call` carries `_meta`, and
[headers_from_meta][a11.sdk.mcp.calls.headers_from_meta] reads the headers it
asks for. Only the headers the action itself declares are applied, plus
whatever `accept_headers` admits: a client of an MCP server is not the process
owner, and a header it did not declare an interest in is one it should not be
able to set. The deadline is the server's.

**The event loop.** The MCP SDK runs on anyio and A11's Python runtime on
asyncio, which is one loop under the asyncio backend that both `serve_stdio`
and uvicorn use.
"""

from __future__ import annotations

import asyncio
import contextlib
from collections.abc import Mapping, Sequence
from typing import Any

from absl import logging

import a11
from a11.sdk.llm import action_name_matches_allowed
from a11.sdk.mcp.calls import DEFAULT_DEADLINE, call_action, headers_from_meta
from a11.sdk.mcp.client import load_mcp
from a11.sdk.mcp.schemas import McpHeaders
from a11.sdk.mcp.tools import ALL_ACTIONS, ActionTool, tools_from_registry

#: Where a Streamable HTTP server listens when it is told nothing else.
DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 8013
DEFAULT_PATH = "/mcp"

#: JSON-RPC "invalid params", for a tool this server does not serve.
_INVALID_PARAMS = -32602


class McpActionServer:
    """One registry, served as MCP tools.

    Holds the declarations and answers the two requests a tool server owes a
    client: `tools/list` from
    [tools_from_registry][a11.sdk.mcp.tools.tools_from_registry], and
    `tools/call` from [call_action][a11.sdk.mcp.calls.call_action].
    [server][a11.sdk.mcp.server.McpActionServer.server] is the SDK object the
    transports run, for a caller that wants to mount it in an application of
    their own.
    """

    def __init__(
        self,
        registry: a11.ActionRegistry,
        *,
        name: str = "a11",
        version: str = "",
        instructions: str = "",
        patterns: Sequence[str] = ALL_ACTIONS,
        deadline: a11.Duration = DEFAULT_DEADLINE,
        accept_headers: Sequence[str] = (),
        describe_actions: bool = True,
    ) -> None:
        """Declare a registry's actions as one server's tools.

        Args:
            registry: The actions to serve.
            name: The server's name, as a client sees it.
            version: The server's version.
            instructions: What a client should know about using these tools.
            patterns: Full-match regular expressions naming which actions to
                serve. Everything runnable, by default.
            deadline: How long any one call may take.
            accept_headers: Full-match patterns for headers a client may set
                through `_meta`, on top of the ones each action declares.
            describe_actions: Carry each action's `a11.actions/v1` entry in its
                tool `_meta`, so an A11 client rebuilds the real schema rather
                than deriving one from JSON Schema.
        """
        self._registry = registry
        self._name = name
        self._version = version
        self._instructions = instructions
        self._patterns = list(patterns)
        self._deadline = deadline
        self._accept_headers = list(accept_headers)
        self._describe_actions = describe_actions
        self._tools: dict[str, ActionTool] = {}
        self._server: Any = None
        self.refresh()

    @property
    def registry(self) -> a11.ActionRegistry:
        """The registry these tools come from."""
        return self._registry

    @property
    def tools(self) -> Mapping[str, ActionTool]:
        """The declared tools, by name."""
        return dict(self._tools)

    def refresh(self) -> list[str]:
        """Re-read the registry, and return the tool names now served.

        A registry that gained or lost an action between calls is picked up
        here, and the next `tools/list` sees it.
        """
        self._tools = {
            tool.action_name: tool
            for tool in tools_from_registry(
                self._registry,
                self._patterns,
                describe_action=self._describe_actions,
            )
        }
        return sorted(self._tools)

    @property
    def server(self) -> Any:
        """The `mcp.server.lowlevel.Server` these tools are bound to.

        Built on first use, so constructing an `McpActionServer` needs no MCP
        SDK and only serving does.

        Raises:
            StatusException: FAILED_PRECONDITION when the SDK is not installed.
        """
        if self._server is None:
            load_mcp()
            from mcp.server.lowlevel import Server

            self._server = Server(
                self._name,
                version=self._version,
                instructions=self._instructions or None,
                on_list_tools=self._on_list_tools,
                on_call_tool=self._on_call_tool,
            )
        return self._server

    # --- the two requests a tool server answers ----------------------------

    async def _on_list_tools(self, ctx: Any, params: Any) -> Any:
        from mcp_types import ListToolsResult, Tool

        # Everything at once: a registry is in memory, and the tool list is
        # small enough that a cursor would only add a round trip.
        return ListToolsResult(
            tools=[
                Tool.model_validate(tool.tool, by_name=False)
                for tool in self._tools.values()
            ]
        )

    async def _on_call_tool(self, ctx: Any, params: Any) -> Any:
        from mcp.shared.exceptions import MCPError
        from mcp_types import CallToolResult

        tool = self._tools.get(params.name)
        if tool is None:
            # Not the tool's failure but the request's, so it is a protocol
            # error rather than an `isError` result.
            raise MCPError(
                _INVALID_PARAMS,
                f"{self._name} serves no tool named {params.name!r}.",
            )

        meta = dict(ctx.meta or {})
        result = await call_action(
            self._registry,
            tool,
            params.arguments,
            call_id=str(ctx.request_id or ""),
            deadline=self._deadline,
            headers=self._accepted_headers(tool, headers_from_meta(meta)),
            on_progress=self._progress_reporter(ctx),
        )
        return CallToolResult.model_validate(result, by_name=False)

    # --- what a client is allowed to influence ------------------------------

    def _accepted_headers(
        self, tool: ActionTool, asked: Mapping[str, str]
    ) -> dict[str, str]:
        """The headers of ``asked`` this server lets a client set."""
        declared = {
            str(header.get("name") or "")
            for header in tool.entry.get("headers", ())
        }
        declared.add(McpHeaders.META.value)
        allowed: dict[str, str] = {}
        for name, value in asked.items():
            if name in declared or action_name_matches_allowed(
                name, self._accept_headers
            ):
                allowed[name] = value
            else:
                logging.debug(
                    "the MCP call to %r asked for the header %r, which it may"
                    " not set",
                    tool.action_name,
                    name,
                )
        return allowed

    def _progress_reporter(self, ctx: Any) -> Any:
        """Relay narration as progress against this request.

        `report_progress` carries the client's own progress token and is a
        no-op when it sent none, so a server never has to ask whether anybody
        is listening. The narration comes back on the result either way.
        """
        counter = 0

        async def report(text: str) -> None:
            nonlocal counter
            counter += 1
            # A total nobody knows, and a count that only goes up, which is
            # what the spec requires of successive notifications.
            await ctx.session.report_progress(
                float(counter), total=None, message=text
            )

        return report


async def serve_stdio(registry: a11.ActionRegistry, **options: Any) -> None:
    """Serve ``registry`` over stdio, until the client goes away.

    Args:
        registry: The actions to serve.
        **options: Passed to
            [McpActionServer][a11.sdk.mcp.server.McpActionServer].
    """
    await run_stdio(McpActionServer(registry, **options))


async def run_stdio(mcp_server: McpActionServer) -> None:
    """Serve an already-built server over stdio, until the client goes away.

    The transport an MCP host launching a subprocess speaks. It claims the
    process's stdout for the protocol, so anything else this process writes
    goes to stderr while it runs.
    """
    load_mcp()
    from mcp.server.stdio import stdio_server

    async with stdio_server() as (read_stream, write_stream):
        logging.info(
            "serving %d action(s) over MCP stdio", len(mcp_server.tools)
        )
        await mcp_server.server.run(
            read_stream,
            write_stream,
            mcp_server.server.create_initialization_options(),
        )


async def serve_http(
    registry: a11.ActionRegistry,
    *,
    host: str = DEFAULT_HOST,
    port: int = DEFAULT_PORT,
    path: str = DEFAULT_PATH,
    **options: Any,
) -> None:
    """Serve ``registry`` over Streamable HTTP, until the server is stopped.

    Args:
        registry: The actions to serve.
        host: Address to listen on. A loopback address turns on the SDK's DNS
            rebinding protection.
        port: Port to listen on.
        path: Path the MCP endpoint answers at.
        **options: Passed to
            [McpActionServer][a11.sdk.mcp.server.McpActionServer].
    """
    await run_http(
        McpActionServer(registry, **options), host=host, port=port, path=path
    )


async def run_http(
    mcp_server: McpActionServer,
    *,
    host: str = DEFAULT_HOST,
    port: int = DEFAULT_PORT,
    path: str = DEFAULT_PATH,
) -> None:
    """Serve an already-built server over Streamable HTTP, on uvicorn.

    Cancelling this asks uvicorn to shut down and waits for it, so a stopped
    command drains its connections and runs the app's lifespan down rather than
    being torn out from under them.
    """
    app = http_app(mcp_server, host=host, path=path)
    import uvicorn

    logging.info(
        "serving %d action(s) over MCP at http://%s:%d%s",
        len(mcp_server.tools),
        host,
        port,
        path,
    )
    config = uvicorn.Config(app, host=host, port=port, log_level="info")
    server = uvicorn.Server(config)
    serving = asyncio.ensure_future(server.serve())
    try:
        await asyncio.shield(serving)
    except asyncio.CancelledError:
        server.should_exit = True
        with contextlib.suppress(Exception):
            await serving
        raise


def http_app(
    mcp_server: McpActionServer,
    *,
    host: str = DEFAULT_HOST,
    path: str = DEFAULT_PATH,
) -> Any:
    """The Streamable HTTP ASGI app, for mounting in an application of yours.

    A Starlette application whose lifespan runs the MCP session manager. An
    application mounting it has to run that lifespan -- pass it through as its
    own, or mount the app rather than its routes -- or no session ever starts.
    """
    load_mcp()
    return mcp_server.server.streamable_http_app(
        streamable_http_path=path, host=host
    )


__all__ = [
    "DEFAULT_HOST",
    "DEFAULT_PATH",
    "DEFAULT_PORT",
    "McpActionServer",
    "http_app",
    "run_http",
    "run_stdio",
    "serve_http",
    "serve_stdio",
]
