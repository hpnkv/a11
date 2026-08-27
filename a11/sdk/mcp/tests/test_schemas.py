# Copyright 2026 The A11 Authors.

"""Tests for reading an MCP tool as an `ActionSchema`.

No server and no MCP SDK: the translation works on `tools/list` entries, so
these are the JSON a server would have sent. Several of them assert the round
trip rather than the intermediate -- what a model is shown for the derived
Action, against the tool's own input schema -- because that is the property the
port rules exist to keep.
"""

import json

import pytest

import a11
from a11.actions import describe
from a11.sdk.llm_tools.runner import definition_from_schema
from a11.sdk.mcp import schemas
from a11.sdk.mcp.schemas import McpHeaders, action_schema_from_tool
from a11.status import StatusCode, StatusException


def _tool(name="probe", **fields):
    entry = {"name": name, "inputSchema": {"type": "object"}}
    entry.update(fields)
    return entry


def _properties(**properties):
    return {"type": "object", "properties": dict(properties)}


def _port_schema(port: a11.ActionPortSchema) -> dict:
    return json.loads(port.json_schema) if port.json_schema else {}


def _definition(schema: a11.ActionSchema) -> dict:
    """What a model is shown for the derived action."""
    return definition_from_schema(describe.schema_to_json(schema))


# -- arguments as ports -------------------------------------------------------


def test_each_property_becomes_a_port():
    tool = _tool(
        inputSchema={
            **_properties(
                query={"type": "string", "description": "What to look for."},
                limit={"type": "integer", "minimum": 1},
            ),
            "required": ["query"],
        }
    )
    derived = action_schema_from_tool(tool)

    assert [argument.port for argument in derived.arguments] == [
        "query",
        "limit",
    ]
    query = derived.schema.inputs["query"]
    assert query.required
    assert query.unary
    # A string argument is a line of text, not a document.
    assert query.type == "text/plain"
    assert query.description == "What to look for."
    assert _port_schema(query) == {
        "type": "string",
        "description": "What to look for.",
    }

    limit = derived.schema.inputs["limit"]
    assert not limit.required
    assert limit.type == "application/json"
    assert _port_schema(limit) == {"type": "integer", "minimum": 1}


def test_a_tool_that_takes_nothing_has_no_input_ports():
    assert action_schema_from_tool(_tool()).schema.inputs == {}
    assert action_schema_from_tool(_tool(inputSchema={})).schema.inputs == {}


def test_an_array_property_becomes_a_streaming_port_of_its_items():
    tool = _tool(
        inputSchema={
            **_properties(paths={"type": "array", "items": {"type": "string"}}),
            "required": ["paths"],
        }
    )
    derived = action_schema_from_tool(tool)

    paths = derived.schema.inputs["paths"]
    assert not paths.unary
    assert _port_schema(paths) == {"type": "string"}
    # And the model is shown the array it started as, minimum length included
    # because the argument is required.
    assert _definition(derived.schema)["input_schema"]["properties"][
        "paths"
    ] == {
        "type": "array",
        "items": {"type": "string"},
        "minItems": 1,
    }


def test_an_optional_array_property_still_streams():
    # `list[str] | None`, which is how every pydantic-backed server spells an
    # optional list. The null branch is dropped: an argument nobody writes is
    # how "absent" is said on a port.
    tool = _tool(
        inputSchema=_properties(
            tags={
                "anyOf": [
                    {"type": "array", "items": {"type": "string"}},
                    {"type": "null"},
                ],
                "default": None,
            }
        )
    )
    tags = action_schema_from_tool(tool).schema.inputs["tags"]
    assert not tags.unary
    assert _port_schema(tags) == {"type": "string"}


def test_a_property_that_may_or_may_not_be_a_list_stays_unary():
    # Ambiguous, so it keeps its whole schema: on a unary port one fragment
    # reads back as the scalar and several as the list, which is the only
    # reading that can serve both.
    tool = _tool(
        inputSchema=_properties(
            value={
                "anyOf": [
                    {"type": "array", "items": {"type": "string"}},
                    {"type": "string"},
                ]
            }
        )
    )
    value = action_schema_from_tool(tool).schema.inputs["value"]
    assert value.unary
    assert _port_schema(value)["anyOf"]


def test_a_positional_tuple_stays_one_value():
    schema = {
        "type": "array",
        "prefixItems": [{"type": "number"}, {"type": "number"}],
    }
    tool = _tool(inputSchema=_properties(point=schema))
    point = action_schema_from_tool(tool).schema.inputs["point"]
    assert point.unary
    assert _port_schema(point) == schema


def test_a_referenced_definition_travels_with_its_port():
    tool = _tool(
        inputSchema={
            "type": "object",
            "$defs": {
                "Point": {
                    "type": "object",
                    "title": "Point",
                    "properties": {"x": {"type": "integer"}},
                    "required": ["x"],
                },
                "Unused": {"type": "string", "title": "Unused"},
            },
            "properties": {
                "where": {"$ref": "#/$defs/Point"},
                "route": {
                    "type": "array",
                    "items": {"$ref": "#/$defs/Point"},
                },
            },
        }
    )
    derived = action_schema_from_tool(tool)

    where = _port_schema(derived.schema.inputs["where"])
    assert where["$ref"] == "#/$defs/Point"
    # Resolved against the parent's definitions and re-homed at the port's own
    # root, with what the port never referenced left behind.
    assert list(where["$defs"]) == ["Point"]
    assert _port_schema(derived.schema.inputs["route"])["$defs"]

    # And a `$ref` still resolves once the ports are assembled into the
    # document a model is shown.
    definition = _definition(derived.schema)["input_schema"]
    assert list(definition["$defs"]) == ["Point"]
    assert definition["properties"]["where"] == {"$ref": "#/$defs/Point"}
    assert definition["properties"]["route"]["items"] == {
        "$ref": "#/$defs/Point"
    }


@pytest.mark.parametrize(
    "input_schema",
    [
        # Requirements that span properties cannot be split into ports.
        {
            **_properties(a={"type": "string"}, b={"type": "string"}),
            "anyOf": [{"required": ["a"]}, {"required": ["b"]}],
        },
        {
            **_properties(a={"type": "string"}),
            "dependentRequired": {"a": ["b"]},
        },
        # Nor can an object whose declared properties are not the whole story.
        {"type": "object", "additionalProperties": {"type": "string"}},
    ],
)
def test_an_object_that_cannot_be_split_keeps_its_arguments_whole(input_schema):
    derived = action_schema_from_tool(_tool(inputSchema=input_schema))

    assert derived.whole_arguments == schemas.ARGUMENTS_INPUT
    assert derived.arguments == ()
    assert list(derived.schema.inputs) == [schemas.ARGUMENTS_INPUT]
    port = derived.schema.inputs[schemas.ARGUMENTS_INPUT]
    assert port.unary
    # Whole, so nothing the schema said is lost.
    for keyword in input_schema:
        assert keyword in _port_schema(port)


# -- results as ports ---------------------------------------------------------


def test_a_result_without_an_output_schema_is_text_and_content():
    derived = action_schema_from_tool(_tool())

    assert set(derived.schema.outputs) == {"text", "content"}
    assert (derived.text_output, derived.content_output) == ("text", "content")
    assert derived.structured_output is None
    assert not derived.schema.outputs["text"].unary
    assert derived.schema.outputs["text"].type == "text/plain"
    assert not derived.schema.outputs["content"].unary
    # No mapping: a caller gets everything the tool returned.
    assert derived.schema.output_to_json_field == {}


def test_a_declared_output_schema_becomes_the_whole_result():
    output_schema = {
        "type": "object",
        "properties": {"total": {"type": "integer"}},
        "required": ["total"],
    }
    derived = action_schema_from_tool(_tool(outputSchema=output_schema))

    assert derived.structured_output == "structured_content"
    structured = derived.schema.outputs["structured_content"]
    assert structured.unary
    assert _port_schema(structured) == output_schema
    # Which is what MCP says a client should prefer, so it is what a model is
    # given as the tool's result.
    assert derived.schema.output_to_json_field == {
        "structured_content": a11.ActionSchema.WHOLE_JSON
    }


def test_a_result_port_moves_out_of_an_arguments_way():
    # A tool with a `text` argument is ordinary, and an input and an output of
    # the same name would be one node.
    derived = action_schema_from_tool(
        _tool(inputSchema=_properties(text={"type": "string"}))
    )

    assert derived.text_output == "text_2"
    assert set(derived.schema.inputs) == {"text"}
    assert set(derived.schema.outputs) == {"text_2", "content"}


def test_a_property_named_like_a_reserved_port_is_renamed():
    derived = action_schema_from_tool(
        _tool(inputSchema=_properties(__log__={"type": "string"}))
    )

    argument = derived.arguments[0]
    assert argument.property == "__log__"
    assert argument.port not in schemas.RESERVED_PORT_NAMES
    assert argument.port in derived.schema.inputs


# -- names, descriptions, headers ---------------------------------------------


@pytest.mark.parametrize(
    ("tool_name", "expected"),
    [
        ("search", "search"),
        ("github.create_issue", "github_create_issue"),
        ("files/read", "files_read"),
        ("-weird-", "weird"),
        ("ok-name", "ok-name"),
        ("!!!", "tool"),
        # A `__` prefix is A11's own mark, and an action wearing it would be
        # left out of the tools a model is offered.
        ("__list_actions__", "_list_actions__"),
    ],
)
def test_a_tool_name_becomes_an_a11_identifier(tool_name, expected):
    derived = action_schema_from_tool(_tool(name=tool_name))
    assert derived.action_name == expected
    assert derived.tool_name == tool_name
    # Renamed or not, the handler is told which tool to call.
    assert derived.schema.headers[McpHeaders.TOOL].default == tool_name.encode()


def test_two_tools_that_sanitise_alike_stay_apart():
    first = action_schema_from_tool(_tool(name="a.b"))
    second = action_schema_from_tool(
        _tool(name="a/b"), taken={first.action_name}
    )
    assert (first.action_name, second.action_name) == ("a_b", "a_b_2")
    assert second.schema.headers[McpHeaders.TOOL].default == b"a/b"


def test_a_prefix_keeps_two_servers_apart():
    derived = action_schema_from_tool(_tool(name="read"), prefix="fs_")
    assert derived.action_name == "fs_read"


def test_a_tool_with_no_name_is_refused():
    with pytest.raises(StatusException) as caught:
        action_schema_from_tool({"inputSchema": {}})
    assert caught.value.status.code == StatusCode.INVALID_ARGUMENT


def test_the_description_carries_the_title_and_the_hints():
    derived = action_schema_from_tool(
        _tool(
            title="Delete a file",
            description="Remove a path from the sandbox.",
            annotations={"destructiveHint": True, "openWorldHint": False},
        )
    )

    assert derived.schema.description.splitlines()[0] == "Delete a file"
    assert "Remove a path from the sandbox." in derived.schema.description
    assert "destructive" in derived.schema.description
    assert "closed set" in derived.schema.description


def test_a_title_that_repeats_the_description_is_not_repeated():
    derived = action_schema_from_tool(
        _tool(title="Search", description="Search the index.")
    )
    assert derived.schema.description == "Search the index."


def test_the_server_is_recorded_on_the_schema():
    derived = action_schema_from_tool(_tool(), server="https://example.com/mcp")
    header = derived.schema.headers[McpHeaders.SERVER]
    assert header.default == b"https://example.com/mcp"
    # And the standard headers are still there, so a deadline propagates.
    assert a11.DEFAULT_ACTION_HEADERS.keys() <= derived.schema.headers.keys()
    assert McpHeaders.META in derived.schema.headers


def test_a_derived_schema_survives_being_described_to_a_peer():
    derived = action_schema_from_tool(
        _tool(
            inputSchema={
                **_properties(
                    query={"type": "string"},
                    tags={"type": "array", "items": {"type": "string"}},
                ),
                "required": ["query"],
            },
            outputSchema={
                "type": "object",
                "properties": {"hits": {"type": "integer"}},
            },
        ),
        server="https://example.com/mcp",
    )

    document = describe.schema_to_json(derived.schema)
    restored = describe.schema_from_json(document)

    assert restored.inputs["query"].json_schema
    assert restored.inputs["tags"].unary is False
    assert restored.outputs["structured_content"].unary
    assert restored.output_to_json_field == {"structured_content": "$"}
    assert restored.headers[McpHeaders.TOOL].default == b"probe"
    assert (
        restored.headers[McpHeaders.SERVER].default
        == b"https://example.com/mcp"
    )


def test_the_derived_definition_matches_the_tool_it_came_from():
    # The round trip the port rules exist for: what a model is shown for the
    # Action is the tool's own argument schema.
    input_schema = {
        **_properties(
            query={"type": "string", "description": "Terms."},
            limit={"type": "integer", "minimum": 1, "default": 10},
            fields={"type": "array", "items": {"type": "string"}},
        ),
        "required": ["query"],
    }
    derived = action_schema_from_tool(_tool(inputSchema=input_schema))

    assert _definition(derived.schema)["input_schema"] == {
        "type": "object",
        "properties": {
            "query": {"type": "string", "description": "Terms."},
            "limit": {"type": "integer", "minimum": 1, "default": 10},
            "fields": {"type": "array", "items": {"type": "string"}},
        },
        "required": ["query"],
    }


def test_a_sdk_tool_model_reads_the_same_as_its_json():
    pytest.importorskip("mcp")
    from mcp_types import Tool

    document = _tool(
        description="Add.",
        inputSchema=_properties(a={"type": "integer"}),
        outputSchema={"type": "object", "properties": {}},
    )
    from_model = action_schema_from_tool(Tool.model_validate(document))
    from_json = action_schema_from_tool(document)

    assert from_model.schema == from_json.schema
