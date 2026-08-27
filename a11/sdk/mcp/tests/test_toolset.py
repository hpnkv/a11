# Copyright 2026 The A11 Authors.

"""End-to-end tests against a real MCP server, in process.

The MCP SDK can connect a client straight to a server object, so these run the
whole path -- discovery, translation, registration, `tools/call` and the result
coming back onto ports -- with no subprocess, no socket and no model.

Each test opens its own connection. The SDK's client is an async context manager
over an anyio task group, and entering and leaving it inside one task is what
keeps its cancel scopes intact; a shared fixture would risk tearing it down from
a different task than built it.
"""

import asyncio
import json

import pytest

import a11
from a11 import _native
from a11.sdk import mcp as a11mcp
from a11.sdk.llm import (
    Interaction,
    LlmHeaders,
    Role,
    ToolCall,
    add_tool_calls_to_interaction,
    decoded_output_text,
)
from a11.sdk.llm_tools.runner import (
    execute_actions_from_interaction,
    get_tool_definitions,
)
from a11.sdk.mcp.schemas import McpHeaders
from a11.status import StatusCode, StatusException

pytest.importorskip("mcp")

import anyio
from mcp.server.mcpserver import MCPServer
from mcp.server.mcpserver.context import Context
from mcp_types import ImageContent, TextContent
from pydantic import BaseModel, Field


class Point(BaseModel):
    """A place, for a tool that takes a model rather than a scalar."""

    x: int = Field(description="Distance east.")
    y: int = 0


class Seen(BaseModel):
    """What a tool was actually handed."""

    text: str = ""
    tags: list[str] | None = None


def build_server() -> MCPServer:
    """A server with one tool per thing worth asserting about."""
    server = MCPServer("a11-test")

    @server.tool()
    def add(a: int, b: int = 3, tags: list[str] | None = None) -> int:
        """Add two numbers, and one for each tag."""
        return a + b + len(tags or [])

    @server.tool()
    def seen(text: str = "", tags: list[str] | None = None) -> Seen:
        """Report the arguments as they arrived."""
        return Seen(text=text, tags=tags)

    @server.tool()
    def move(p: Point, text: str = "") -> Point:
        """Move a point east by the length of a word."""
        return Point(x=p.x + len(text), y=p.y)

    @server.tool()
    def picture() -> list:
        """Return a caption and an image."""
        return [
            TextContent(type="text", text="a cat"),
            ImageContent(type="image", data="aGk=", mimeType="image/png"),
        ]

    @server.tool()
    def boom(why: str) -> str:
        """Always fail."""
        raise ValueError(why)

    @server.tool()
    def nothing() -> str:
        """Take no arguments."""
        return "nothing to do"

    @server.tool()
    async def slowly(seconds: float) -> str:
        """Answer late."""
        await anyio.sleep(seconds)
        return "awake"

    @server.tool()
    async def report(ctx: Context) -> str:
        """Narrate progress while working."""
        await ctx.report_progress(1, 2, "halfway")
        return "done"

    @server.tool()
    async def peek(ctx: Context) -> str:
        """Report one key of the request's own metadata."""
        meta = ctx.request_context.meta or {}
        return str(meta.get("tenant", ""))

    return server


def _connect(**options):
    return a11mcp.connect(build_server(), **options)


async def _collect(node: a11.AsyncNode) -> list:
    return [value async for value in node.iter_values()]


async def _log_lines(node: a11.AsyncNode) -> list[str]:
    """The action's narration, as the tool runner reads it."""
    lines = []
    async for chunk in node.iter_chunks():
        if chunk is None or chunk.is_null() or _native.is_status_chunk(chunk):
            continue
        lines.append(_native.log_record_from_chunk(chunk)["text"])
    return lines


# -- discovery and registration -----------------------------------------------


@pytest.mark.asyncio
async def test_every_tool_becomes_an_action():
    async with _connect() as toolset:
        registered = set(toolset.registry.list_registered_actions())
        assert {"add", "move", "boom", "nothing", "report"} <= registered
        assert set(toolset.tools) <= registered
        assert toolset.tools["add"].tool_name == "add"
        assert toolset.server == "a11-test"


@pytest.mark.asyncio
async def test_a_prefix_namespaces_a_server():
    async with _connect(prefix="test_") as toolset:
        assert "test_add" in toolset.registry.list_registered_actions()
        assert toolset.tools["test_add"].tool_name == "add"


@pytest.mark.asyncio
async def test_an_action_already_registered_is_kept():
    registry = a11.ActionRegistry()
    mine = a11.ActionSchema(name="add", description="Mine.")
    registry.register("add", mine)

    async with _connect(registry=registry) as toolset:
        assert "add" not in toolset.tools
        assert registry.get_schema("add").description == "Mine."
        # The other tools still arrived.
        assert "move" in toolset.tools


@pytest.mark.asyncio
async def test_overwrite_lets_a_tool_take_a_name():
    registry = a11.ActionRegistry()
    registry.register("add", a11.ActionSchema(name="add", description="Mine."))

    async with _connect(registry=registry, overwrite=True) as toolset:
        assert "add" in toolset.tools
        assert registry.get_schema("add").description != "Mine."


# -- calling a tool -----------------------------------------------------------


@pytest.mark.asyncio
async def test_an_action_calls_its_tool_and_streams_the_result():
    async with _connect() as toolset:
        action = toolset.action("add").run()
        await asyncio.gather(
            action["a"].finalize(4),
            action["b"].finalize(5),
            action["tags"].close(),
        )
        text, structured, content = await asyncio.gather(
            _collect(action["text"]),
            action["structured_content"].consume(allow_none=True),
            _collect(action["content"]),
        )
        await action.wait()

        assert text == ["9"]
        assert structured == {"result": 9}
        assert content == []


@pytest.mark.asyncio
async def test_an_argument_nobody_wrote_is_left_to_the_server():
    # `b` defaults to 3 on the server, and an unwritten port must not send null
    # in its place.
    async with _connect() as toolset:
        action = toolset.action("add").run()
        await asyncio.gather(
            action["a"].finalize(1),
            action["b"].close(),
            action["tags"].close(),
        )
        structured = await action["structured_content"].consume(allow_none=True)
        await asyncio.gather(_collect(action["text"]), action.wait())
        assert structured == {"result": 4}


@pytest.mark.asyncio
async def test_a_streaming_argument_arrives_as_a_list():
    async with _connect() as toolset:
        action = toolset.action("add").run()
        tags = action["tags"]
        await tags.put("one")
        await tags.put("two", final=True)
        await tags.close()
        await asyncio.gather(action["a"].finalize(0), action["b"].finalize(0))
        structured = await action["structured_content"].consume(allow_none=True)
        await asyncio.gather(_collect(action["text"]), action.wait())
        assert structured == {"result": 2}


@pytest.mark.asyncio
async def test_one_value_on_a_streaming_port_is_still_a_list():
    # The reason an array argument gets a streaming port: a list of one has to
    # stay a list on the way to a server that validates it.
    async with _connect() as toolset:
        action = toolset.action("seen").run()
        await asyncio.gather(
            action["text"].close(), action["tags"].finalize("only")
        )
        structured = await action["structured_content"].consume(allow_none=True)
        await asyncio.gather(_collect(action["text_2"]), action.wait())
        assert structured["tags"] == ["only"]


@pytest.mark.asyncio
async def test_a_model_argument_travels_as_its_document():
    async with _connect() as toolset:
        action = toolset.action("move").run()
        await asyncio.gather(
            action["p"].finalize({"x": 1, "y": 2}),
            action["text"].finalize("abc"),
        )
        structured = await action["structured_content"].consume(allow_none=True)
        await asyncio.gather(_collect(action["text_2"]), action.wait())
        assert structured == {"x": 4, "y": 2}


@pytest.mark.asyncio
async def test_a_tool_that_takes_nothing_just_runs():
    async with _connect() as toolset:
        action = toolset.action("nothing").run()
        assert await _collect(action["text"]) == ["nothing to do"]
        await asyncio.gather(_collect(action["content"]), action.wait())


@pytest.mark.asyncio
async def test_content_that_is_not_text_lands_on_the_content_port():
    async with _connect() as toolset:
        action = toolset.action("picture").run()
        text, content = await asyncio.gather(
            _collect(action["text"]), _collect(action["content"])
        )
        await action.wait()

        assert text == ["a cat"]
        assert content == [
            {"type": "image", "data": "aGk=", "mimeType": "image/png"}
        ]


# -- failures -----------------------------------------------------------------


@pytest.mark.asyncio
async def test_a_failing_tool_fails_the_action():
    async with _connect() as toolset:
        action = toolset.action("boom").run()
        await action["why"].finalize("no reason")
        with pytest.raises(StatusException) as caught:
            await action.wait()
        assert caught.value.status.code == StatusCode.INTERNAL
        assert "boom" in caught.value.status.message


@pytest.mark.asyncio
async def test_a_tool_the_server_does_not_serve_fails():
    async with _connect() as toolset:
        action = toolset.action("nothing")
        action.set_header(McpHeaders.TOOL.value, b"no_such_tool")
        action.run()
        with pytest.raises(StatusException):
            await action.wait()


@pytest.mark.asyncio
async def test_the_deadline_bounds_the_call():
    async with _connect() as toolset:
        action = toolset.action("slowly")
        a11.set_deadline_header(action, a11.now() + a11.Duration.seconds(0.3))
        action.run()
        await action["seconds"].finalize(30)
        with pytest.raises(StatusException) as caught:
            await action.wait()
        assert caught.value.status.code == StatusCode.DEADLINE_EXCEEDED


# -- headers ------------------------------------------------------------------


@pytest.mark.asyncio
async def test_the_tool_header_chooses_the_tool():
    # The header the translation writes as a default is what the handler calls,
    # so overriding it reaches another tool on the same server.
    async with _connect() as toolset:
        action = toolset.action("nothing")
        action.set_header(McpHeaders.TOOL.value, b"picture")
        action.run()
        assert await _collect(action["text"]) == ["a cat"]
        await asyncio.gather(_collect(action["content"]), action.wait())


@pytest.mark.asyncio
async def test_a_call_aimed_at_another_server_is_refused():
    async with _connect() as toolset:
        action = toolset.action("nothing")
        action.set_header(McpHeaders.SERVER.value, b"https://elsewhere/mcp")
        action.run()
        with pytest.raises(StatusException) as caught:
            await action.wait()
        assert caught.value.status.code == StatusCode.INVALID_ARGUMENT
        assert "elsewhere" in caught.value.status.message


@pytest.mark.asyncio
async def test_the_meta_header_reaches_the_server():
    async with _connect() as toolset:
        action = toolset.action("peek")
        action.set_header(
            McpHeaders.META.value, json.dumps({"tenant": "acme"}).encode()
        )
        action.run()
        assert await _collect(action["text"]) == ["acme"]
        await asyncio.gather(_collect(action["content"]), action.wait())


@pytest.mark.asyncio
async def test_a_meta_header_that_is_not_an_object_is_refused():
    async with _connect() as toolset:
        action = toolset.action("peek")
        action.set_header(McpHeaders.META.value, b"[1, 2]")
        action.run()
        with pytest.raises(StatusException) as caught:
            await action.wait()
        assert caught.value.status.code == StatusCode.INVALID_ARGUMENT


# -- narration ----------------------------------------------------------------


@pytest.mark.asyncio
async def test_progress_is_narrated_on_the_log_port():
    async with _connect() as toolset:
        action = toolset.action("report")
        log = action.get_log_node()
        action.run()
        text, lines = await asyncio.gather(
            _collect(action["text"]), _log_lines(log)
        )
        await asyncio.gather(_collect(action["content"]), action.wait())

        assert text == ["done"]
        assert any("halfway" in line for line in lines)
        # And the narration never reaches a declared output.
        assert not any("halfway" in value for value in text)


# -- the model's view ---------------------------------------------------------


@pytest.mark.asyncio
async def test_a_model_is_shown_the_tools_own_schema():
    async with _connect() as toolset:
        [definition] = get_tool_definitions(toolset.registry, ["add"])

        assert definition["name"] == "add"
        assert "Add two numbers" in definition["description"]
        properties = definition["input_schema"]["properties"]
        assert properties["a"]["type"] == "integer"
        assert properties["tags"] == {
            "type": "array",
            "items": {"type": "string"},
        }
        assert definition["input_schema"]["required"] == ["a"]


@pytest.mark.asyncio
async def test_the_tool_runner_calls_a_tool_end_to_end():
    """One turn's worth of the real path: a tool call in, a tool result out."""
    async with _connect() as toolset:
        registry = toolset.registry
        results: dict[str, str] = {}

        driver_schema = a11.ActionSchema(
            name="drive_tools",
            outputs={"done": a11.ActionPortSchema("done", "text/plain")},
        )

        async def drive(action: a11.Action) -> None:
            interaction = Interaction(role=Role.ASSISTANT)
            await add_tool_calls_to_interaction(
                [
                    ToolCall(
                        name="add",
                        id="call-1",
                        params={"a": 2, "tags": ["x", "y"]},
                    ),
                    ToolCall(name="boom", id="call-2", params={"why": "told"}),
                ],
                interaction,
                registry,
            )
            executed = await execute_actions_from_interaction(
                interaction, action, registry
            )
            results["ok"] = await decoded_output_text(
                executed.outputs["call-1"]
            )
            results["failed"] = executed.error_message("call-2") or ""
            await action["done"].close()

        registry.register("drive_tools", driver_schema, drive)
        action = registry.make_action("drive_tools")
        action.set_header(LlmHeaders.ALLOWED_LLM_ACTIONS.value, b".*")
        action.run()
        await _collect(action["done"])
        await action.wait()

        # The tool declares an output schema, so its structured result *is* the
        # result the model reads -- not an object wrapping it.
        assert json.loads(results["ok"]) == {"result": 7}
        # And one failed call is reported as that call's failure.
        assert results["failed"].startswith("INTERNAL:")
        assert "boom" in results["failed"]
