# Actions

An [`Action`][a11.actions.action.Action] is a named, schema-described unit of
work whose typed input and output ports are [nodes](nodes.md). Actions stream
inputs and outputs and execute locally or over remote sessions.

## Execution modes and completion

An action instance is one-shot. Configure its schema, collaborators, headers,
and port mappings before selecting one execution mode:

| Mode | Entry point | Execution location |
| --- | --- | --- |
| Local | `run()` | A bound handler in the current process |
| Remote | `call()` | A registered handler reached through a session or wire stream |

Both modes expose the same input and output nodes. A caller can start writing a
large input after dispatch and read output before the handler finishes. Moving
an operation to another process therefore changes its bindings, while its
schema and port I/O remain stable.

Starting and finishing are separate barriers. `run()` schedules a local
handler, and `call()` queues a remote control message. `wait_for_dispatch()`
reports whether a remote peer accepted the call. `wait()` reports the final
handler or remote status after output cleanup. See the
[Action lifecycle](../lifecycles/action.md) for cancellation, nested calls, and
the complete transition model.

## Port boundaries

Use one port for each result with an independent lifecycle or reader. A
model action can stream visible text while returning structured interaction
state on another port. An image action can stream progress separately from its
finished binary asset. This lets callers drain the outputs concurrently and
apply distinct types and size limits.

`unary=True` declares one complete logical value; other ports carry a sequence
of values. The underlying node type remains the same. Handlers mark semantic
completion with `finalize()`. Runtime cleanup closes open writers, but it does
not infer that a partial value is complete.

Read or explicitly discard every output a handler can produce. An undrained
output can apply backpressure to the producer and delay action completion. The
[streaming guide](../guides/streaming.md) covers node reads and the
[generative media guide](../guides/generative-media.md) shows concurrent
outputs with different representations.

## Action definitions

### Schema definitions

Actions declare typed ports via [`ActionSchema`][a11.actions.action.ActionSchema] and
bind execution handlers:

```python
import a11

SCHEMA = a11.ActionSchema(
    name="transform_text",
    description="Transform input text to uppercase.",
    inputs={
        "text": a11.ActionPortSchema(
            name="text", type="text/plain", typeinfo=str, required=True
        )
    },
    outputs={
        "result": a11.ActionPortSchema(
            name="result", type="text/plain", typeinfo=str, required=True
        )
    },
)

async def handler(action: a11.Action) -> None:
    text = await action["text"].consume()
    await action["result"].finalize(text.upper())

action = a11.Action(SCHEMA).bind_handler(handler).run()
await action["text"].finalize("hello world")
print(await action["result"].consume())
await action.wait()
```

### Annotation definitions

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
handlers for local execution and dispatch through networked services. The
schema provides discovery and validation; the handler supplies the local
implementation. A registry can expose the same contract to application calls,
LLM tools, remote peers, and Flow compositions.

```python
registry = ActionRegistry()
registry.register("transform_text", SCHEMA, handler)

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

Signed delegation proofs and connection-scoped authorization contexts use the
reserved action headers described in the
[authorization reference](authorization.md).

::: a11.actions.action.DefaultHeaders

::: a11.actions.action.DEFAULT_HEADERS
