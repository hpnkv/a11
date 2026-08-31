# Serve A11 actions as MCP tools

An MCP host — Claude Desktop, an editor, another agent runtime — discovers tools
over the Model Context Protocol. `a11.sdk.mcp.server` publishes an
[`ActionRegistry`][a11.actions.registry.ActionRegistry] as one such server, with
one tool per action: the arguments come from the action's input ports, the
result schema from its outputs, and running the tool runs the action.

This is the reverse of [using MCP tools as actions](mcp-tools.md), and the two
share their machinery. The tool a client discovers here is derived from the same
`a11.actions/v1` document that describes the action to an A11 peer and offers it
to a model, so the three views cannot drift.

Install the MCP integration with `pip install 'a11-kit[mcp]'`.

## Serve a registry

```python
import a11
from a11.sdk import mcp

REGISTRY = a11.ActionRegistry()


@REGISTRY.action
async def summarise(document: str, style: str = "neutral") -> str:
    """Summarise a document."""
    ...


await mcp.serve_stdio(REGISTRY, name="my-tools")
```

`serve_stdio` speaks the transport an MCP host uses when it launches a server as
a subprocess. `serve_http` listens for Streamable HTTP clients:

```python
await mcp.serve_http(REGISTRY, host="127.0.0.1", port=8013, path="/mcp")
```

For an application that owns its own web server,
[`http_app`][a11.sdk.mcp.server.http_app] returns the Starlette app to mount.
Its lifespan runs the MCP session manager, so the mounting application has to
run that lifespan.

## From the command line

`a11 serve` publishes a registry over A11's own transports and MCP at once,
from the same process and the same registry.

```sh
a11 serve mypkg.actions --mcp                 # Streamable HTTP on 8013
a11 serve mypkg.actions --mcp-stdio           # launched by an MCP host
a11 serve mypkg.actions --ws --mcp            # both protocols, one registry
```

`--mcp-allow PATTERN` narrows which actions become tools, and
`--mcp-accept-header PATTERN` widens which headers a client may set.
`--mcp-stdio` gives the protocol this process's stdout, so the command's summary
goes to stderr and the command ends when the client disconnects.

To see the declarations without starting a server:

```sh
python scripts/mcp_playground.py --declare mypkg.actions
```

## What an action becomes

Take `counted`, which takes a line and writes two outputs — one word at a time,
and their number:

| | Declared as |
|---|---|
| `text` input, unary, required | `inputSchema.properties.text`, in `required` |
| `parts` input, streaming | `{"type": "array", "items": …}` |
| an autofilled input | nothing; a caller cannot write one |
| `words` output, streaming | `outputSchema.properties.words`, an array |
| `total` output, unary | `outputSchema.properties.total` |

A result comes back three ways at once, which is what MCP asks of a server that
declares an output schema:

* `structuredContent` — the object the outputs decode to, the same value
  [`decode_action_output_fragments`][a11.sdk.llm.decode_action_output_fragments]
  gives a model for the same run.
* `content` — that object serialised into a text block, for a client that reads
  only content blocks.
* a content block per picture or sound. A port whose media type is `image/*` or
  `audio/*` becomes an `ImageContent` or `AudioContent` block and leaves the
  structured document, because base64 inside JSON is a string to everything
  downstream.

`outputSchema` is declared only when the result is a JSON object. An action that
maps one port to the whole result (`output_to_json_field`) and returns a string
answers in text alone.

### Streaming, and what MCP can carry

A `tools/call` is one request and one response, so a client reads the result
when the action finishes. What arrives during the run is narration: anything the
handler writes with [`Action.log`][a11.actions.action.Action.log] is relayed as
a progress notification as it happens, and comes back whole on the result's
`_meta` for a client that asked for no notifications. Narration never enters the
tool result, so a model reads what the action declared and nothing else.

## Deadlines, headers and `_meta`

The server owns the deadline: every call gets `x-a11-deadline`, which bounds the
handler, everything it calls, and the drain of what it wrote.

MCP has no header on a call; it has `_meta`, and
[`headers_from_meta`][a11.sdk.mcp.calls.headers_from_meta] reads the headers a
client asks for out of it, three ways:

| In `_meta` | Becomes |
|---|---|
| `to.a11/headers`, an object | those headers, by name |
| any `x-a11-*` or `x-otel-*` key | that header |
| the whole object | the `x-a11-mcp-meta` header |

The last is the inverse of what the client half sends *out* as `_meta`, so
metadata survives an A11 → MCP → A11 round trip.

Only the headers an action declares are applied. A client of an MCP server is
not the process owner, and a header the action did not declare an interest in is
one it should not be able to set; `accept_headers` widens that where a
deployment wants it.

## Failure

An action that fails becomes `isError` with the failure text — `INTERNAL: …`,
`DEADLINE_EXCEEDED: …` — which is what MCP gives a model to correct itself with,
and what the client half turns back into a non-OK action status. Arguments that
do not fit the schema fail the same way, so the model can fix its own call. A
tool the server does not serve is a protocol error instead: nothing ran, and
there is no tool result to report.

## Two A11 peers over MCP

Each tool carries its action's `a11.actions/v1` document in the tool's `_meta`,
under `to.a11/action`. Any client ignores it and sees an ordinary MCP tool. An
A11 client reads it and rebuilds the action as it was declared — port names,
which ports stream, header schemas — rather than deriving an approximation from
JSON Schema, and the result lands on the action's own output ports:

```python
async with mcp.connect("http://127.0.0.1:8013/mcp") as toolset:
    call = toolset.action("counted").run()
    await call["text"].finalize("one two")
    async for word in call["words"]:
        print(word)
    await call.wait()
```

Pass `describe_actions=False` to leave it off.
