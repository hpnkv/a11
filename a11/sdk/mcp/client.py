# Copyright 2026 The A11 Authors.

"""Reaching an MCP server, and turning what it serves into an ActionRegistry.

The only module here that imports the MCP SDK, and it imports it lazily, the way
[a11.sdk.interact_with_llm][a11.sdk.interact_with_llm] imports a provider's --
so `a11.sdk.mcp` can be imported, and a schema translated, wherever A11 runs.

Discovery is the same shape as asking an A11 peer what it serves
([a11.client.discovery][a11.client.discovery]): list, translate, register a
schema and a handler per answer. What differs is that MCP has no equivalent of
A11's `__list_actions__` answer -- there is no `ActionSchema` on the wire, only
a JSON Schema per tool -- which is what
[a11.sdk.mcp.schemas][a11.sdk.mcp.schemas] exists to bridge.

```python
from a11.sdk import mcp

async with mcp.connect("https://example.com/mcp") as toolset:
    result = await toolset.action("search").run()["text"].consume(str)
```

The registry `toolset.registry` holds every tool as an ordinary Action, so it
can be handed to `interact_with_llm` as the tools a model may call, served to a
peer, or called from a flow -- nothing downstream knows MCP is involved.

**Lifetime.** `connect` is an async context manager because the MCP SDK's client
is one: its transport, its session and its receive loop live for the duration of
a `with`, and entering and leaving in the same task is what keeps the SDK's
cancel scopes intact. A long-lived host should hold the `with` open in the task
that owns it (an `AsyncExitStack` at startup, say) rather than try to enter and
exit it from different places. A caller that already manages its own
`mcp.Client` builds an [McpToolset][a11.sdk.mcp.client.McpToolset] on it
directly.

**HTTP headers and authorization** belong to the connection rather than to a
call, so they are the SDK's business, not A11's: build the transport with an
HTTP client that carries them and hand that to `connect`.

```python
import httpx2
from mcp.client.streamable_http import streamable_http_client

http = httpx2.AsyncClient(headers={"Authorization": f"Bearer {token}"})
url = "https://example.com/mcp"
transport = streamable_http_client(url, http_client=http)
async with mcp.connect(transport, server_label=url) as toolset:
    ...
```
"""

from __future__ import annotations

import contextlib
from collections.abc import AsyncIterator, Collection, Mapping, Sequence
from typing import Any

from absl import logging

import a11
from a11.sdk.mcp.handlers import make_handler
from a11.sdk.mcp.schemas import McpTool, action_schema_from_tool
from a11.status import Status, StatusCode

#: What to install to use this module.
INSTALL_HINT = "pip install 'a11-kit[mcp]'"

# A server that answers `tools/list` with a cursor that never advances would
# otherwise page for ever. The bound is generous -- a hundred pages of tools is
# already far past what a model can be shown -- and hitting it is logged.
_MAX_TOOL_PAGES = 100


def load_mcp() -> Any:
    """Import the MCP SDK, or say what to install.

    Raises:
        StatusException: FAILED_PRECONDITION when the SDK is missing or will
            not import.
    """
    try:
        import mcp
    except ImportError as error:
        raise Status(
            code=StatusCode.FAILED_PRECONDITION,
            message=(
                "The MCP adapter needs the Model Context Protocol SDK. Install"
                f" it with:  {INSTALL_HINT}"
            ),
        ).to_exception() from error
    except Exception as error:  # noqa: BLE001 - a broken SDK is a precondition
        raise Status(
            code=StatusCode.FAILED_PRECONDITION,
            message=(
                f"The MCP SDK failed to import: {type(error).__name__}: {error}"
            ),
        ).to_exception() from error
    return mcp


def server_name(server: Any) -> str:
    """How to name a connection target in a schema, for a person reading it.

    A URL names itself; a subprocess is named by its command line; an
    in-process server by its own name. Anything else -- a transport a caller
    built -- has no name to take, so pass one to
    [connect][a11.sdk.mcp.client.connect].
    """
    if isinstance(server, str):
        return server
    command = getattr(server, "command", None)
    if isinstance(command, str):
        arguments = getattr(server, "args", None) or []
        return " ".join(["stdio:" + command, *(str(a) for a in arguments)])
    name = getattr(server, "name", None)
    if isinstance(name, str) and name:
        return name
    return type(server).__name__


class McpToolset:
    """The tools one MCP server serves, as A11 Actions.

    Built by [connect][a11.sdk.mcp.client.connect], or directly on an
    `mcp.Client` a caller entered itself. It owns the translation
    (`{action name: McpTool}`), the registry the actions went onto, and the call
    every handler goes through.
    """

    def __init__(
        self,
        client: Any,
        *,
        server: str = "",
        registry: a11.ActionRegistry | None = None,
        prefix: str = "",
    ) -> None:
        """Bind a toolset to an entered MCP client.

        Args:
            client: An `mcp.Client` inside its `async with`.
            server: How to name the server in the derived schemas; defaults to
                whatever [server_name][a11.sdk.mcp.client.server_name] makes of
                the client's target.
            registry: Where the actions are registered; a new one by default.
            prefix: Prepended to every derived action name, to keep two
                servers' tools apart in one registry.
        """
        self._client = client
        self._server = server or server_name(getattr(client, "server", client))
        self._registry = (
            registry if registry is not None else a11.ActionRegistry()
        )
        self._prefix = prefix
        self._tools: dict[str, McpTool] = {}

    @property
    def client(self) -> Any:
        """The underlying `mcp.Client`, for everything MCP does that A11 does
        not: resources, prompts, subscriptions."""
        return self._client

    @property
    def server(self) -> str:
        """The server these actions call, as it appears in their schemas."""
        return self._server

    @property
    def registry(self) -> a11.ActionRegistry:
        """The registry the tools are registered on."""
        return self._registry

    @property
    def tools(self) -> Mapping[str, McpTool]:
        """The translated tools, by the action name each is registered under."""
        return dict(self._tools)

    def action(self, name: str) -> a11.Action:
        """A ready-to-run Action for one registered tool.

        The short way to call a tool directly -- `await toolset.action("search")
        .run()` -- without going through a registry lookup or a model.

        Raises:
            StatusException: NOT_FOUND when no such tool was registered.
        """
        tool = self._tools.get(name)
        if tool is None:
            raise Status(
                code=StatusCode.NOT_FOUND,
                message=(
                    f"No MCP tool is registered as {name!r} for"
                    f" {self._server!r}."
                ),
            ).to_exception()
        return (
            a11.Action(tool.schema)
            .bind_registry(self._registry)
            .bind_handler(self._registry.get_handler(name))
        )

    async def call(
        self,
        name: str,
        arguments: dict[str, Any] | None = None,
        *,
        read_timeout_seconds: float | None = None,
        progress_callback: Any = None,
        meta: Mapping[str, Any] | None = None,
    ) -> Any:
        """Send one `tools/call` to the server.

        The [McpCall][a11.sdk.mcp.handlers.McpCall] every handler is built
        against; also usable on its own, for a caller that wants the MCP result
        rather than an Action's ports.
        """
        return await self._client.call_tool(
            name,
            arguments,
            read_timeout_seconds=read_timeout_seconds,
            progress_callback=progress_callback,
            meta=dict(meta) if meta else None,
        )

    async def discover(
        self, *, taken: Collection[str] = (), replacing: bool = False
    ) -> list[McpTool]:
        """Ask the server what it serves, and translate every answer.

        Pages `tools/list` to the end. A tool that will not translate is
        logged and skipped rather than costing the caller the others.

        Args:
            taken: Extra names to derive distinct action names around, on top of
                the ones already registered here.
            replacing: Whether the registry's existing names are up for grabs.
                They are not, by default, so a tool that clashes with one gets a
                distinct name -- but a caller about to
                [install][a11.sdk.mcp.client.McpToolset.install] with
                ``overwrite`` wants the tool to take the name rather than to be
                renamed away from the one it collided with.

        Returns:
            One [McpTool][a11.sdk.mcp.schemas.McpTool] per translatable tool.
        """
        entries = await self._list_tools()
        claimed = {*taken, *self._tools}
        if not replacing:
            claimed.update(self._registry.list_registered_actions())
        tools: list[McpTool] = []
        for entry in entries:
            try:
                tool = action_schema_from_tool(
                    entry,
                    server=self._server,
                    prefix=self._prefix,
                    taken=claimed,
                )
            except Exception:
                # One tool whose schema we cannot read should not cost the
                # caller the other forty, and the name is what a person needs
                # to go and look at the server.
                logging.warning(
                    "could not translate the MCP tool %r served by %s",
                    getattr(entry, "name", None) or entry,
                    self._server,
                    exc_info=True,
                )
                continue
            claimed.add(tool.action_name)
            tools.append(tool)
        return tools

    async def _list_tools(self) -> list[Any]:
        """Every `tools/list` entry the server will give us."""
        entries: list[Any] = []
        cursor: str | None = None
        for page in range(_MAX_TOOL_PAGES):
            result = await self._client.list_tools(cursor=cursor)
            entries.extend(result.tools)
            cursor = getattr(result, "next_cursor", None)
            if not cursor:
                return entries
            if page == _MAX_TOOL_PAGES - 1:
                logging.warning(
                    "%s is still paginating tools/list after %d pages; keeping"
                    " the %d tools listed so far",
                    self._server,
                    _MAX_TOOL_PAGES,
                    len(entries),
                )
        return entries

    def install(
        self, tools: Sequence[McpTool], *, overwrite: bool = False
    ) -> list[str]:
        """Register each tool as an Action here.

        Args:
            tools: What [discover][a11.sdk.mcp.client.McpToolset.discover]
                returned.
            overwrite: Replace a name the registry already serves. Off by
                default, following
                [install_schemas][a11.client.discovery.install_schemas]: a local
                action with a handler of its own is not something a remote tool
                should silently take over.

        Returns:
            The action names registered, in order.
        """
        registered: list[str] = []
        for tool in tools:
            name = tool.action_name
            if not overwrite and self._registry.is_registered(name):
                logging.info(
                    "keeping the registered %r over the MCP tool %r from %s",
                    name,
                    tool.tool_name,
                    self._server,
                )
                continue
            self._registry.register(
                name, tool.schema, make_handler(tool, self.call)
            )
            self._tools[name] = tool
            registered.append(name)
        logging.info(
            "registered %d MCP tool(s) from %s", len(registered), self._server
        )
        return registered

    async def refresh(self, *, overwrite: bool = False) -> list[str]:
        """[discover][a11.sdk.mcp.client.McpToolset.discover] then
        [install][a11.sdk.mcp.client.McpToolset.install], the common pairing.

        Call it again to pick up a server whose tool list has changed; a tool
        already registered here keeps its action name.
        """
        return self.install(
            await self.discover(replacing=overwrite), overwrite=overwrite
        )


@contextlib.asynccontextmanager
async def connect(
    server: Any,
    *,
    registry: a11.ActionRegistry | None = None,
    prefix: str = "",
    overwrite: bool = False,
    server_label: str = "",
    **client_options: Any,
) -> AsyncIterator[McpToolset]:
    """Connect to an MCP server and register everything it serves.

    Args:
        server: What to connect to, as `mcp.Client` accepts it: a URL for
            Streamable HTTP, an `mcp.StdioServerParameters` to launch a
            subprocess, a transport built by hand, an in-process server (which
            is what the tests use), or an `mcp.Client` this has not entered yet.
        registry: Where to register the tools; a new registry by default.
        prefix: Prepended to every action name, to keep two servers apart in
            one registry.
        overwrite: Let a tool replace an action of the same name already in
            ``registry``.
        server_label: How to name the server in the derived schemas. Worth
            setting for a hand-built transport, which has no name to take.
        **client_options: Passed to `mcp.Client` -- `sampling_callback`,
            `elicitation_callback`, `logging_callback`, `client_info`,
            `read_timeout_seconds` and the rest. Ignored when ``server`` is
            already a client.

    Yields:
        The [McpToolset][a11.sdk.mcp.client.McpToolset], with every tool
        registered.

    Raises:
        StatusException: FAILED_PRECONDITION when the MCP SDK is not installed.
    """
    mcp = load_mcp()
    if isinstance(server, mcp.Client):
        client = server
        label = server_label or server_name(client.server)
    else:
        client = mcp.Client(server, **client_options)
        label = server_label or server_name(server)

    async with client:
        toolset = McpToolset(
            client, server=label, registry=registry, prefix=prefix
        )
        await toolset.refresh(overwrite=overwrite)
        yield toolset


__all__ = [
    "INSTALL_HINT",
    "McpToolset",
    "connect",
    "load_mcp",
    "server_name",
]
