# Copyright 2026 The A11 Authors.

"""An LLM-friendly Bash tool for A11.

Four Actions -- ``shell_start``, ``shell_execute``, ``shell_list`` and
``shell_exit`` -- let an agent open persistent, stateful shells and run commands
in them. Register them all on a registry with :func:`register`:

```python
from a11.actions import ActionRegistry
from a11.sdk import bash

registry = ActionRegistry()
bash.register(registry)
```

The command process is an ordinary ``bash`` subprocess (see
:class:`~a11.sdk.bash.shell.BashShell`) driven asynchronously, so it can later
be wrapped in kernel-level sandboxing without changing the Action surface.
"""

from a11.actions import ActionRegistry, ActionSchema, ActionHandler
from a11.sdk.bash.handlers import (
    shell_execute,
    shell_exit,
    shell_list,
    shell_start,
)
from a11.sdk.bash.manager import (
    GLOBAL_SCOPE,
    MAX_GLOBAL_SHELLS,
    MAX_SHELLS_PER_SESSION,
    ShellManager,
    get_shell_manager,
)
from a11.sdk.bash.prompt import get_system_prompt
from a11.sdk.llm import USER_FACING_LOG_PORT
from a11.sdk.bash.schemas import (
    SHELL_EXECUTE_SCHEMA,
    SHELL_EXIT_SCHEMA,
    SHELL_ID_HEADER,
    SHELL_LIST_SCHEMA,
    SHELL_START_SCHEMA,
    A11ShellExecuteParameters,
)
from a11.sdk.bash.shell import BashShell

#: The four (schema, handler) pairs, in protocol order.
SHELL_ACTIONS: tuple[tuple[ActionSchema, ActionHandler], ...] = (
    (SHELL_START_SCHEMA, shell_start),
    (SHELL_EXECUTE_SCHEMA, shell_execute),
    (SHELL_LIST_SCHEMA, shell_list),
    (SHELL_EXIT_SCHEMA, shell_exit),
)


def register(registry: ActionRegistry) -> None:
    """Register all four shell Actions on ``registry``."""
    for schema, handler in SHELL_ACTIONS:
        registry.register(schema.name, schema, handler)


__all__ = [
    "A11ShellExecuteParameters",
    "BashShell",
    "GLOBAL_SCOPE",
    "MAX_GLOBAL_SHELLS",
    "MAX_SHELLS_PER_SESSION",
    "SHELL_ACTIONS",
    "SHELL_EXECUTE_SCHEMA",
    "SHELL_EXIT_SCHEMA",
    "SHELL_ID_HEADER",
    "SHELL_LIST_SCHEMA",
    "SHELL_START_SCHEMA",
    "ShellManager",
    "USER_FACING_LOG_PORT",
    "get_shell_manager",
    "get_system_prompt",
    "register",
    "shell_execute",
    "shell_exit",
    "shell_list",
    "shell_start",
]
