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

"""Public facade for the native action registry."""

from __future__ import annotations

from collections.abc import Callable, Mapping
from typing import TYPE_CHECKING, Any

from a11 import _native
from a11.actions.action import ActionHeaderSchema
from a11.actions.annotated import DEFAULT_OUTPUT_NAME, action_from_callable
from a11.actions.describe import fill_json_schemas

from a11._native import ActionRegistry

if TYPE_CHECKING:
    # Stub-only names: `ActionHandler` and `NativeActionHandler` are aliases the
    # generated stub package declares, not runtime attributes of the extension.
    # `from __future__ import annotations` keeps the signature below from
    # needing them at run time, and the generated stub resolves them.
    from a11._native import ActionHandler, ActionSchema, NativeActionHandler

    # Use the qualified name required by the generated stub. Import only during
    # type checking because `a11.flow` imports this package at runtime.
    import a11.flow.plan

ActionRegistry.__module__ = __name__

_native_register = ActionRegistry.register


# The generated stub renders an attached method's annotations verbatim, so these
# stay spelled the way the native signature spelled them -- names the stub
# package can resolve -- rather than as `Any`.
def _register(
    self: ActionRegistry,
    action_name: str,
    schema: ActionSchema,
    handler: ActionHandler | NativeActionHandler | None = None,
) -> None:
    """Register an action with a schema and optional async handler.

    Examples:
        Publish an application handler under its schema name:

        ```python
        registry.register("summarise", SUMMARISE, summarise)
        ```

        Register a peer action's schema without a local handler:

        ```python
        registry.register("shell_execute", SHELL_EXECUTE)
        ```

    Ports with `typeinfo` receive a derived `json_schema` at registration.
    The native describer then returns the same schema to local and peer
    callers. See
    [a11.actions.describe][a11.actions.describe].
    """
    fill_json_schemas(schema)
    return _native_register(self, action_name, schema, handler)


ActionRegistry.register = _register


# The generated stub renders an attached method's annotations verbatim, so this
# signature stays inside what the stub package can resolve: ``output`` really
# takes a port name or an [OutputPort][a11.actions.annotated.OutputPort], which
# lives in a module the stub cannot name.
def _action(
    self: ActionRegistry,
    fn: Callable[..., Any] | None = None,
    *,
    name: str | None = None,
    description: str | None = None,
    output: Any = DEFAULT_OUTPUT_NAME,
    headers: Mapping[str, ActionHeaderSchema] | None = None,
) -> Callable[..., Any]:
    """Register a function as an Action, deriving both halves from it.

    The decorator form of
    [`action_from_callable`][a11.actions.annotated.action_from_callable]: the
    schema and the handler are built from the function's annotations and
    registered here, so the whole Action is the function and its signature.

    ```python
    registry = ActionRegistry()

    @registry.action
    async def summarise(
        document: str,
        style: Annotated[str | None, a11.InputPort(description="Tone.")] = None,
    ) -> str:
        # The docstring becomes the Action's description.
        return await model.summarise(document, style or "neutral")
    ```

    The function comes back unchanged, so it stays directly callable and
    testable; what was derived from it is on ``fn.action_schema`` and
    ``fn.action_handler``.

    Args:
        fn: The function to bind, when used bare as ``@registry.action``.
        name: Action name to register under; defaults to ``fn.__name__``.
        description: Action description; defaults to ``fn``'s docstring.
        output: Where ``fn``'s own result goes -- a port name, or an
            [`OutputPort`][a11.actions.annotated.OutputPort] to say more about
            that port. Ignored by a function that declares `OutputPort`
            parameters, which has no such result.
        headers: Extra header schemas to merge into the Action's, for headers
            the function does not itself take a parameter for.

    Returns:
        ``fn`` itself, or the decorator that will take it.
    """

    def decorate(target: Callable[..., Any]) -> Callable[..., Any]:
        schema, handler = action_from_callable(
            target,
            name=name,
            description=description,
            output=output,
            headers=headers,
        )
        self.register(schema.name, schema, handler)
        target.action_schema = schema
        target.action_handler = handler
        return target

    return decorate if fn is None else decorate(fn)


ActionRegistry.action = _action


def _one_flow(source: str, source_name: str) -> a11.flow.plan.FlowPlan:
    """Return the one named Flow declared by ``source``.

    Reject entry flows and files with multiple Flows. Use
    [`Program.register_all`][a11.flow.plan.Program.register_all] for the latter.
    """
    from a11.flow.plan import compile_source

    program = compile_source(source, source_name)
    if program.has_entry:
        raise ValueError(
            f"The flow source for {source_name!r} declares a `flow {{ ... }}`"
            " entry point, which has no name and so cannot be registered as an"
            " action. Give the flow a name."
        )
    if len(program.names) != 1:
        declared = ", ".join(program.names) or "none"
        raise ValueError(
            f"The flow source for {source_name!r} must declare exactly one flow"
            f" to be registered under one name (declared: {declared}). Use"
            " a11.flow.loads(...).register_all(registry) for a file of several."
        )
    return program[program.names[0]]


def _flow(
    self: ActionRegistry,
    source: str,
    *,
    name: str | None = None,
    source_name: str = "",
) -> a11.flow.plan.FlowPlan:
    """Register a Flow's declared ports and handler as an Action.

    The source declares the Action's ports and execution:

    ```python
    registry = ActionRegistry()

    greet = registry.flow('''
        flow greet {
          in  name:  string
          out reply: string
          "Hello, " then name then "!" -> reply
          drain reply
        }
    ''')
    ```

    Source must contain exactly one named Flow. Use
    [`register_all`][a11.flow.plan.Program.register_all] for multiple Flows.

    Args:
        source: The flow's text.
        name: Action name; defaults to the Flow's name. The schema uses it too.
        source_name: Source label in diagnostics, usually a filename. Defaults
            to the registered name.

    Returns:
        The compiled [FlowPlan][a11.flow.plan.FlowPlan] for direct execution.

    Raises:
        FlowSyntaxError: If the source will not compile.
        ValueError: If it is not exactly one named flow.
    """
    plan = _one_flow(source, source_name or name or "<flow>")
    schema = plan.schema
    if name and name != schema.name:
        # Keep the registered key and described schema name aligned.
        schema = schema.model_copy(update={"name": name})
    self.register(schema.name, schema, plan.handler)
    return plan


ActionRegistry.flow = _flow

__all__ = ["ActionRegistry"]
