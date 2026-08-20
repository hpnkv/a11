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
