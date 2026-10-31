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

"""Native formatting and execution agree across equivalent whitespace."""

import pytest

from a11 import flow


def format_source(source):
    result = flow.request({"method": "format", "source": source})["result"]
    assert not result["diagnostics"]
    return result["formatted"]


@pytest.mark.asyncio
@pytest.mark.parametrize("gap", [" ", "\t", "\n", "\r\n  "])
async def test_whitespace_variants_format_and_execute_alike(gap):
    source = """mod helpers {
flow double exported { in value: int; out result: int;
value | map it + it -> result; }
}
flow main { in value: int; out result: int;
work = run helpers::double(value: value); work.result -> result; }
"""
    tokens = flow.request({"method": "tokens", "source": source})["result"][
        "tokens"
    ]
    variant = gap.join(
        source[token["start"] : token["end"]]
        for token in tokens
        if token["lexical"] not in {"newline", "eof"}
    )
    expected = format_source(source)
    assert format_source(variant) == expected
    assert format_source(expected) == expected
    assert ";" not in expected
    assert "mod helpers {\nflow double" in expected
    program = flow.loads(variant)
    assert await program.main.invoke(value=7) == {"result": 14}


def test_formatting_preserves_comment_hint_ownership():
    source = """# A detached comment.

struct Detached { orphan: string }
# A record comment.
struct Record {
# The field comment.
value: string
}
# The flow comment.
flow main {
# The input comment.
in value: Record
# The output comment.
out result: string
value.value -> result
}
"""
    formatted = format_source(source)
    assert format_source(formatted) == formatted
    for text in [source, formatted]:
        for needle, expected in [
            ("Detached {", ""),
            ("Record {", "A record comment."),
            ("value: string\n}", "The field comment."),
            ("main {", "The flow comment."),
            ("value: Record", "The input comment."),
            ("result: string", "The output comment."),
        ]:
            result = flow.request(
                {
                    "method": "describe",
                    "source": text,
                    "offset": text.index(needle),
                }
            )["result"]
            assert result["detail"] == expected
        program = flow.loads(text)
        assert not program.main.schema.description
        assert not program.main.schema.inputs["value"].description
