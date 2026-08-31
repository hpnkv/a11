# Copyright 2026 The A11 Authors.

"""An MCP server's tools, as A11 Actions.

Point this at a Model Context Protocol server and it gives back an
[ActionRegistry][a11.actions.registry.ActionRegistry] holding one Action per
tool: a schema derived from the tool's JSON Schema, and a handler that calls the
tool and streams its result back onto the action's ports. From there the tools
are ordinary A11 actions -- a model can be offered them by
`interact_with_llm`, a flow can `call` them, a peer can be served them -- and
nothing downstream needs to know MCP was involved.

```python
import a11
from a11.sdk import mcp

async with mcp.connect("https://example.com/mcp") as toolset:
    # Every tool the server serves, as Actions.
    print(sorted(toolset.registry.list_registered_actions()))

    # Call one directly...
    search = toolset.action("search").run()
    await search["query"].finalize("streaming runtimes")
    async for line in search["text"]:
        print(line)
    await search.wait()

    # ...or hand the whole registry to a model.
    interact = (
        a11.Action(INTERACT_WITH_LLM_SCHEMA)
        .bind_registry(toolset.registry)
        .bind_handler(interact_with_llm)
        .set_header(LlmHeaders.ALLOWED_LLM_ACTIONS.value, ".*")
    )
```

Or the other way round: [a11.sdk.mcp.server][a11.sdk.mcp.server] serves an
`ActionRegistry` to MCP clients, so the actions an A11 process already has
become tools a Claude Desktop or an editor can call.

```python
await mcp.serve_stdio(REGISTRY, name="my-tools")
```

Each direction is two halves, and they separate cleanly:

* [a11.sdk.mcp.schemas][a11.sdk.mcp.schemas] is the **translation** -- one MCP
  tool read as an `ActionSchema`, with arguments as ports, results as ports, and
  MCP's own per-call context as headers. It touches no transport and imports no
  SDK, so a schema can be derived and shipped to a peer anywhere A11 runs.
* [a11.sdk.mcp.client][a11.sdk.mcp.client] is the **connection** -- discovery,
  registration and the `tools/call` each handler makes, over the MCP SDK.
* [a11.sdk.mcp.tools][a11.sdk.mcp.tools] is the **translation back** -- one
  registered action declared as an MCP tool, arguments from ports and a result
  schema from outputs -- and [a11.sdk.mcp.calls][a11.sdk.mcp.calls] runs one
  call against it. Neither imports the SDK either.
* [a11.sdk.mcp.server][a11.sdk.mcp.server] is the **serving** -- the MCP server
  object, over stdio or Streamable HTTP.

Needs the MCP SDK (`pip install 'a11-kit[mcp]'`); everything but
`a11.sdk.mcp.client` works without it.
"""

from a11.sdk.mcp.calls import call_action, headers_from_meta
from a11.sdk.mcp.client import (
    INSTALL_HINT,
    McpToolset,
    connect,
    load_mcp,
    server_name,
)
from a11.sdk.mcp.handlers import McpCall, make_handler
from a11.sdk.mcp.schemas import (
    ARGUMENTS_INPUT,
    CONTENT_OUTPUT,
    MCP_HEADERS,
    STRUCTURED_OUTPUT,
    TEXT_OUTPUT,
    McpArgument,
    McpHeaders,
    McpMeta,
    McpTool,
    action_schema_from_tool,
    described_action,
    sanitise_name,
    tool_description,
    tool_document,
)
from a11.sdk.mcp.server import (
    McpActionServer,
    http_app,
    serve_http,
    serve_stdio,
)
from a11.sdk.mcp.tools import (
    ActionTool,
    tool_from_entry,
    tools_from_registry,
)

__all__ = [
    "ARGUMENTS_INPUT",
    "CONTENT_OUTPUT",
    "INSTALL_HINT",
    "MCP_HEADERS",
    "STRUCTURED_OUTPUT",
    "TEXT_OUTPUT",
    "ActionTool",
    "McpActionServer",
    "McpArgument",
    "McpCall",
    "McpHeaders",
    "McpMeta",
    "McpTool",
    "McpToolset",
    "action_schema_from_tool",
    "call_action",
    "connect",
    "described_action",
    "headers_from_meta",
    "http_app",
    "load_mcp",
    "make_handler",
    "sanitise_name",
    "serve_http",
    "serve_stdio",
    "server_name",
    "tool_description",
    "tool_document",
    "tool_from_entry",
    "tools_from_registry",
]
