# Use MCP tools as A11 actions

An MCP server publishes tools as JSON Schema. `a11.sdk.mcp` connects to the
server and creates an [`ActionRegistry`][a11.actions.registry.ActionRegistry]
with one [`Action`][a11.actions.action.Action] per tool. Each action has a schema
derived from the MCP declaration and a handler that streams the tool result
onto its output ports.

The resulting actions work with the rest of A11: offer them to a model through
[`interact_with_llm`](interact-actions.md), call them from a flow, or serve them
to another peer.

Install the MCP integration with `pip install 'a11-kit[mcp]'`.

## Connect

```python
from a11.sdk import mcp

async with mcp.connect("https://example.com/mcp") as toolset:
    print(sorted(toolset.registry.list_registered_actions()))

    search = toolset.action("search").run()
    await search["query"].finalize("streaming runtimes")
    async for line in search["text"]:
        print(line)
    await search.wait()
```

`connect` accepts a URL for Streamable HTTP, an
`mcp.StdioServerParameters` value that launches a subprocess, or an existing MCP
transport. Its async context owns the client session. A long-lived host should
enter that context from its owning task and keep it open, commonly through an
`AsyncExitStack` created at startup.

Pass `registry=` to add the tools to a registry you already have, and `prefix=`
to keep two servers' tools apart in it.

Configure HTTP headers and authorization on the transport when creating the
connection. Every tool call over that connection then uses the same client
configuration.

```python
import httpx2
from mcp.client.streamable_http import streamable_http_client

http = httpx2.AsyncClient(headers={"Authorization": f"Bearer {token}"})
url = "https://example.com/mcp"
async with mcp.connect(
    streamable_http_client(url, http_client=http), server_label=url
) as toolset:
    ...
```

## What a tool becomes

Take a tool whose arguments are `{"query": string, "fields": string[]}` and whose
result is described by an output schema. The derived Action has:

| Port | | |
|---|---|---|
| `query` | input, unary, `text/plain` | one argument, carrying the property's own JSON Schema |
| `fields` | input, streaming, `application/json` | a homogeneous array becomes a stream of its items |
| `text` | output, streaming, `text/plain` | the result's text blocks, in order |
| `content` | output, streaming, `application/json` | the result's image, audio and resource blocks, verbatim |
| `structured_content` | output, unary, `application/json` | `structuredContent`, present only when the tool declares an output schema |

One port per argument preserves the tool's signature. Converting the action
back into a model-facing tool definition produces the argument schema published
by the server.

```python
from a11.sdk.llm_tools import runner

runner.get_tool_definitions(toolset.registry, ["search"])
```

An array argument gets a *streaming* port. A model's list argument is written
one fragment per element, which distinguishes a one-element list from one
scalar value. A positional tuple
(`prefixItems`) is one value and stays unary, as does an argument that may be
either a list or a scalar.

A tool whose argument object constrains its properties *together* — `anyOf`
around them, say, or open `additionalProperties` — cannot be split without
changing what it accepts, so it keeps a single unary `arguments` port carrying
the whole schema.

When the tool declares an output schema, that document is the result: the schema
maps `structured_content` to the whole JSON value, so a model reads exactly what
the tool said it returns while a flow can still read the other ports.

## Carry MCP context in headers

It travels as a header — the same way A11 carries a deadline or a trace — and the
names are on `a11.sdk.mcp.McpHeaders`:

| Header | |
|---|---|
| `x-a11-mcp-tool` | The tool to call. Its default is the server's own spelling of the name, so an action renamed to be a valid A11 identifier still reaches the right tool. |
| `x-a11-mcp-server` | The server the handler is bound to. A call naming a different server is refused. |
| `x-a11-mcp-meta` | A JSON object merged into the request's `_meta`, MCP's own per-request metadata. |

Two more translations are automatic:

- **`x-a11-deadline` becomes the call's read timeout**, so one policy bounds the
  whole nested call and a slow server fails with `DEADLINE_EXCEEDED`.
- **MCP progress notifications become narration** on the action's log port,
  which is the one output a model never sees.

## Failure

MCP reports a tool's own failure inside a successful response (`isError`). A11
maps it to a non-OK action status: `INTERNAL`, with the server's text. The tool
runner returns that failure to the model while allowing other requested calls
to finish. Protocol errors retain the closest status code: an unknown method is
`NOT_FOUND`, bad parameters are `INVALID_ARGUMENT`, and a timeout is
`DEADLINE_EXCEEDED`.

## Try a real server

```sh
python scripts/mcp_playground.py --command 'uvx mcp-server-fetch' \
    --tool fetch --arguments '{"url": "https://example.com"}'
```

It prints each tool's derived schema and the tool definition a model would be
shown, then calls one and streams everything it writes.
