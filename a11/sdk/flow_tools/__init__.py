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

"""Register Flow actions for a model or client.

``flow_actions``, ``flow_check`` and ``flow_run`` let a model compose tools in
an [A11 Flow][a11.flow]. Register them beside the actions they may call:

```python
from a11.actions import ActionRegistry
from a11.sdk import flow_tools

registry = ActionRegistry()
flow_tools.register(registry)
system_prompt = flow_tools.get_system_prompt()
```

Intermediate values stay inside the Flow; the model receives its outputs.
Use [get_system_prompt][a11.sdk.flow_tools.prompt.get_system_prompt] or the
``a11.sdk.skill.Skill`` for model instructions.

A client can use ``flow_run`` with live port nodes. Naming a port on
``input_streams`` leaves it open while the Flow runs; outputs can be read as
they fill. Node IDs come from
[flow_input_node_id][a11.sdk.flow_tools.handlers.flow_input_node_id] and
[flow_output_node_id][a11.sdk.flow_tools.handlers.flow_output_node_id].
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

#: The discovery, compile-only, and execution (schema, handler) pairs.
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
