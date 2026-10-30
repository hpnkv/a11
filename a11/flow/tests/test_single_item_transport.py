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

"""Public configuration and native transport evidence."""

import asyncio

import pytest

from a11 import flow
from a11.actions import ActionRegistry
from a11.status import StatusException


@pytest.mark.asyncio
@pytest.mark.parametrize("enabled", [False, True])
async def test_slots_preserve_null_eof_fanout_and_replay(enabled):
    source = """
flow main {
 out result: object
 out replay: string stream
 nodes private {
 n = node()
 null -> n
 first = try take n
 second = try take n
 {first: first, second: second} -> result
 text = node()
 "one" -> text
 for i in [1, 2, 3] | flatten { text -> replay }
 }
}
"""
    program = flow.loads(source, single_item_slots=enabled)
    actual = await program.main.invoke(registry=ActionRegistry())
    assert actual == {
        "result": {
            "first": {"has_value": True, "value": None},
            "second": {"has_value": False},
        },
        "replay": ["one"] * 3,
    }


def test_explanation_uses_native_evidence_and_preserves_public_plan():
    source = """
flow main { out result: int stream
 nodes private {
  n = node()
  1 -> n
  n -> result
 }
}
"""
    baseline = flow.loads(source, single_item_slots=False)
    optimized = flow.loads(source)
    assert optimized.optimizations()["single_item_slots"]
    assert baseline.describe() == optimized.describe()
    for enabled, program in ((False, baseline), (True, optimized)):
        explanation = program.optimizations()
        assert explanation["format"] == "flow.optimizations/v1"
        assert explanation["single_item_slots"] is enabled
        node = next(
            ref
            for ref in explanation["flows"][0]["refs"]
            if ref["label"] == "n"
        )
        assert node["eligible"]
        assert node["upper"] == 1
        assert not node["exactly_one_on_success"]
        assert node["producer_pump"]
        assert node["status"] == ("applied" if enabled else "eligible")
        shared = flow.request(
            {
                "method": "optimizations",
                "source": source,
                "single_item_slots": enabled,
            }
        )["result"]
        assert shared["flows"] == explanation["flows"]
        assert "fixes" not in str(shared)
    invalid = flow.request(
        {
            "method": "optimizations",
            "source": source,
            "single_item_slots": "bad",
        }
    )
    assert not invalid["ok"]


@pytest.mark.asyncio
@pytest.mark.parametrize("enabled", [False, True])
async def test_eager_initialization_and_inactive_failures_keep_activation(
    enabled,
):
    source = """
flow main {
 out result: int
 if false { unused = take ([] | flatten) }
 value = 1
 wait all
 value = value + 1
 value -> result
}
"""
    program = flow.loads(source, single_item_slots=enabled)
    assert await program.main.invoke(registry=ActionRegistry()) == {"result": 2}
    bad = flow.loads(
        "flow main { unused = take ([] | flatten) }",
        single_item_slots=enabled,
    )
    with pytest.raises(StatusException):
        await bad.main.invoke(registry=ActionRegistry())


@pytest.mark.asyncio
@pytest.mark.parametrize("enabled", [False, True])
async def test_program_interpreter_accepts_transport_option(enabled):
    result = await asyncio.to_thread(
        flow.run_program,
        "flow { out exit_code: int\n 0 -> exit_code }",
        standard_streams=False,
        single_item_slots=enabled,
    )
    assert result["exit_code"] == 0
