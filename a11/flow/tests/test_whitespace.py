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

"""Whitespace and explicit source boundaries use the native grammar."""

import asyncio

import pytest

from a11 import flow
from a11.actions import ActionRegistry, ActionSchema
from a11.status import StatusException


def _format(source):
    result = flow.request({"method": "format", "source": source})["result"]
    assert not result["diagnostics"]
    return result["formatted"]


@pytest.mark.asyncio
@pytest.mark.parametrize("separator", ["\n", ";", ";\n"])
async def test_explicit_terminators_preserve_concurrent_calls(separator):
    registry = ActionRegistry()
    started = [asyncio.Event(), asyncio.Event()]
    release = asyncio.Event()

    def handler(index):
        async def run(action):
            started[index].set()
            await release.wait()

        return run

    for index in range(2):
        name = f"gate{index}"
        registry.register(name, ActionSchema(name=name), handler(index))
    source = (
        "flow main {"
        + separator.join(["", "run gate0()", "run gate1()", "wait all", ""])
        + "}"
    )
    program = flow.loads(source)
    task = asyncio.create_task(program.main.invoke(registry=registry))
    try:
        await asyncio.wait_for(
            asyncio.gather(*(event.wait() for event in started)), 3
        )
    finally:
        release.set()
        await task
    formatted = _format(source)
    assert ";" not in formatted
    assert _format(formatted) == formatted


@pytest.mark.asyncio
@pytest.mark.parametrize("indent", ["", "  ", "\t\t"])
async def test_optional_failure_message_continues_without_indentation_policy(
    indent,
):
    source = (
        "flow main { if true { fail invalid_argument\n"
        + indent
        + '"Configuration is missing."\n} }'
    )
    with pytest.raises(StatusException) as raised:
        await flow.loads(source).main.invoke(registry=ActionRegistry())
    assert raised.value.status.message == "Configuration is missing."
    assert _format(_format(source)) == _format(source)


@pytest.mark.asyncio
async def test_grouped_newlines_preserve_short_circuiting_and_literal_values():
    source = r"""flow main {
  out yes: bool
  out no: bool
  out text: string
  (true
    or std::b64decode("!")) -> yes
  (false
    and std::b64decode("!")) -> no
  std
    ::strformat(
      "first;"
      # the comment separates source lines, not values
      " second %s",
      ({value: ["café", "other"]}
        .value[
          0
        ])
    ) -> text
}
"""
    expected = {"yes": True, "no": False, "text": "first; second café"}
    registry = ActionRegistry()
    assert await flow.loads(source).main.invoke(registry=registry) == expected
    formatted = _format(source)
    assert (
        await flow.loads(formatted).main.invoke(registry=registry) == expected
    )
    assert _format(formatted) == formatted


@pytest.mark.parametrize("terminator", ["\n", ";\n"])
def test_transfer_after_failure_keeps_a_separate_statement(terminator):
    source = (
        "flow main { out output: string\nfail invalid_argument"
        + terminator
        + '"Configuration is missing." -> output\n}'
    )
    syntax = flow.request({"method": "parse", "source": source})["result"]
    assert not syntax["diagnostics"]
    statements = syntax["flows"][0]["body"]
    assert [statement["kind"] for statement in statements] == ["fail", "pipe"]
    assert statements[0]["message"] is None
    assert _format(_format(source)) == _format(source)
