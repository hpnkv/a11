# Copyright 2026 The A11 Authors.

"""Drive a real MCP server through A11 Actions, or see a registry as one.

Point it at a server and it prints what the tools became -- the derived
`ActionSchema` for each, and the tool definition a model would be shown -- then
optionally calls one and streams the result back off the action's ports.

```sh
# A server over Streamable HTTP.
python scripts/mcp_playground.py --server https://example.com/mcp

# A server launched as a subprocess.
python scripts/mcp_playground.py --command 'uvx mcp-server-fetch'

# ...and call one of its tools.
python scripts/mcp_playground.py --command 'uvx mcp-server-fetch' \
    --tool fetch --arguments '{"url": "https://example.com"}'
```

The other direction prints the `tools/list` entry each registered action would
be published as, without starting a server. `a11 serve --mcp` starts one.

```sh
python scripts/mcp_playground.py --declare mypkg.actions
```
"""

import asyncio
import json
import shlex
from typing import Any, Sequence

from absl import app, flags, logging

import a11
from a11 import _native
from a11.actions import describe
from a11.sdk import mcp
from a11.sdk.llm_tools.runner import definition_from_schema

_SERVER = flags.DEFINE_string(
    "server", "", "URL of an MCP server to reach over Streamable HTTP."
)
_COMMAND = flags.DEFINE_string(
    "command", "", "Command line of an MCP server to launch over stdio."
)
_BEARER = flags.DEFINE_string(
    "bearer", "", "Bearer token to present to an HTTP server."
)
_TOOL = flags.DEFINE_string(
    "tool", "", "Action to call once discovery is done."
)
_ARGUMENTS = flags.DEFINE_string(
    "arguments", "{}", "JSON object of arguments for --tool."
)
_DEADLINE_SECONDS = flags.DEFINE_float(
    "deadline_seconds", 60.0, "How long the call may take."
)
_SCHEMAS = flags.DEFINE_bool(
    "schemas", True, "Print each tool's derived schema and tool definition."
)
_DECLARE = flags.DEFINE_string(
    "declare",
    "",
    "MODULE[:SYMBOL] holding an ActionRegistry to declare as MCP tools,"
    " printing what a client would discover.",
)


def _target() -> Any:
    """What to connect to, from the flags."""
    if _COMMAND.value:
        from mcp import StdioServerParameters

        command, *arguments = shlex.split(_COMMAND.value)
        return StdioServerParameters(command=command, args=arguments)
    if not _SERVER.value:
        raise app.UsageError("Pass --server or --command.")
    if not _BEARER.value:
        return _SERVER.value

    # Authorization belongs to the connection, so it is carried by the HTTP
    # client the transport is built with rather than by an action header.
    import httpx2
    from mcp.client.streamable_http import streamable_http_client

    http = httpx2.AsyncClient(
        headers={"Authorization": f"Bearer {_BEARER.value}"}
    )
    return streamable_http_client(_SERVER.value, http_client=http)


async def _call(toolset: mcp.McpToolset, name: str, arguments: dict) -> None:
    """Run one tool as an Action and print everything it writes."""
    tool = toolset.tools[name]
    action = toolset.action(name)
    a11.set_deadline_header(
        action, a11.now() + a11.Duration.seconds(_DEADLINE_SECONDS.value)
    )
    log = action.get_log_node()
    action.run()

    async def feed() -> None:
        if tool.whole_arguments is not None:
            await action[tool.whole_arguments].finalize(arguments or None)
            return
        for argument in tool.arguments:
            value = arguments.get(argument.property)
            node = action[argument.port]
            if value is None:
                await node.close()
            elif argument.unary:
                await node.finalize(value)
            else:
                values = value if isinstance(value, list) else [value]
                for index, item in enumerate(values):
                    await node.put(item, final=index == len(values) - 1)
                await node.close()

    async def show(label: str, port: str) -> None:
        async for value in action[port].iter_values():
            print(
                f"{label}:"
                f" {value if isinstance(value, str) else json.dumps(value)}"
            )

    async def narrate() -> None:
        async for chunk in log.iter_chunks():
            if (
                chunk is None
                or chunk.is_null()
                or _native.is_status_chunk(chunk)
            ):
                continue
            print(f"log: {_native.log_record_from_chunk(chunk)['text']}")

    reads = [
        show("text", tool.text_output),
        show("content", tool.content_output),
    ]
    if tool.structured_output is not None:
        reads.append(show("structured", tool.structured_output))
    await asyncio.gather(feed(), narrate(), *reads)
    await action.wait()


def _declare(target: str) -> None:
    """Print what a registry's actions would be published as."""
    from a11.cli.commands.serve import resolve_registry
    from a11.sdk.mcp.tools import tools_from_registry

    registry, module_path, symbol = resolve_registry(target)
    print(f"# {module_path}:{symbol}\n")
    for tool in tools_from_registry(registry):
        print(json.dumps(tool.tool, indent=2))
        print()


async def main(_: Sequence[str]) -> None:
    if _DECLARE.value:
        _declare(_DECLARE.value)
        return

    arguments = json.loads(_ARGUMENTS.value)
    if not isinstance(arguments, dict):
        raise app.UsageError("--arguments must be a JSON object.")

    async with mcp.connect(_target()) as toolset:
        print(f"# {toolset.server}\n")
        for name, tool in sorted(toolset.tools.items()):
            print(f"{name}  <-  {tool.tool_name}")
            if not _SCHEMAS.value:
                continue
            entry = describe.schema_to_json(tool.schema)
            print(json.dumps(entry, indent=2))
            print("as a tool definition:")
            print(json.dumps(definition_from_schema(entry), indent=2))
            print()

        if _TOOL.value:
            print(f"\n# calling {_TOOL.value}\n")
            await _call(toolset, _TOOL.value, arguments)


def sync_main(argv: Sequence[str]) -> None:
    asyncio.run(main(argv))


if __name__ == "__main__":
    app.run(sync_main)
