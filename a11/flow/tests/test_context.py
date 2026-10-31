"""Context lists cross the native runtime's Python host boundary."""

import pytest

import a11
from a11 import flow


@pytest.mark.asyncio
async def test_context_excerpts_and_the_list_prelude():
    program = flow.loads("""
flow excerpts {
  in lines: string stream

  out groups: list[string] stream

  list[int] counts = list([1, 1])
  lines | context std::contains(it, "error") before counts[0] after counts[1] -> groups
}
""")
    result = await program["excerpts"].invoke(
        {"lines": ["start", "error one", "error two", "end"]},
        registry=a11.ActionRegistry(),
    )
    assert result["groups"] == [["start", "error one", "error two", "end"]]
    unmatched = await program["excerpts"].invoke(
        {"lines": ["start", "end"]}, registry=a11.ActionRegistry()
    )
    assert unmatched["groups"] == []


@pytest.mark.asyncio
@pytest.mark.parametrize("base", [None, 0, -2])
async def test_context_numbered_objects_and_reset_indices(base):
    source = """
flow excerpts {
  in items: object stream

  out groups: list[object] stream

  items | context it.match before 1 after 2 numbered BASE -> groups
}
""".replace("BASE", "" if base is None else f"base {base}")
    program = flow.loads(source)
    rows = [
        {"match": index in [2, 4, 5], "data": [index, {"nested": True}]}
        for index in range(9)
    ]
    for _ in range(2):
        result = await program["excerpts"].invoke(
            {"items": rows}, registry=a11.ActionRegistry()
        )
        start = 1 if base is None else base
        assert result["groups"] == [
            [
                {"index": start + index, "value": rows[index]}
                for index in range(1, 8)
            ]
        ]
