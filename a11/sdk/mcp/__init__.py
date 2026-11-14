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

"""Use MCP tools as A11 actions, or serve A11 actions to MCP clients.

`connect` discovers an MCP server's tools and returns an
[ActionRegistry][a11.actions.registry.ActionRegistry] with a schema and handler
for each tool:

```python
from a11.sdk import mcp

async with mcp.connect("https://example.com/mcp") as toolset:
    search = toolset.action("search").run()
    await search["query"].finalize("streaming runtimes")
    async for line in search["text"]:
        print(line)
    await search.wait()
```

The registry can also supply tools to a model or a Flow program. To publish
an existing registry to MCP clients, run:

```python
await mcp.serve_stdio(REGISTRY, name="my-tools")
```

[schemas][a11.sdk.mcp.schemas] and [tools][a11.sdk.mcp.tools] translate tool
and action declarations. [client][a11.sdk.mcp.client] and
[server][a11.sdk.mcp.server] manage MCP connections and serving. Install the
MCP SDK with ``pip install 'a11-kit[mcp]'`` to connect or serve.
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
