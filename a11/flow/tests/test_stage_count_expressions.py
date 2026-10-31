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

"""Count stages accept scalar configuration through the native language."""

import pytest

from a11 import flow
from a11.status import StatusException

CASES = [
    ("first", "[1, 2, 3, 4] | flatten", [1, 2]),
    ("last", "[1, 2, 3, 4] | flatten", [3, 4]),
    ("drop", "[1, 2, 3, 4] | flatten", [3, 4]),
    ("batch", "[1, 2, 3, 4] | flatten", [[1, 2], [3, 4]]),
    ("window", "[1, 2, 3, 4] | flatten", [[1, 2], [2, 3], [3, 4]]),
    ("truncate", '["abcd", "efgh"] | flatten', ["ab", "ef"]),
    ("chunk", '"abcd"', ["ab", "cd"]),
]


@pytest.mark.asyncio
@pytest.mark.parametrize("stage, source, expected", CASES)
@pytest.mark.parametrize("slots", [False, True])
async def test_count_expressions_work_for_every_count_stage(
    stage, source, expected, slots
):
    for count in [
        "settings.count",
        "(settings.count + 0)",
        "(\nsettings.count\n)",
    ]:
        text = f"""flow main {{
  in settings: object
  out result: std::json stream
  {source} | {stage} {count} -> result
}}
"""
        formatted = flow.request({"method": "format", "source": text})["result"]
        assert not formatted["diagnostics"]
        for variant in [text, formatted["formatted"]]:
            program = flow.loads(variant, single_item_slots=slots)
            actual = await program.main.invoke(settings={"count": 2})
            assert actual == {"result": expected}
            assert "settings.count" in str(program.describe())


@pytest.mark.asyncio
@pytest.mark.parametrize("stage, source, _expected", CASES)
@pytest.mark.parametrize("count", [True, 2.5, "2", None, [], {}])
async def test_dynamic_counts_require_integers(stage, source, _expected, count):
    text = f"""flow main {{
  in settings: object
  {source} | {stage} settings.count -> _
}}
"""
    with pytest.raises(StatusException) as raised:
        await flow.loads(text).main.invoke(settings={"count": count})
    assert raised.value.status.code.name == "INVALID_ARGUMENT"
    assert "must be an integer" in raised.value.status.message


@pytest.mark.asyncio
@pytest.mark.parametrize("drop", ["loc.skip", "(loc.start - 1)"])
async def test_loop_counts_match_both_context_extraction_forms(drop):
    source = f"""flow contexts {{
  out snippets: object stream
  for loc in [{{start: 1, skip: 0, count: 2}},
              {{start: 3, skip: 2, count: 1}}] {{
    lines = ["a", "b", "c", "d"] | flatten
    text = lines | drop {drop} | first loc.count | join "\\n"
    {{line: loc.start, text: text}} -> snippets
  }}
}}
"""
    result = await flow.loads(source)["contexts"].invoke()
    assert result == {
        "snippets": [{"line": 1, "text": "a\nb"}, {"line": 3, "text": "c"}]
    }


@pytest.mark.asyncio
@pytest.mark.parametrize("slots", [False, True])
async def test_count_stream_is_read_once_per_activation(slots):
    source = """flow main {
  out result: int stream
  counts = node()
  2 -> counts
  [1, 2, 3, 4] | flatten | first (take counts) -> result
}
"""
    program = flow.loads(source, single_item_slots=slots)
    assert await program.main.invoke() == {"result": [1, 2]}
