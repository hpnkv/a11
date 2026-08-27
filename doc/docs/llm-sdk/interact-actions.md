# Provide tools to interact_with_*

The included `interact_with_llm` action routes a conversation to Claude,
Gemini, or Ollama. Its `tools` input is a stream of provider-neutral tool
definitions. Bind the same registry to the action so requested names can be
resolved and run.

The handler owns the repeated model/tool loop: send definitions, receive tool
calls, run them, encode one result for every call, and ask the model to
continue. The application supplies policy and handlers without reconstructing
the different message shapes expected by each provider.

## Prepare the model action

```python
import asyncio
import os

import a11
from a11.sdk.interact_with_llm import (
    INTERACT_WITH_LLM_SCHEMA,
    interact_with_llm,
)
from a11.sdk.llm import LlmHeaders
from a11.sdk.llm_tools import runner

allowed = ["look_up_order"]
interact = (
    a11.Action(INTERACT_WITH_LLM_SCHEMA)
    .bind_handler(interact_with_llm)
    .bind_registry(registry)  # Contains LOOK_UP_ORDER and its handler.
    .set_header(LlmHeaders.PROVIDER.value, "gemini")
    .set_header(LlmHeaders.MODEL.value, "gemini-3.5-flash")
    .set_header(LlmHeaders.API_KEY.value, os.environ["GEMINI_API_KEY"])
    .set_header(LlmHeaders.ALLOWED_LLM_ACTIONS.value, ",".join(allowed))
    .run()
)

tool_definitions = runner.get_tool_definitions(registry, allowed)
```

## Feed the turn and tools

```python
interactions = interact["interactions"]
for previous in history:
    await interactions.put(previous)
await interactions.finalize(question)

await interact["config"].finalize()

tools = interact["tools"]
for definition in tool_definitions:
    await tools.put(definition)
await tools.finalize()
```

The handler sends those definitions to the chosen provider. If the model calls
one or more, the included runner executes the calls independently, returns each
result in the provider's expected shape, and continues until the model produces
an answer or the deadline ends. One failed call is reported without discarding
successful calls from the same response.

Sending the definitions is optional for actions the handler's own registry
already holds. Before the request, the handler collects the turn's tools with
`runner.collect_tools`: the definitions on the `tools` port that the allow-list
matches, **plus** every registered action name it matches that the caller did not
describe. So a remote caller that sets `x-a11-allowed-llm-actions` to
`shell_.*` is offered the server's shell tools without having to reproduce their
schemas, and a caller that does not is not offered them at all — the allow-list
is both the permission and the request.

Read visible text as it streams, and retain completed interactions separately:

```python
async def print_answer() -> None:
    async for text in interact["text_output"]:
        print(text, end="", flush=True)

print_task = asyncio.create_task(print_answer())
new_interactions = [item async for item in interact["new_interactions"]]
await print_task
await interact.wait()

# Persist both sides of the turn for the next request.
history.extend([question, *new_interactions])
```

The backend-specific actions (`interact_with_claude`,
`interact_with_gemini`, and `interact_with_ollama`) expose the same `tools`
port and registry pattern when direct provider control is preferable. The
routing action provides one application boundary: switching providers requires
only a header change and leaves the conversation flow unchanged.
