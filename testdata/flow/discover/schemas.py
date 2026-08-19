# Copyright 2026 The A11 Authors.

"""What the Python side of the ActionSchema scanner is pinned against.

Not a module anybody imports: every declaration here is a shape
`cpp/a11/flow/discover.cc` has to read, including the ones it is meant to read
*badly*. `ActionSchema` is mentioned in this docstring on purpose -- a mention
inside a string is not a declaration, and the test says so.
"""

from __future__ import annotations

import a11

#: A module constant, which is how a real schema names a shared port.
NARRATION_PORT = "narration"

#: The name of the action below, bound to a name rather than written inline.
NAMED_ELSEWHERE = "reads-its-name-from-a-constant"


# The ordinary shape: keyword arguments, literal everything.
SIMPLE = a11.ActionSchema(
    name="simple",
    description="Return the input unchanged.",
    inputs={
        "text": a11.ActionPortSchema(
            name="text", type="text/plain", unary=True, required=True
        )
    },
    outputs={"out": a11.ActionPortSchema(name="out", type="text/plain")},
)


# Positional port arguments, a description written as adjacent literals, and a
# triple-quoted one that the reader should get back without the indentation this
# file put in front of it.
PROSE = a11.ActionSchema(
    name="prose",
    description=(
        "A description that outgrew its line, written as two literals next to"
        " each other, which is one string."
    ),
    inputs={
        "question": a11.ActionPortSchema(
            "question",
            "text/plain",
            description="""
            What to find out.

            A second paragraph, indented in the source and not in the text.
            """,
            required=True,
        ),
    },
    outputs={
        "answer": a11.ActionPortSchema("answer", "text/plain"),
        NARRATION_PORT: a11.ActionPortSchema(
            NARRATION_PORT,
            "text/plain",
            description="Narration for the person watching.",
        ),
    },
)


# A name that is a constant of this file. Resolvable, and the port keyed by a
# constant is resolvable for the same reason.
FROM_CONSTANT = a11.ActionSchema(
    name=NAMED_ELSEWHERE,
    description="Names itself with a constant declared above.",
    outputs={NARRATION_PORT: a11.ActionPortSchema(NARRATION_PORT, "text/plain")},
)


def _built_at_run_time(suffix: str) -> a11.ActionSchema:
    """A schema whose name cannot be read without running the code.

    The scanner is expected to find *nothing* here: an action with no name is
    one nothing can look up, so half an entry would be worse than none.
    """
    return a11.ActionSchema(
        name=f"computed-{suffix}",
        description="Not findable, and the test says so.",
    )


# A mention in a comment is not a declaration:
# ECHO = a11.ActionSchema(name="in-a-comment", description="Not real.")
