# Copyright 2026 The A11 Authors.

"""Shapes: what a `struct` gives a flow, and what it gives Python.

The language half of this is pinned in C++ (`cpp/tests/flow_dto_test.cc` and
`cpp/tests/flow_shape_test.cc`), because that is where the one implementation
lives. What is left for here is the *boundary*: a value of a declared shape
should arrive in Python as an instance of a real pydantic model, and the plan
a Python caller reads should say what the shape holds.
"""

from __future__ import annotations

import json

import pytest

from a11 import flow
from a11.flow.plan import _model_for_dto
from a11.flow.tests.test_flow import registry, run_flow  # noqa: F401

SOURCE = """
struct Source {
  describe "Where an answer came from."

  id:    string required             "Stable id."
  url:   string required matching "^https?://"
  kind:  string one of ["page", "paper"] default "page"
  tags:  string[] unique
  rank:  number 0..1 default 0.5
  parent: Source
}

flow make-a-source {
  in  id:      string required
  out source:  Source
  out rendered: string

  made = node()
  Source{"id": id, "url": "https://example.test/x"} -> made
  made -> source
  made | text -> rendered
}
"""


def test_a_shape_is_in_the_plan_a_caller_reads():
    program = flow.loads(SOURCE, "shapes.flow")
    described = program.describe()
    assert [shape["struct"] for shape in described["structs"]] == ["Source"]

    shape = described["structs"][0]
    assert shape["description"] == "Where an answer came from."
    # The order fields were written in, beside them: a JSON object's keys have
    # no order a reader may rely on, and a shape's fields have one.
    assert shape["order"] == ["id", "url", "kind", "tags", "rank", "parent"]
    assert shape["fields"]["id"]["required"] is True
    assert shape["fields"]["url"]["pattern"] == "^https?://"
    assert shape["fields"]["kind"]["default"] == "page"
    assert shape["fields"]["tags"]["unique"] is True
    assert shape["fields"]["tags"]["element"] == "string"
    assert shape["fields"]["rank"]["range"] == {"minimum": 0, "maximum": 1}
    # A field naming another shape says so, so nothing downstream has to decide
    # whether a bare name is a shape again.
    assert shape["fields"]["parent"]["struct"] == "Source"
    assert shape["binary"] is False

    # And a port typed with one carries it by name.
    assert program["make-a-source"].describe()["outputs"]["source"]["type"] == "Source"


@pytest.mark.asyncio
async def test_a_shape_typed_port_carries_a_pydantic_model(registry):  # noqa: F811
    result = await run_flow(SOURCE, registry, id="a")
    source = result["source"]

    # Not a mapping that happens to have the right keys: an instance of a model
    # built from the shape, with everything a model gives.
    assert type(source).__name__ == "Source"
    assert source.id == "a"
    assert source.url == "https://example.test/x"
    # The defaults the shape gives were filled in on the way through.
    assert source.kind == "page"
    assert source.rank == 0.5
    assert source.parent is None
    assert source.model_dump()["id"] == "a"


def test_the_model_is_built_once_per_shape():
    described = {
        "struct": "Cached",
        "description": "",
        "order": ["a"],
        "fields": {"a": {"resolved": "string", "required": True, "description": ""}},
    }
    first = _model_for_dto(json.dumps(described))
    assert _model_for_dto(json.dumps(described)) is first
    # A stream of ten thousand records is one class, not ten thousand.
    assert first.__name__ == "Cached"


def test_a_shape_that_names_itself_gives_a_recursive_model():
    described = {
        "struct": "Node",
        "description": "",
        "order": ["name", "child"],
        "fields": {
            "name": {"resolved": "string", "required": True, "description": ""},
            "child": {
                "resolved": "Node",
                "struct": "Node",
                "required": False,
                "description": "",
            },
        },
    }
    model = _model_for_dto(json.dumps(described))
    built = model.model_validate({"name": "a", "child": {"name": "b"}})
    assert type(built.child) is model
    assert built.child.name == "b"


@pytest.mark.asyncio
async def test_one_value_reads_as_a_value_and_cuts_into_pieces(registry):  # noqa: F811
    """`let` both ways: a value to branch on, and a value to cut up."""
    result = await run_flow(
        """
        flow both {
          in  codes: number stream required
          in  body:  string stream required
          out ok:     bool
          out frames: string stream

          let code = codes
          code >= 200 and code < 300 -> ok

          let image = body
          image | chunk 4 -> frames
        }
        """,
        registry,
        codes=[204],
        body=["abcdefghij"],
    )
    assert result["ok"] is True
    assert result["frames"] == ["abcd", "efgh", "ij"]


def test_a_shape_and_a_json_schema_are_the_same_idea_both_ways():
    schemas = flow.request({"method": "schema", "source": SOURCE, "struct": "Source"})
    assert schemas["ok"], schemas
    schema = schemas["result"]["schemas"]["Source"]
    assert schema["type"] == "object"
    assert schema["required"] == ["id", "url"]
    assert schema["properties"]["url"]["pattern"] == "^https?://"
    assert schema["properties"]["tags"]["uniqueItems"] is True
    assert schema["properties"]["rank"]["minimum"] == 0
    # A shape that names itself is a reference into `$defs`, which is what lets
    # it round-trip at all.
    assert schema["properties"]["parent"]["$ref"] == "#/$defs/Source"

    back = flow.request({"method": "shapes", "schema": schema})
    assert back["ok"], back
    assert back["result"]["diagnostics"] == []
    # The answer is *source*: it can be pasted into a file, read and checked in.
    text = back["result"]["text"]
    assert "struct Source {" in text
    assert 'matching "^https?://"' in text
    assert "parent: Source" in text
    # And it compiles, which is the only proof that matters.
    assert flow.loads(text + "\nflow f { in x: Source required\n out y: string\n"
                      "  x.id -> y }\n", "back.flow")


def test_a_schema_the_language_did_not_write_is_read_as_far_as_it_goes():
    back = flow.request(
        {
            "method": "shapes",
            "name": "external-thing",
            "schema": {
                "type": "object",
                "required": ["name"],
                "properties": {
                    "name": {"type": "string", "maxLength": 40},
                    "count": {"type": "integer", "minimum": 1},
                    "when": {"type": "string", "format": "date-time"},
                    "blob": {"type": "string", "contentEncoding": "base64"},
                    "either": {"oneOf": [{"type": "string"}]},
                },
            },
        }
    )
    assert back["ok"], back
    text = back["result"]["text"]
    # A name that would not lex as one is repaired rather than refused.
    assert "struct ExternalThing {" in text
    assert "name:   string required ..40" in text
    assert "count:  integer 1.." in text
    # The two types JSON has no word for are read back from what says how to
    # read them.
    assert "when:   time" in text
    assert "blob:   bytes" in text
    # What could not be read at all keeps the loosest type there is, rather
    # than being left out of the shape entirely.
    assert "either: json" in text
    # And what had no Flow spelling is reported rather than silently dropped.
    assert any(
        "oneOf" in diagnostic["message"]
        for diagnostic in back["result"]["diagnostics"]
    )
