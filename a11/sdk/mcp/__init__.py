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

The two halves are separable, and worth knowing apart:

* [a11.sdk.mcp.schemas][a11.sdk.mcp.schemas] is the **translation** -- one MCP
  tool read as an `ActionSchema`, with arguments as ports, results as ports, and
  MCP's own per-call context as headers. It touches no transport and imports no
  SDK, so a schema can be derived and shipped to a peer anywhere A11 runs.
* [a11.sdk.mcp.client][a11.sdk.mcp.client] is the **connection** -- discovery,
  registration and the `tools/call` each handler makes, over the MCP SDK.

Needs the MCP SDK (`pip install 'a11-kit[mcp]'`); everything but
`a11.sdk.mcp.client` works without it.
"""

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
    McpTool,
    action_schema_from_tool,
    sanitise_name,
    tool_description,
    tool_document,
)

__all__ = [
    "ARGUMENTS_INPUT",
    "CONTENT_OUTPUT",
    "INSTALL_HINT",
    "MCP_HEADERS",
    "STRUCTURED_OUTPUT",
    "TEXT_OUTPUT",
    "McpArgument",
    "McpCall",
    "McpHeaders",
    "McpTool",
    "McpToolset",
    "action_schema_from_tool",
    "connect",
    "load_mcp",
    "make_handler",
    "sanitise_name",
    "server_name",
    "tool_description",
    "tool_document",
]
