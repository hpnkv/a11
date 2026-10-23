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

"""Serve the current Codex turn's tools over authenticated loopback MCP."""

import asyncio
import contextlib
import secrets
import socket
from collections.abc import AsyncIterator, Awaitable, Callable
from typing import Any

from a11.sdk.mcp.client import load_mcp

ToolHandler = Callable[[dict[str, Any], str], Awaitable[dict[str, Any]]]
SERVER_NAME = "a11_actions"


@contextlib.asynccontextmanager
async def serve_tools(
    tools: list[dict[str, Any]],
    handler: ToolHandler,
    root_thread: Callable[[], str],
    tool_timeout: float | None = None,
) -> AsyncIterator[tuple[dict[str, Any], dict[str, str]]]:
    """Serve per-tool schemas and dispatch calls through the turn's handler."""
    if not tools:
        yield {}, {}
        return
    load_mcp()
    from mcp.server.lowlevel import Server
    from mcp.shared.exceptions import MCPError
    from mcp_types import CallToolResult, ListToolsResult, Tool
    from starlette.responses import Response
    import uvicorn

    declared = {tool["name"]: Tool.model_validate(tool) for tool in tools}
    calls: set[asyncio.Task[Any]] = set()

    async def list_tools(ctx: Any, params: Any) -> ListToolsResult:
        return ListToolsResult(tools=list(declared.values()))

    async def call_tool(ctx: Any, params: Any) -> CallToolResult:
        if params.name not in declared:
            raise MCPError(-32602, f"Unknown A11 tool: {params.name}")
        meta = dict(ctx.meta or {})
        task = asyncio.current_task()
        assert task is not None
        calls.add(task)
        try:
            result = await handler(
                {
                    "tool": params.name,
                    "callId": meta.get("callId") or secrets.token_hex(16),
                    "arguments": params.arguments or {},
                },
                meta.get("threadId") or root_thread(),
            )
            return CallToolResult.model_validate(result)
        except Exception as error:
            return CallToolResult(
                content=[{"type": "text", "text": str(error)}], isError=True
            )
        finally:
            calls.discard(task)

    sdk_server = Server(
        SERVER_NAME,
        version="1",
        on_list_tools=list_tools,
        on_call_tool=call_tool,
    )
    app = sdk_server.streamable_http_app(
        host="127.0.0.1", stateless_http=True, json_response=True
    )
    token = secrets.token_hex(32)
    authorization = f"Bearer {token}".encode()

    async def authenticated(scope: Any, receive: Any, send: Any) -> None:
        if scope["type"] == "http":
            provided = dict(scope.get("headers") or []).get(
                b"authorization", b""
            )
            if not secrets.compare_digest(provided, authorization):
                await Response(status_code=401)(scope, receive, send)
                return
        await app(scope, receive, send)

    ready = asyncio.Event()

    class LocalServer(uvicorn.Server):
        @contextlib.contextmanager
        def capture_signals(self):
            yield

        async def startup(self, sockets=None):
            await super().startup(sockets=sockets)
            ready.set()

    server = LocalServer(
        uvicorn.Config(
            authenticated,
            host="127.0.0.1",
            log_config=None,
            access_log=False,
            lifespan="on",
            timeout_graceful_shutdown=1,
        )
    )
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.bind(("127.0.0.1", 0))
        listener.setblocking(False)
        serving = asyncio.create_task(server.serve(sockets=[listener]))
        started = asyncio.create_task(ready.wait())
        try:
            await asyncio.wait(
                {serving, started}, return_when=asyncio.FIRST_COMPLETED
            )
            if serving.done():
                serving.result()
                raise RuntimeError("Codex MCP server stopped during startup.")
            env_key = f"A11_CODEX_MCP_TOKEN_{secrets.token_hex(8).upper()}"
            prefix = f"mcp_servers.{SERVER_NAME}"
            yield {
                f"{prefix}.url": (
                    f"http://127.0.0.1:{listener.getsockname()[1]}/mcp"
                ),
                f"{prefix}.bearer_token_env_var": env_key,
                f"{prefix}.supports_parallel_tool_calls": True,
                f"{prefix}.required": True,
                **(
                    {f"{prefix}.tool_timeout_sec": tool_timeout}
                    if tool_timeout is not None
                    else {}
                ),
                **{
                    f"{prefix}.tools.{name}.approval_mode": "approve"
                    for name in declared
                },
            }, {env_key: token}
        finally:
            started.cancel()
            for task in calls:
                task.cancel()
            await asyncio.gather(started, *calls, return_exceptions=True)
            server.should_exit = True
            await serving
