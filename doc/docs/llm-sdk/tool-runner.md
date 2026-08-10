# Run actions requested by an interaction

When a model requests tools, its assistant `Interaction`
contains `action_calls` and the corresponding `action_inputs`. The included
runner turns those records back into live nested A11 actions, waits for them,
and returns output fragments. Those outputs belong on a **new user
interaction**, which follows the assistant interaction containing the calls.

```python
import a11

from a11.sdk.llm import Interaction, Role
from a11.sdk.llm_tools.runner import execute_actions_from_interaction

# `parent_action` is the running model action. Its registry and deadline are
# inherited by each nested tool call.
outputs = await execute_actions_from_interaction(
    assistant_interaction,
    parent_action,
)

tool_result_interaction = Interaction(
    previous_interaction_id=assistant_interaction.id,
    role=Role.USER,
    action_outputs=outputs,
    content=[
        a11.to_chunk(
            {
                "role": "user",
                # This adapter returns the provider's tool-result blocks.
                "content": await build_tool_result_blocks(outputs),
            }
        )
    ],
)
```

Do not mutate `assistant_interaction.action_outputs`. The assistant interaction
is the model's request to use tools; the new user interaction is the
application's response. Keeping them as separate, ordered turns preserves the
conversation structure expected by Claude and the other included handlers.

The runner performs the lifecycle work that is easy to get subtly wrong:

1. It checks every requested name against `x-a11-allowed-llm-actions`.
2. It resolves the schema and handler from the action registry.
3. It creates a nested action, preserves the model’s call ID, and propagates
   the parent deadline.
4. It streams input fragments to their ports while leaving autofilled ports to
   the runtime.
5. It waits for completion and maps output port names into the JSON-facing
   fields declared by the action schema.

The runner returns those fragments; it does not create the user interaction or
encode provider content. For example, `interact_with_claude` builds Anthropic
`tool_result` blocks, places them in a new `Role.USER` interaction alongside
`action_outputs`, emits that interaction, and only then asks Claude to
continue.

## Narrate a run without telling the model

An action may declare an output port named `user_facing_log`
(`a11.sdk.llm.USER_FACING_LOG_PORT`). It carries the tool's own account of what
it did, written for the person watching: a summary line, then whatever detail is
worth reading. The shell tools in `a11.sdk.bash` all declare one.

The runner drains that port like any other — an unread port would stall the
action writing it — but keeps it out of the tool result the model is shown, and
returns it as that call's log instead:

```python
executed = await execute_actions_from_interaction(assistant_interaction, action)

executed.logs            # {tool call id: log}, only for calls that wrote one
executed.log_metadata()  # {"tool_logs": b'{"call id": "..."}'}, or {}
```

Merge `log_metadata()` into the `backend_specific_metadata` of the interaction
carrying the tool results, which is what the included handlers do. Metadata is
the one part of an interaction no backend turns into provider content, so the
log stays out of the model's context while still being stored with the
conversation and available to a replayed transcript.

## Make the allow-list explicit

The registry defines what the application *can* run. The header defines what
this particular model call *may* run. Both are required boundaries.

```python
from a11.sdk.llm import LlmHeaders

parent_action.set_header(
    LlmHeaders.ALLOWED_LLM_ACTIONS.value,
    # Patterns are full-match regular expressions.
    r"look_up_order|search_catalog",
)
```

Do not build this value from untrusted model output. Choose it from application
policy, user permissions, and the needs of the current workflow. A call outside
the allow-list fails with `PERMISSION_DENIED` before an action is started.

Most applications do not call `execute_actions_from_interaction` directly:
the included `interact_with_*` handlers call it while continuing the model/tool
loop. Use it directly when building a custom model provider or an interaction
orchestrator with its own loop.
