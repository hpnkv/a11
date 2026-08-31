# Copyright 2026 The A11 Authors.

"""End-to-end tests against a real MCP client, in process.

The mirror of `test_toolset.py`: there an A11 registry is built from somebody
else's MCP server, here an MCP client is served by an A11 registry. The SDK
connects a client straight to a server object, so these run the whole path --
declaration, discovery, `tools/call`, an action run, and the result coming back
-- with no subprocess, no socket and no model.

The last test closes the loop: `a11.sdk.mcp.connect` against the server built
here, so an action goes out as a tool and comes back as an action.
"""

import json

import pytest

import a11
from a11.sdk import mcp as a11mcp
from a11.sdk.mcp import server as a11server
from a11.sdk.mcp.schemas import McpHeaders, McpMeta
from a11.sdk.mcp.tests.test_tools import build_registry

pytest.importorskip("mcp")

import mcp


def build_server(**options) -> a11server.McpActionServer:
    """The test registry, served as MCP tools."""
    registry = build_registry()

    @registry.action
    async def boom(why: str) -> str:
        """Always fail."""
        raise ValueError(why)

    @registry.action
    async def narrated(action: a11.Action, text: str) -> str:
        """Narrate while working."""
        await action.log("halfway")
        return text

    @registry.action
    async def peek(action: a11.Action, text: str = "") -> str:
        """Report one header of the call."""
        return action.get_header("x-a11-tenant", decode=True) or ""

    @registry.action
    async def metadata(action: a11.Action) -> str:
        """Report the MCP metadata the call carried."""
        return action.get_header(McpHeaders.META.value, decode=True) or ""

    return a11server.McpActionServer(registry, name="a11-test", **options)


@pytest.mark.asyncio
async def test_a_client_lists_the_registered_actions():
    async with mcp.Client(build_server().server) as client:
        listed = {tool.name: tool for tool in (await client.list_tools()).tools}

    assert {"move", "shout", "counted", "picture", "nothing"} <= set(listed)
    assert listed["move"].description.startswith("Move a point east")
    assert set(listed["move"].input_schema["properties"]) == {"p", "text"}
    assert listed["counted"].output_schema is not None


@pytest.mark.asyncio
async def test_a_call_runs_the_action_and_returns_its_result():
    async with mcp.Client(build_server().server) as client:
        result = await client.call_tool("move", {"p": {"x": 1}, "text": "abc"})

    assert not result.is_error
    assert result.structured_content == {"output": {"x": 4, "y": 0}}
    # The serialized result travels as text too, for a client that reads only
    # content blocks.
    assert "4" in result.content[0].text


@pytest.mark.asyncio
async def test_a_streaming_output_comes_back_as_a_sequence():
    async with mcp.Client(build_server().server) as client:
        result = await client.call_tool("counted", {"text": "one two three"})

    assert result.structured_content == {
        "words": ["one", "two", "three"],
        "total": 3,
    }


@pytest.mark.asyncio
async def test_a_stream_of_one_is_still_a_sequence():
    # The port carried one value, and the output schema says the field is an
    # array; a client validating the result against it reads a list.
    async with mcp.Client(build_server().server) as client:
        result = await client.call_tool("counted", {"text": "one"})

    assert result.structured_content == {"words": ["one"], "total": 1}


@pytest.mark.asyncio
async def test_a_streaming_input_takes_a_list():
    async with mcp.Client(build_server().server) as client:
        result = await client.call_tool("joined", {"parts": ["a", "b", "c"]})

    assert result.structured_content == {"output": "abc"}


@pytest.mark.asyncio
async def test_an_action_taking_nothing_is_called_with_nothing():
    async with mcp.Client(build_server().server) as client:
        result = await client.call_tool("nothing", {})

    assert result.structured_content == {"output": "nothing to do"}


@pytest.mark.asyncio
async def test_a_picture_comes_back_as_a_content_block():
    async with mcp.Client(build_server().server) as client:
        result = await client.call_tool("picture", {})

    images = [block for block in result.content if block.type == "image"]
    assert len(images) == 1
    assert images[0].mime_type == "image/png"
    assert images[0].meta == {McpMeta.PORT.value: "image"}
    # The picture left the structured result; the caption stayed in it.
    assert result.structured_content == {"caption": "a cat"}


@pytest.mark.asyncio
async def test_a_failing_action_is_a_failed_tool_call():
    async with mcp.Client(build_server().server) as client:
        result = await client.call_tool("boom", {"why": "no reason"})

    assert result.is_error
    assert "no reason" in result.content[0].text


@pytest.mark.asyncio
async def test_a_missing_argument_is_reported_to_the_caller():
    async with mcp.Client(build_server().server) as client:
        result = await client.call_tool("shout", {})

    assert result.is_error
    assert "text" in result.content[0].text


@pytest.mark.asyncio
async def test_a_tool_this_server_does_not_serve_is_a_protocol_error():
    async with mcp.Client(build_server().server) as client:
        with pytest.raises(Exception, match="no tool named"):
            await client.call_tool("nowhere", {})


@pytest.mark.asyncio
async def test_narration_becomes_progress():
    seen: list[str] = []

    async def on_progress(progress, total, message):
        seen.append(message or "")

    async with mcp.Client(build_server().server) as client:
        await client.call_tool(
            "narrated", {"text": "done"}, progress_callback=on_progress
        )

    assert "halfway" in "".join(seen)


@pytest.mark.asyncio
async def test_narration_comes_back_on_the_result_without_a_progress_token():
    async with mcp.Client(build_server().server) as client:
        result = await client.call_tool("narrated", {"text": "done"})

    assert "halfway" in result.meta[a11mcp.calls.LOG_META]


@pytest.mark.asyncio
async def test_a_call_meta_becomes_a_header_the_action_reads():
    server = build_server(accept_headers=["x-a11-tenant"])
    async with mcp.Client(server.server) as client:
        result = await client.call_tool(
            "peek", {}, meta={McpMeta.HEADERS.value: {"x-a11-tenant": "acme"}}
        )

    assert result.structured_content == {"output": "acme"}


@pytest.mark.asyncio
async def test_a_header_the_server_does_not_accept_is_left_off():
    async with mcp.Client(build_server().server) as client:
        result = await client.call_tool(
            "peek", {}, meta={McpMeta.HEADERS.value: {"x-a11-tenant": "acme"}}
        )

    assert result.structured_content == {"output": ""}


@pytest.mark.asyncio
async def test_patterns_narrow_what_a_client_sees():
    server = build_server(patterns=["sho.*"])
    async with mcp.Client(server.server) as client:
        listed = {tool.name for tool in (await client.list_tools()).tools}

    assert listed == {"shout"}


@pytest.mark.asyncio
async def test_an_action_survives_the_round_trip_back_into_a_registry():
    """A11 -> MCP -> A11, with the schema intact rather than approximated."""
    served = build_server()
    async with a11mcp.connect(served.server, server_label="in-process") as back:
        rebuilt = back.registry.get_schema("counted")
        original = served.registry.get_schema("counted")

        assert set(rebuilt.inputs) == set(original.inputs)
        assert set(rebuilt.outputs) == set(original.outputs)
        assert rebuilt.outputs["words"].unary is False
        assert rebuilt.inputs["text"].required is True

        call = back.action("counted").run()
        await call["text"].finalize("one two")
        assert [word async for word in call["words"]] == ["one", "two"]
        assert await call["total"].consume(int) == 2
        await call.wait()


@pytest.mark.asyncio
async def test_a_picture_survives_the_round_trip_as_bytes():
    async with a11mcp.connect(
        build_server().server, server_label="in-process"
    ) as back:
        call = back.action("picture").run()
        assert await call["image"].consume(bytes) == b"not really a png"
        assert await call["caption"].consume(str) == "a cat"
        await call.wait()


@pytest.mark.asyncio
async def test_a_failure_survives_the_round_trip_as_a_failure():
    async with a11mcp.connect(
        build_server().server, server_label="in-process"
    ) as back:
        call = back.action("boom").run()
        await call["why"].finalize("no reason")
        with pytest.raises(Exception, match="no reason"):
            await call.wait()


@pytest.mark.asyncio
async def test_the_whole_call_meta_reaches_the_action_as_one_header():
    # The same header `handlers` sends back *out* as `_meta`, so a round trip
    # through two A11 peers keeps a caller's metadata.
    async with mcp.Client(build_server().server) as client:
        result = await client.call_tool("metadata", {}, meta={"tenant": "acme"})

    carried = json.loads(result.structured_content["output"])
    assert carried["tenant"] == "acme"
    # MCP's own reserved keys are the SDK's, and stay out of it.
    assert not any(
        key.startswith("io.modelcontextprotocol/") for key in carried
    )
