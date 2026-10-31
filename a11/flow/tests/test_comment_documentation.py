# Copyright 2026 The A11 Authors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Leading comments supply native editor hints without schema descriptions."""

import pytest

from a11 import flow
from a11.actions import ActionRegistry

SOURCE = """# A record to render.
# Includes its display name.
struct Item {
  # The display name.
  title: string
}
# Render a record.
flow render {
  # The record to render.
  in value: Item
  # The rendered text.
  out result: string
  value.title -> result
}
"""


def request(method, source=SOURCE, **fields):
    answer = flow.request({"method": method, "source": source, **fields})
    assert answer["ok"], answer
    return answer["result"]


@pytest.mark.asyncio
async def test_comment_hints_leave_runtime_schemas_and_registries_unchanged():
    program = flow.loads(SOURCE)
    before = program.describe()
    registry = ActionRegistry()
    program.register_all(registry)
    for needle, expected in [
        ("Item {", "A record to render.\nIncludes its display name."),
        ("title:", "The display name."),
        ("value.title", "The record to render."),
        ("value.title ->", "The display name."),
        ("render {", "Render a record."),
        ("result:", "The rendered text."),
    ]:
        offset = SOURCE.index(needle)
        if needle == "value.title ->":
            offset += len("value.")
        hover = request("describe", offset=offset)
        assert hover["detail"] == expected
        assert expected in hover["markdown"]
    assert program.describe() == before
    described = before["structs"][0]
    assert not described["description"]
    assert not described["fields"]["title"]["description"]
    for schema in [program["render"].schema, registry.get_schema("render")]:
        assert not schema.description
        assert not schema.inputs["value"].description
        assert not schema.outputs["result"].description
    assert await program["render"].invoke(
        value={"title": "hello"}, registry=registry
    ) == {"result": "hello"}


def test_imported_aliases_keep_comments_in_editor_hints_only():
    source = """import "library" as lib
flow main {
  in item: lib::Item
  out text: string
  work = run lib::render(value: item)
  work.result -> text
}
"""
    library = SOURCE.replace("struct Item {", "struct Item exported {").replace(
        "flow render {", "flow render exported {"
    )
    fields = {"modules": {"library": library}, "path": "root.flow"}
    hover = request(
        "describe", source, offset=source.index("lib::render"), **fields
    )
    assert "Render a record." in hover["markdown"]
    outputs = request(
        "complete",
        source,
        offset=source.index("work.result") + len("work."),
        **fields,
    )
    output = next(
        item for item in outputs["proposals"] if item["name"] == "result"
    )
    assert "The rendered text." in output["documentation"]
    unfinished = source[: source.index("value: item")]
    arguments = request(
        "complete", unfinished, offset=len(unfinished), **fields
    )
    argument = next(
        item for item in arguments["proposals"] if item["name"] == "value"
    )
    assert "The record to render." in argument["documentation"]
    shape = request(
        "describe", source, offset=source.index("lib::Item"), **fields
    )
    assert "A record to render." in shape["markdown"]
    assert "The display name." in shape["markdown"]
    program = flow.loads(source, modules={"library": library})
    assert not program["main"].schema.description


def test_explicit_descriptions_take_precedence_over_comment_hints():
    source = SOURCE.replace(
        "flow render {",
        'flow render {\n  describe "Explicit flow description."',
    ).replace("title: string", 'title: string "Explicit field description."')
    hover = request("describe", source, offset=source.index("render {"))
    assert hover["detail"] == "Explicit flow description."
    hover = request("describe", source, offset=source.index("title:"))
    assert hover["detail"] == "Explicit field description."
