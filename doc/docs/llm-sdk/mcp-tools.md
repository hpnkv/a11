# Use an MCP server's tools as Actions

An MCP server publishes tools as JSON Schema over its own protocol.
`a11.sdk.mcp` reads that into A11's own terms: connect to a
server and you get an [`ActionRegistry`][a11.actions.registry.ActionRegistry]
holding one [`Action`][a11.actions.action.Action] per tool — a schema derived
from the tool's, and a handler that calls the tool and streams its result onto
the action's ports.

From there they are ordinary actions. A model can be offered them by
[`interact_with_llm`](interact-actions.md), a flow can `call` them, a peer can be
served them, and nothing downstream needs to know MCP was involved.

Needs the MCP SDK: `pip install 'a11-kit[mcp]'`.

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

`connect` takes anything the MCP SDK's client does: a URL for Streamable HTTP,
an `mcp.StdioServerParameters` to launch a server as a subprocess, or a transport
you built yourself. It is an async context manager because the SDK's client is
one — a long-lived host should hold it open in the task that owns it (an
`AsyncExitStack` at start-up), rather than entering and leaving it from different
places.

Pass `registry=` to add the tools to a registry you already have, and `prefix=`
to keep two servers' tools apart in it.

HTTP headers and authorization belong to the connection rather than to a call, so
they are the SDK's business: build the transport with an HTTP client that carries
them.

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

One port per argument is what makes the tool's real signature visible: the tool
definition a model is shown for the Action comes back out looking like the
schema the server published.

```python
from a11.sdk.llm_tools import runner

runner.get_tool_definitions(toolset.registry, ["search"])
```

An array argument gets a *streaming* port for a reason worth knowing: a model's
list argument is written one fragment per element, so only a non-unary port can
tell "a list of one" from "that one value". A positional tuple
(`prefixItems`) is one value and stays unary, as does an argument that may be
either a list or a scalar.

A tool whose argument object constrains its properties *together* — `anyOf`
around them, say, or open `additionalProperties` — cannot be split without
changing what it accepts, so it keeps a single unary `arguments` port carrying
the whole schema.

When the tool declares an output schema, that document is the result: the schema
maps `structured_content` to the whole JSON value, so a model reads exactly what
the tool said it returns while a flow can still read the other ports.

## MCP context that is not an argument

It travels as a header — the same way A11 carries a deadline or a trace — and the
names are on `a11.sdk.mcp.McpHeaders`:

| Header | |
|---|---|
| `x-a11-mcp-tool` | The tool to call. Its default is the server's own spelling of the name, so an action renamed to be a valid A11 identifier still reaches the right tool. |
| `x-a11-mcp-server` | The server the handler is bound to. A call naming a different one is refused rather than sent somewhere else. |
| `x-a11-mcp-meta` | A JSON object merged into the request's `_meta`, MCP's own per-request metadata. |

Two more translations are automatic:

* **`x-a11-deadline` becomes the call's read timeout**, so one policy bounds the
  whole nested call and a slow server fails with `DEADLINE_EXCEEDED`.
* **MCP progress notifications become narration** on the action's log port,
  which is the one output a model never sees.

## Failure

MCP reports a tool's own failure inside a successful response (`isError`) so a
model can self-correct. A11 says the same thing with a non-OK status: the action
fails with `INTERNAL` carrying the server's text, and the tool runner turns that
into the failure the model reads — one failed call among several does not sink
the rest. A protocol error becomes the status code that matches it: an unknown
method is `NOT_FOUND`, bad params `INVALID_ARGUMENT`, a timeout
`DEADLINE_EXCEEDED`.

## Trying it against a real server

```sh
python scripts/mcp_playground.py --command 'uvx mcp-server-fetch' \
    --tool fetch --arguments '{"url": "https://example.com"}'
```

It prints each tool's derived schema and the tool definition a model would be
shown, then calls one and streams everything it writes.
