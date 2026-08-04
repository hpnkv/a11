# Actions

An [`Action`][a11.actions.action.Action] is a named, schema-described unit of
work whose typed ports are [nodes](nodes.md). Actions compose and stream.

## Action

Bind local work with
[`bind_handler`][a11.actions.action.Action.bind_handler] and start it with
[`run`][a11.actions.action.Action.run]. Input and output ports are live
[`AsyncNode`][a11.nodes.async_node.AsyncNode] objects throughout the run;
[`wait`][a11.actions.action.Action.wait] is the terminal status boundary.
Inside a handler, `make_nested` on
[`Action`][a11.actions.action.Action] preserves parent context and
[`call`][a11.actions.action.Action.call]
dispatches the child. Bind a [`Session`][a11.service.session.Session] first with
[`bind_session`][a11.actions.action.Action.bind_session] to make that dispatch
remote, and use [`cancel`][a11.actions.action.Action.cancel] when the caller no
longer needs the result.

::: a11.actions.action.Action

## ActionRegistry

[`register`][a11.actions.registry.ActionRegistry.register] publishes an async
handler under a schema name;
[`make_action`][a11.actions.registry.ActionRegistry.make_action] then creates a
correctly configured action without repeating the schema.

::: a11.actions.registry.ActionRegistry

## Schemas

::: a11.actions.action.ActionSchema

::: a11.actions.action.ActionPortSchema

::: a11.actions.action.ActionHeaderSchema

::: a11.actions.action.ActionSettings

## Header helpers

::: a11.actions.action.DefaultHeaders
