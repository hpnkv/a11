# Actions

An [`Action`][a11.actions.action.Action] is a named, schema-described unit of
work whose typed input and output ports are [nodes](nodes.md). Actions stream inputs
and outputs and can execute locally or over remote sessions.

## Defining and Running Actions

### Defining with Schemas

Actions declare typed ports via [`ActionSchema`][a11.actions.action.ActionSchema] and
bind execution handlers:

```python
import a11

SCHEMA = a11.ActionSchema(
    name="transform_text",
    description="Transforms input text to uppercase.",
    inputs={"text": a11.ActionPortSchema(name="text", type="text/plain", typeinfo=str, required=True)},
    outputs={"result": a11.ActionPortSchema(name="result", type="text/plain", typeinfo=str, required=True)},
)

async def handler(action: a11.Action) -> None:
    text = await action["text"].consume()
    await action["result"].finalize(text.upper())

# Run locally
action = a11.Action(SCHEMA).bind_handler(handler).run()
await action["text"].finalize("hello world")
result = await action["result"].consume()  # "HELLO WORLD"
await action.wait()
```

### Defining with Type Annotations

Declare action handlers directly with typed signatures:

```python
from a11.actions import ActionRegistry

registry = ActionRegistry()

@registry.action(name="summarize")
async def summarize(prompt: str) -> str:
    return f"Summary: {prompt[:50]}..."
```

A composition of actions needs no signature to read: a flow declares its own
ports and is its own handler, so registering one takes the text and nothing else.

```python
greet = registry.flow("""
flow greet {
  in  name:  string
  out reply: string
  "Hello, " then name then "!" -> reply
  drain reply
}
""")
```

::: a11.actions.action.Action

## ActionRegistry

[`ActionRegistry`][a11.actions.registry.ActionRegistry] manages action schemas and
handlers, supporting both local execution and dispatch through networked services:

```python
registry = ActionRegistry()
registry.register("transform_text", SCHEMA, handler)

# Construct configured actions
action = registry.make_action("transform_text")
```

::: a11.actions.registry.ActionRegistry

## Actions from annotations

::: a11.actions.annotated

::: a11.actions.annotated.action_from_callable

::: a11.actions.annotated.InputPort

::: a11.actions.annotated.OutputPort

::: a11.actions.annotated.Header

## Schemas

::: a11.actions.action.ActionSchema

::: a11.actions.action.ActionPortSchema

::: a11.actions.action.ActionHeaderSchema

::: a11.actions.action.ActionSettings

## Describing actions

::: a11.actions.describe

::: a11.actions.describe.schema_to_json

::: a11.actions.describe.schema_from_json

::: a11.actions.describe.registry_to_json

::: a11.actions.describe.schemas_in_document

::: a11.actions.describe.fill_json_schemas

::: a11.actions.describe.json_schema_for

::: a11.actions.describe.builtin_action_names

::: a11.actions.describe.is_reserved_action

## Header helpers

::: a11.actions.action.DefaultHeaders

::: a11.actions.action.DEFAULT_HEADERS
