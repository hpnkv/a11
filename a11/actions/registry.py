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

        Or the schema alone, which says the action lives on a peer and is to be
        reached with a flow's `call` rather than run here:

        ```python
        registry.register("shell_execute", SHELL_EXECUTE)
        ```

    Each port carrying a `typeinfo` also gets a `json_schema` derived from it on
    the way in, because only Python can read a Python type. That happens here
    rather than when the action is described, so the answer a peer gets over the
    wire -- built by the native describer, which sees only what is on the schema
    -- is the same document a local caller gets. See
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

__all__ = ["ActionRegistry"]
