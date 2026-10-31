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

"""Exercise native frame configuration and invocation isolation."""

import asyncio

import pytest

from a11 import flow
from a11.status import StatusCode, StatusException


@pytest.mark.asyncio
@pytest.mark.parametrize("cache_bytes", [0, 256 * 1024])
async def test_frame_reuse_preserves_values_and_independent_invocations(
    cache_bytes,
):
    program = flow.loads("""
flow main {
 in values: int stream
 out result: object stream
 for value in values parallel 4 {
   if value > 0 { {number: value, data: [value, value + 1]} -> result }
 }
}
""")
    main = program["main"]
    results = await asyncio.gather(
        *(
            main.invoke(
                {"values": list(range(seed * 16, seed * 16 + 16))},
                frame_cache_bytes=cache_bytes,
            )
            for seed in range(8)
        )
    )
    for seed, result in enumerate(results):
        expected = [
            {"number": value, "data": [value, value + 1]}
            for value in range(seed * 16, seed * 16 + 16)
            if value > 0
        ]
        assert sorted(result["result"], key=lambda v: v["number"]) == expected


@pytest.mark.asyncio
async def test_frame_budget_raises_status_exception():
    main = flow.loads("flow main { out result: int\n 42 -> result }")["main"]
    with pytest.raises(StatusException) as raised:
        await main.invoke(frame_budget_bytes=0)
    assert raised.value.status.code == StatusCode.RESOURCE_EXHAUSTED
    assert await main.invoke() == {"result": 42}


def test_interpreter_frame_budget_uses_status_boundary():
    with pytest.raises(StatusException) as raised:
        flow.run_program(
            "flow { out exit_code: int\n 0 -> exit_code }",
            standard_streams=False,
            frame_budget_bytes=0,
        )
    assert raised.value.status.code == StatusCode.RESOURCE_EXHAUSTED
    assert (
        flow.run_program(
            "flow { out exit_code: int\n 0 -> exit_code }",
            standard_streams=False,
            frame_cache_bytes=0,
        )["exit_code"]
        == 0
    )
