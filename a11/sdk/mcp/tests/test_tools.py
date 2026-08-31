# Copyright 2026 The A11 Authors.

"""Declaring registered actions as MCP tools.

The translation alone: no server, no client, no MCP SDK. What a tool
declaration has to say is decided by what an MCP client does with it, so the
assertions here are about the wire document rather than about the objects that
produced it.
"""

from collections.abc import AsyncIterator
from typing import Annotated

import pytest
from pydantic import BaseModel, Field

import a11
from a11.actions import describe
from a11.actions.annotated import OutputPort
from a11.sdk.mcp import tools
from a11.sdk.mcp.schemas import McpMeta
from a11.status import StatusException


class Point(BaseModel):
    """A place, for an action that takes a model rather than a scalar."""

    x: int = Field(description="Distance east.")
    y: int = 0


def build_registry() -> a11.ActionRegistry:
    """A registry with one action per thing worth asserting about."""
    registry = a11.ActionRegistry()

    @registry.action
    async def move(p: Point, text: str = "") -> Point:
        """Move a point east by the length of a word."""
        return Point(x=p.x + len(text), y=p.y)

    @registry.action
    async def shout(text: str) -> str:
        """Upper-case one line."""
        return text.upper()

    @registry.action
    async def joined(parts: AsyncIterator[str]) -> str:
        """Join what arrives."""
        return "".join([part async for part in parts])

    @registry.action
    async def counted(
        text: str,
        words: Annotated[
            a11.AsyncNode, OutputPort(description="One word each.")
        ],
        total: Annotated[
            a11.AsyncNode,
            OutputPort(mimetype="application/json", unary=True),
        ],
    ) -> None:
        """Split a line and count it."""
        for word in text.split():
            await words.put(word)
        await words.finalize()
        await total.finalize(len(text.split()))

    @registry.action
    async def picture(
        caption: Annotated[
            a11.AsyncNode,
            OutputPort(mimetype="text/plain", typeinfo=str, unary=True),
        ],
        image: Annotated[
            a11.AsyncNode, OutputPort(mimetype="image/png", unary=True)
        ],
    ) -> None:
        """Draw something."""
        await caption.finalize("a cat")
        await image.finalize(b"not really a png")

    @registry.action
    async def nothing() -> str:
        """Take no arguments."""
        return "nothing to do"

    # Built by hand, for the one thing the decorator does not express: a port
    # mapped to the whole result document.
    whole = a11.ActionSchema(
        name="whole",
        description="One string, and nothing around it.",
        inputs={
            "text": a11.ActionPortSchema(
                "text", "text/plain", required=True, unary=True, typeinfo=str
            )
        },
        outputs={
            "result": a11.ActionPortSchema(
                "result", "text/plain", required=True, unary=True, typeinfo=str
            )
        },
        output_to_json_field={"result": a11.ActionSchema.WHOLE_JSON},
    )

    async def handle_whole(action: a11.Action) -> None:
        await action["result"].finalize(await action["text"].consume(str))

    registry.register("whole", whole, handle_whole)

    return registry


def entry_for(registry: a11.ActionRegistry, name: str) -> dict:
    """One action's `a11.actions/v1` entry."""
    document = describe.registry_to_json(registry, {"exact": [name]})
    return describe.schemas_in_document(document)[0]


@pytest.fixture(name="registry")
def registry_fixture() -> a11.ActionRegistry:
    return build_registry()


def declared(registry: a11.ActionRegistry) -> dict[str, tools.ActionTool]:
    return {
        tool.action_name: tool for tool in tools.tools_from_registry(registry)
    }


def test_an_input_port_becomes_an_argument(registry):
    tool = tools.tool_from_entry(entry_for(registry, "move")).tool

    schema = tool["inputSchema"]
    assert schema["type"] == "object"
    assert set(schema["properties"]) == {"p", "text"}
    assert schema["required"] == ["p"]
    assert tool["description"].startswith("Move a point east")
    # The model's own schema travelled with the port, so a client sees the
    # fields rather than an opaque object.
    point = schema["properties"]["p"]
    resolved = schema.get("$defs", {}).get(point.get("$ref", "").split("/")[-1])
    assert "x" in (resolved or point).get("properties", {})


def test_a_streaming_input_is_an_array_of_its_items(registry):
    schema = tools.tool_from_entry(entry_for(registry, "joined")).tool[
        "inputSchema"
    ]

    assert schema["properties"]["parts"] == {
        "type": "array",
        "items": {"type": "string"},
    }


def test_an_action_taking_nothing_declares_no_arguments(registry):
    schema = tools.tool_from_entry(entry_for(registry, "nothing")).tool[
        "inputSchema"
    ]

    assert schema == {"type": "object", "properties": {}}


def test_declared_outputs_become_the_output_schema(registry):
    tool = tools.tool_from_entry(entry_for(registry, "counted"))

    schema = tool.tool["outputSchema"]
    assert schema["type"] == "object"
    assert set(schema["properties"]) == {"words", "total"}
    # A streaming output is a sequence of what it carries.
    assert schema["properties"]["words"]["type"] == "array"
    assert tool.structured


def test_one_undeclared_output_is_a_field_like_any_other(registry):
    # What `decode_action_output_fragments` gives a model for the same action:
    # an object of one field, which is what the schema declares.
    schema = tools.tool_from_entry(entry_for(registry, "shout")).tool[
        "outputSchema"
    ]

    assert schema["properties"] == {"output": {"type": "string"}}


def test_a_result_that_is_not_an_object_declares_no_output_schema(registry):
    # `whole` maps its one output to the whole result, and a string is not a
    # `structuredContent`, so the tool answers in text.
    tool = tools.tool_from_entry(entry_for(registry, "whole"))

    assert "outputSchema" not in tool.tool
    assert not tool.structured
    assert tool.whole_json == "result"


def test_a_media_port_leaves_the_structured_result(registry):
    tool = tools.tool_from_entry(entry_for(registry, "picture"))

    assert tool.media == {"image": "image/png"}
    assert set(tool.tool["outputSchema"]["properties"]) == {"caption"}


def test_the_action_entry_rides_the_tool_meta(registry):
    entry = entry_for(registry, "move")
    tool = tools.tool_from_entry(entry)

    assert tool.tool["_meta"][McpMeta.ACTION.value] == entry
    assert (
        "_meta" not in tools.tool_from_entry(entry, describe_action=False).tool
    )


def test_only_runnable_unreserved_actions_are_served(registry):
    registry.register(
        "elsewhere",
        a11.ActionSchema(name="elsewhere", description="On a peer."),
        None,
    )

    names = set(declared(registry))
    assert "elsewhere" not in names
    assert not any(describe.is_reserved_action(name) for name in names)
    assert "move" in names


def test_patterns_narrow_what_is_served(registry):
    served = tools.tools_from_registry(registry, ["sho.*", "nothing"])

    assert {tool.action_name for tool in served} == {"shout", "nothing"}


def test_an_action_skipped_by_name_is_not_served(registry):
    served = tools.tools_from_registry(registry, skip={"shout"})

    assert "shout" not in {tool.action_name for tool in served}


def test_a_nameless_entry_is_refused():
    with pytest.raises(StatusException):
        tools.tool_from_entry({"description": "no name"})
