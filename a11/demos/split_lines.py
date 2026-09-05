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

"""One text in, one value per line out.

A model that has been asked for a list answers with lines, and a composition
that wants to fan out over them needs them as a *stream* -- one value per line,
so a `for` has something to iterate. Flow has no such stage on purpose: it
composes actions rather than growing a string library, so the primitive it lacks
is supplied the same way everything else is, as an action.

Registered by `a11.demos.web_demos_server` and used by `deep_research.flow`.
"""

from __future__ import annotations

import a11

SPLIT_LINES_SCHEMA = a11.ActionSchema(
    name="split_lines",
    description=(
        "Split one text into its non-empty lines, trimmed, one value per line."
    ),
    inputs={
        "text": a11.ActionPortSchema(
            name="text",
            type="text/plain",
            typeinfo=str,
            unary=True,
            required=True,
            description="The text to split.",
        )
    },
    outputs={
        "lines": a11.ActionPortSchema(
            name="lines",
            type="text/plain",
            typeinfo=str,
            required=True,
            description="The lines, in order, without the empty ones.",
        )
    },
)


async def split_lines(action: a11.Action) -> None:
    text = await action["text"].consume(str, allow_none=True) or ""
    lines = action["lines"]
    try:
        for line in text.splitlines():
            if stripped := line.strip():
                await lines.put(stripped)
    finally:
        await lines.finalize()
