# Copyright 2026 The A11 Authors.

"""Tests for the model's view of an action, `definition_from_schema`.

The interesting part is what happens to a port whose `json_schema` is a document
with its own `$defs`: a `$ref` resolves against the root of whatever document it
lands in, so embedding one verbatim under `properties` would leave the reference
dangling.
"""

import json

import pytest
from pydantic import BaseModel, Field

import a11
from a11.actions import describe
from a11.sdk.llm_tools.runner import definition_from_schema


class Address(BaseModel):
    city: str = Field(description="Town or city.")
    postcode: str = ""


class Customer(BaseModel):
    name: str
    home: Address
    work: Address | None = None


def _definition(schema: a11.ActionSchema) -> dict:
    """What a model is shown for ``schema``, via the described document."""
    registry = a11.ActionRegistry()
    registry.register(schema.name, schema)
    return definition_from_schema(describe.schema_to_json(schema))


def _resolve(document: dict, ref: str) -> dict:
    """Follow a root-relative `$ref`, or fail saying it dangles."""
    assert ref.startswith("#/$defs/"), ref
    defs = document.get("$defs", {})
    name = ref.removeprefix("#/$defs/")
    assert name in defs, f"{ref} does not resolve in {sorted(defs)}"
    return defs[name]


def test_model_typed_port_refs_resolve_at_the_document_root():
    schema = a11.ActionSchema(
        name="greet_customer",
        inputs={
            "customer": a11.ActionPortSchema(
                "customer",
                "application/json",
                unary=True,
                required=True,
                typeinfo=Customer,
            )
        },
    )
    definition = _definition(schema)["input_schema"]

    # The port's own `$defs` moved to the root, and the reference the port
    # carries resolves there.
    assert "$defs" not in definition["properties"]["customer"]
    customer = _resolve(
        definition, definition["properties"]["customer"]["$ref"]
    )
    assert _resolve(definition, customer["properties"]["home"]["$ref"]) == {
        "properties": {
            "city": {
                "description": "Town or city.",
                "title": "City",
                "type": "string",
            },
            "postcode": {"default": "", "title": "Postcode", "type": "string"},
        },
        "required": ["city"],
        "title": "Address",
        "type": "object",
    }


def test_a_definition_of_ports_sharing_a_definition_keeps_one_copy():
    schema = a11.ActionSchema(
        name="compare",
        inputs={
            "left": a11.ActionPortSchema(
                "left", "application/json", unary=True, typeinfo=Address
            ),
            "right": a11.ActionPortSchema(
                "right", "application/json", unary=True, typeinfo=Address
            ),
        },
    )
    definition = _definition(schema)["input_schema"]

    assert list(definition["$defs"]) == ["Address"]
    assert (
        definition["properties"]["left"]["$ref"]
        == definition["properties"]["right"]["$ref"]
    )


def test_a_streaming_port_of_a_model_keeps_its_array_wrapper():
    schema = a11.ActionSchema(
        name="greet_all",
        inputs={
            "customers": a11.ActionPortSchema(
                "customers",
                "application/json",
                required=True,
                typeinfo=Customer,
            )
        },
    )
    definition = _definition(schema)["input_schema"]

    customers = definition["properties"]["customers"]
    assert customers["type"] == "array"
    assert customers["minItems"] == 1
    assert definition["required"] == ["customers"]
    _resolve(definition, customers["items"]["$ref"])


def test_a_written_json_schema_is_shown_as_written():
    # A port whose schema was stated rather than derived -- an MCP tool's, say
    # -- is passed through, hoisting aside.
    schema = a11.ActionSchema(
        name="stated",
        inputs={
            "count": a11.ActionPortSchema(
                "count",
                "application/json",
                unary=True,
                json_schema=json.dumps({"type": "integer", "minimum": 1}),
            )
        },
    )
    definition = _definition(schema)["input_schema"]
    assert definition["properties"]["count"] == {
        "type": "integer",
        "minimum": 1,
    }


def test_an_untyped_port_is_still_an_object():
    schema = a11.ActionSchema(
        name="untyped",
        inputs={
            "anything": a11.ActionPortSchema(
                "anything", "application/json", unary=True
            )
        },
    )
    definition = _definition(schema)["input_schema"]
    assert definition["properties"]["anything"] == {"type": "object"}


@pytest.mark.parametrize("required", [True, False])
def test_autofilled_inputs_are_never_shown(required):
    schema = a11.ActionSchema(
        name="autofilled",
        inputs={
            "secret": a11.ActionPortSchema(
                "secret",
                "text/plain",
                unary=True,
                required=required,
                autofills=[
                    a11.NodeFragment(
                        continued=False, data=a11.to_chunk("s3cret")
                    )
                ],
            ),
            "query": a11.ActionPortSchema(
                "query", "text/plain", unary=True, typeinfo=str
            ),
        },
    )
    definition = _definition(schema)["input_schema"]
    assert list(definition["properties"]) == ["query"]
