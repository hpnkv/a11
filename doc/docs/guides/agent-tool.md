# A toy agent with a tool

An agent is a model that can *act* — call functions you expose and use the
results. In A11 a tool is nothing new: it is an **action** you register, and
`interact_with_llm` lets the model call it mid-conversation. This page gives the
model a `get_weather` tool with a simulated response, building on the
[LLM interaction](llm.md).

```python
import os

import a11
from a11.sdk.interact_with_llm import (
    INTERACT_WITH_LLM_SCHEMA,
    interact_with_llm,
)
from a11.sdk.llm import Interaction, LlmHeaders, Role
from a11.sdk.llm_tools import runner
```

## Write the tool as an action

The handler reads its `location` input and writes a `report` — here a canned
string instead of a real API call:

```python
async def get_weather(action):
    location = await action["location"].consume()
    await action["report"].put(f"It is 22°C and sunny in {location}.")
    await action["report"].drain_and_close()
```

Its schema names the ports and describes them; the descriptions are what the
model sees when deciding whether and how to call the tool:

```python
GET_WEATHER = a11.ActionSchema(
    name="get_weather",
    description="Get the current weather for a location.",
    inputs={"location": a11.ActionPortSchema(
        name="location", type="text/plain", typeinfo=str, required=True,
        description="The city and state, e.g. San Francisco, CA.")},
    outputs={"report": a11.ActionPortSchema(
        name="report", type="text/plain", typeinfo=str, required=True,
        description="A short human-readable weather report.")},
)
```

## Register it

Put the tool in a registry so the interaction can dispatch it by name:

```python
registry = a11.ActionRegistry()
registry.register("get_weather", GET_WEATHER, get_weather)
```

## Let the model reach it

Three things connect the registry to the model. `bind_registry` makes the tool
dispatchable; the `ALLOWED_LLM_ACTIONS` header allow-lists which registered
actions the model may call; and the tool *definitions* are streamed in on the
`tools` port so the provider knows the tool's shape:

```python
interact = (
    a11.Action(INTERACT_WITH_LLM_SCHEMA)
    .bind_handler(interact_with_llm)
    .bind_registry(registry)
    .set_header(LlmHeaders.PROVIDER.value, "gemini")
    .set_header(LlmHeaders.MODEL.value, "gemini-3.5-flash")
    .set_header(LlmHeaders.API_KEY.value, os.environ["GEMINI_API_KEY"])
    .set_header(LlmHeaders.ALLOWED_LLM_ACTIONS.value, "get_weather")
    .run()
)

tool_definitions = runner.get_tool_definitions(registry, ["get_weather"])
```

## Run the turn

Feeding and reading are just like the [plain interaction](llm.md) — the only
addition is streaming the tool definitions onto the `tools` port:

```python
user_turn = Interaction(
    role=Role.USER,
    content=[a11.to_chunk({"role": "user", "content": [
        {"type": "text", "text": "What's the weather in Paris?"}]})],
)

async with (
    interact["interactions"] as interactions,
    interact["config"],
    interact["tools"] as tools,
):
    await interactions.put_final(user_turn)
    for tool in tool_definitions:
        await tools.put(tool)
    await tools.put_null_final()
```

When the model decides to call `get_weather`, `interact_with_llm` dispatches the
action **in this process**, streams its `report` back to the model, and lets the
model continue — so the final text on `text_output` already reflects the tool
result. You just read the output:

```python
async for chunk in interact["text_output"]:
    print(chunk, end="", flush=True)
```

## What just happened

The loop — model asks for a tool, the tool runs, its output goes back, the model
answers — is the whole of "being an agent". Because the tool is an ordinary
action:

- it could stream progress on `report` instead of returning one string;
- it could itself `.call()` a *remote* action (see
  [local to remote](local-to-remote.md)), so a client tool can front a real
  weather service;
- swapping the canned string for a live API call changes nothing about the
  wiring above.

The complete interactive version — multi-turn, multi-provider, with the tool
registry — is `examples/002-llm-interactions`.
