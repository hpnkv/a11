# Use an Action as an LLM tool

An LLM tool is an ordinary A11 [`Action`][a11.actions.action.Action]. Its schema
already gives the runtime everything a model needs: a stable name, a useful
description, typed inputs, and typed outputs. The LLM adapter converts that
native schema to JSON Schema; the action handler remains normal application
code.

This keeps one contract at four boundaries: direct application calls, model
function calling, remote action discovery, and Flow composition. Adding a tool
does not require a provider-specific wrapper around the underlying operation.

## Define the action

```python
import a11

LOOK_UP_ORDER = a11.ActionSchema(
    name="look_up_order",
    description="Return the current fulfilment status of an order.",
    inputs={
        "order_id": a11.ActionPortSchema(
            name="order_id",
            type="text/plain",
            typeinfo=str,
            required=True,
            description="The customer-visible order number.",
        )
    },
    outputs={
        "status": a11.ActionPortSchema(
            name="status",
            type="text/plain",
            typeinfo=str,
            required=True,
        )
    },
)

async def look_up_order(action: a11.Action) -> None:
    order_id = await action["order_id"].consume()
    result = await orders.fetch_status(order_id)  # Your application service.
    await action["status"].finalize(result)
```

Descriptions matter: they tell the model when the tool is relevant and what a
valid argument means. Keep them specific and avoid instructions that belong in
the system prompt.

## Convert the ActionSchema to JSON Schema

```python
from a11.sdk.llm_tools.adapter import ToolAdapter

adapter = ToolAdapter(LOOK_UP_ORDER)
input_schema = adapter.input_schema
```

`input_schema` is suitable for a provider’s function/tool declaration:

```json
{
  "type": "object",
  "properties": {"order_id": {"type": "string"}},
  "required": ["order_id"]
}
```

The adapter maps Pydantic models, enums, unions, collections, and annotated
field constraints, and places reusable definitions in a root `$defs`. Streaming
ports become arrays. Inputs marked for runtime autofill are omitted because the
model must not supply values such as identity or session context.

When the model calls the tool, the runner writes arguments through these same
typed ports. Invalid arguments fail as a structured action status, so the
provider adapter can return the error as that call's tool result.

For normal use, register the action and let the runner build the complete tool
definition:

```python
from a11.sdk.llm_tools import runner

registry = a11.ActionRegistry()
registry.register("look_up_order", LOOK_UP_ORDER, look_up_order)

tools = runner.get_tool_definitions(registry, ["look_up_order"])
# Each item contains name, description, and input_schema.
```
