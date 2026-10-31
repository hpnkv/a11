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

"""Build instructions for composing the model's tools with Flow.

One text, in two shapes:
[get_system_prompt][a11.sdk.flow_tools.prompt.get_system_prompt] for a host
that composes a system prompt out of parts (the way ``a11 chat`` does with the
shell tools), and [get_skill][a11.sdk.flow_tools.prompt.get_skill] for one that
loads ``a11.sdk.skill.Skill``s. Both include the packaged compact Flow
reference used by the coding agent.

``SKILL.md`` beside this module is generated from these constants and checked
in, so a host that reads skills off disk gets the same words. The test suite
fails when the file and the constants disagree.
"""

from __future__ import annotations

import pathlib
from typing import TYPE_CHECKING

from a11.flow.reference import short_reference

if TYPE_CHECKING:
    from a11.sdk.skill import Skill

#: The generated ``SKILL.md``, for a host that loads skills from disk.
SKILL_MD_PATH = pathlib.Path(__file__).with_name("SKILL.md")

#: The skill's name, in the SKILL.md sense: lowercase, hyphenated, stable.
SKILL_NAME = "a11-flow"

#: What the skill is for, and when to reach for it.
SKILL_DESCRIPTION = (
    "Execute almost every tool operation through A11 Flow, including single"
    " reads, searches, edits, commands, and requests. Use one-step Flows for"
    " small operations and compose known chains or independent work in one"
    " call. Keep intermediate values out of model context and return bounded"
    " evidence for the next decision. The execution tool is flow_run;"
    " flow_actions and flow_check provide optional discovery and checking."
)

_BODY = """# Execute tools through Flow

Use `flow_run` for almost every executable tool operation, including one-step
work. Call underlying tools inside the Flow. After a result needs a fresh
decision, use another Flow for the next operation. Direct calls are reserved
for discovery/checks, user interaction, session controls, completion reporting,
tools unavailable through Flow, and explicit user requests. Never invoke a Flow
tool recursively from a Flow.

Use action contracts already in context. Discovery and compile-only checks are
optional: request missing contracts with `flow_actions(names: [...])`, or use
`flow_check` when compiler feedback would help. Execute with `flow_run`, which
compiles and checks permissions before dispatch. Fix clear compiler diagnostics
and retry the Flow; a correctable syntax error does not require direct tools.

Pass raw source and an `inputs` object keyed by the flow's input ports. Supply a
list for a stream port and one value for a unary port. The result is an object
keyed by the declared output ports; return only what the next decision needs.
Use `run` for actions marked `runnable: true`; `call` dispatches to the peer and
requires a stream supplied by the host.

The compact reference below is the primary language guide. Consult extended
guides only for a specific question it does not answer.
"""


def get_system_prompt() -> str:
    """Return the tool protocol and the complete compact Flow reference."""
    return _BODY + "\n\n" + short_reference()


def get_skill() -> "Skill":
    """The same instructions as an ``a11.sdk.skill.Skill``.

    Imported on demand because ``a11.sdk.skill`` needs PyYAML for frontmatter,
    while generating prompt text does not.
    """
    from a11.sdk.skill import Skill

    return Skill(
        name=SKILL_NAME,
        description=SKILL_DESCRIPTION,
        body=get_system_prompt(),
    )


__all__ = [
    "SKILL_DESCRIPTION",
    "SKILL_MD_PATH",
    "SKILL_NAME",
    "get_skill",
    "get_system_prompt",
]
