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

"""Public native continuation, option and editor contracts."""

import pytest

from a11 import flow
from a11.actions import ActionRegistry
from a11.flow.diagnostics import FlowSyntaxError
from a11.status import StatusException


@pytest.mark.asyncio
@pytest.mark.parametrize("slots", [False, True])
async def test_serial_chains_shared_handles_and_broadcast(slots):
    program = flow.loads(
        """
flow main {
  out left: int stream
  out right: int stream
  tail = 2 -> 1 -> left, right
  alias = tail
  middle = 3 -> alias
  4 -> middle
  wait tail
}
""",
        single_item_slots=slots,
    )
    assert await program.main.invoke(registry=ActionRegistry()) == {
        "left": [1, 2, 3, 4],
        "right": [1, 2, 3, 4],
    }
    evidence = program.describe()
    assert "serial_sources" in str(evidence)
    assert "all-registered-writers" in str(evidence)


@pytest.mark.asyncio
@pytest.mark.parametrize("payload", [None, False, 0, "", [], {}])
async def test_optional_binding_preserves_false_like_payloads(payload):
    program = flow.loads("""
flow main {
  in candidate: std::option[std::any]
  out payload: std::any
  out missing: bool
  if value = take candidate {
    value -> payload
  } else {
    true -> missing
  }
}
""")
    actual = await program.main.invoke(
        candidate={"has_value": True, "value": payload},
        registry=ActionRegistry(),
    )
    assert actual.get("payload", object()) == payload
    assert actual["missing"] is None


@pytest.mark.asyncio
async def test_direct_optional_reads_are_successive_and_else_if_is_lazy():
    program = flow.loads("""
flow main {
  in rows: int stream
  out values: int stream
  if first = try take rows {
    first -> values
  } else if fallback = try take rows {
    fallback -> values
  }
  wait all
  candidate = try take rows
  if second = candidate {
    second -> values
  }
  wait all
  if eof = try take rows {
    eof -> _
  }
}
""")
    assert await program.main.invoke(
        rows=[0, 2], registry=ActionRegistry()
    ) == {"values": [0, 2]}


@pytest.mark.asyncio
async def test_option_builder_and_transfer_builder_use_native_categories():
    expression = flow.Expression
    reference = expression.reference
    branch = flow.Statement.if_some(
        "item",
        expression.builtin(
            "std::option", [expression.literal({"has_value": True, "value": 0})]
        ),
        [flow.Statement.pipe(reference("item"), reference("selected"))],
    )
    chain = expression.literal(3).transfer(
        expression.literal(2).transfer(reference("ordered"))
    )
    binding = flow.Statement.bind("tail", chain)
    program = flow.loads(
        "flow main { out selected: int\n out ordered: int stream\n"
        + branch.source
        + "\n"
        + binding.source
        + "\n}"
    )
    assert await program.main.invoke(registry=ActionRegistry()) == {
        "selected": 0,
        "ordered": [2, 3],
    }


@pytest.mark.asyncio
async def test_upstream_failure_does_not_select_an_absence_branch():
    program = flow.loads("""
flow broken { out rows: int stream
  if true { fail unavailable "upstream failed" }
}
flow main {
  out absent: bool
  source = try run broken()
  if row = try take source.rows {
    row -> _
  } else {
    true -> absent
  }
}
""")
    with pytest.raises(StatusException):
        await program.main.invoke(registry=ActionRegistry())


def test_invalid_optional_bindings_and_sink_categories_are_diagnostics():
    for body in (
        "if item = {has_value: true, value: 0} { item -> _ }",
        "if item = {present: true, value: 0} { item -> _ }",
        "item = 1\nif item = std::option({has_value: false}) {}",
        "if item = std::option({has_value: false}) {}\nitem -> _",
        "tail = 1 -> _\ntail -> _",
        "tail = 1 -> _\n{value: tail} -> _",
        "tail = 1 -> _\nwait all\n2 -> tail",
    ):
        with pytest.raises(FlowSyntaxError):
            flow.loads("flow main {\n" + body + "\n}")


def test_native_editor_roles_and_partial_input():
    source = """
struct Row { name: string required }
flow main {
  in rows: Row stream
  out names: string stream
  std::duration timeout = 1s
  if row = try take rows {
    row.name -> names
  }
}
"""
    highlighted = flow.request({"method": "tokens", "source": source})["result"]
    type_at = source.index("std::duration")
    pieces = [
        token
        for token in highlighted["tokens"]
        if type_at <= token["start"] < type_at + len("std::duration")
    ]
    assert len(pieces) == 3
    assert all(token["kind"] == "builtin-type" for token in pieces)
    parsed = flow.request({"method": "parse", "source": source})["result"]
    assert "success-branch" in str(parsed)
    for tail in (
        "if row =",
        "if row = try take",
        "if row = try take rows { row.",
        "a -> b ->",
        "tail = a -> (b ->",
    ):
        response = flow.request(
            {"method": "check", "source": "flow main { " + tail}
        )
        assert response["result"]["format"] == "flow.diagnostics/v1"


def test_optional_payload_completion_is_typed_and_branch_local():
    prefix = (
        "struct Row { name: string required }\n"
        "flow main { in rows: Row stream\n"
    )
    inside = prefix + "if row = try take rows { row."
    proposed = flow.request(
        {"method": "complete", "source": inside, "offset": len(inside)}
    )["result"]
    assert "name" in [item["name"] for item in proposed["proposals"]]
    outside = prefix + "if row = try take rows {}\n"
    proposed = flow.request(
        {"method": "complete", "source": outside, "offset": len(outside)}
    )["result"]
    assert "row" not in [item["name"] for item in proposed["proposals"]]


def test_option_schema_uses_only_the_presence_field():
    response = flow.request(
        {
            "method": "schema",
            "source": "struct Choice { item: std::option[int] required }",
        }
    )["result"]
    assert "has_value" in str(response)
    assert "'present'" not in str(response)


@pytest.mark.asyncio
async def test_optional_binding_accepts_typed_parallel_loop_items():
    program = flow.loads("""
struct Row { name: string required }
flow main {
  in candidates: std::option[Row] stream
  out names: string stream
  for candidate in candidates | where true parallel 2 {
    if row = candidate {
      row.name -> names
    }
  }
}
""")
    actual = await program.main.invoke(
        candidates=[
            {"has_value": True, "value": {"name": "one"}},
            {"has_value": False},
            {"has_value": True, "value": {"name": "two"}},
        ],
        registry=ActionRegistry(),
    )
    assert sorted(actual["names"]) == ["one", "two"]
