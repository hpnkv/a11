# Copyright 2026 The A11 Authors.

"""Flow as a capability you hand to a model, or to a client.

Three Actions -- ``flow_actions``, ``flow_check`` and ``flow_run`` -- let an
LLM compose the tools it already has into an [A11 Flow][a11.flow] and run the
composition as a single tool call. Register them on the registry that holds
those tools:

```python
from a11.actions import ActionRegistry
from a11.sdk import flow_tools

registry = ActionRegistry()
flow_tools.register(registry)
system_prompt = flow_tools.get_system_prompt()
```

The registry matters: a flow calls actions by name through the registry it is
run under, so these belong beside the actions they are meant to compose.

What this buys, and why it is worth a tool of its own: the values a composition
moves between steps never pass through the model. A flow that fetches four
pages and summarises them sends one summary back; the model reads the summary
and is never charged for the pages. Every ordinary sequence of tool calls pays
for each intermediate twice, once to read it and once to quote it into the next
call.

The instructions to put in front of the model come with it, as either
[get_system_prompt][a11.sdk.flow_tools.prompt.get_system_prompt] text or an
``a11.sdk.skill.Skill`` -- the same words, in whichever shape the host prefers.

A client is the other kind of caller, and it wants ``flow_run`` too -- with the
flow's ports as *nodes* rather than an object of values. Naming a port on
``input_streams`` leaves it open to write while the flow runs, and every output
is readable as it fills; the ids are derived from the call's own
([flow_input_node_id][a11.sdk.flow_tools.handlers.flow_input_node_id],
[flow_output_node_id][a11.sdk.flow_tools.handlers.flow_output_node_id]). That is
the same tool serving a caller that has a session, not a second one.
"""

from a11.actions import ActionHandler, ActionRegistry, ActionSchema
from a11.sdk.flow_tools.handlers import (
    FLOW_ACTION_SUFFIX,
    describe_composable_actions,
    flow_actions,
    flow_check,
    flow_input_node_id,
    flow_output_node_id,
    flow_run,
    verify_calls,
)
from a11.sdk.flow_tools.prompt import (
    SKILL_DESCRIPTION,
    SKILL_MD_PATH,
    SKILL_NAME,
    get_skill,
    get_system_prompt,
)
from a11.sdk.flow_tools.schemas import (
    FLOW_ACTIONS_SCHEMA,
    FLOW_CHECK_SCHEMA,
    FLOW_RUN_SCHEMA,
    FLOW_TOOL_NAMES,
)

#: The three (schema, handler) pairs, in the order a model needs them.
FLOW_ACTIONS: tuple[tuple[ActionSchema, ActionHandler], ...] = (
    (FLOW_ACTIONS_SCHEMA, flow_actions),
    (FLOW_CHECK_SCHEMA, flow_check),
    (FLOW_RUN_SCHEMA, flow_run),
)


def register(registry: ActionRegistry) -> None:
    """Register all three Flow Actions on ``registry``."""
    for schema, handler in FLOW_ACTIONS:
        registry.register(schema.name, schema, handler)


__all__ = [
    "FLOW_ACTION_SUFFIX",
    "FLOW_ACTIONS",
    "FLOW_ACTIONS_SCHEMA",
    "FLOW_CHECK_SCHEMA",
    "FLOW_RUN_SCHEMA",
    "FLOW_TOOL_NAMES",
    "SKILL_DESCRIPTION",
    "SKILL_MD_PATH",
    "SKILL_NAME",
    "describe_composable_actions",
    "flow_actions",
    "flow_check",
    "flow_input_node_id",
    "flow_output_node_id",
    "flow_run",
    "get_skill",
    "get_system_prompt",
    "register",
    "verify_calls",
]
