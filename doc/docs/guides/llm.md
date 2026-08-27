# Stream a model response

`interact_with_llm` presents Claude, Gemini, and Ollama through one action
contract. Feed it messages, generation settings, and optional tool definitions;
read the response while the model is generating it; retain the completed
interaction for the next turn.

Provider SDKs commonly expose one event stream containing text deltas,
reasoning, tool-call arguments, usage, and completion events. A11 separates
those concerns into named output ports. A chat UI can consume `text_output`, an
observability view can consume `thoughts` or `event_stream`, and conversation
storage can retain `new_interactions`. None has to inspect the others' events.

Alongside `import a11` this page needs a few names from the SDK, which is where
the model helpers live:

```python
import a11
from a11.sdk.interact_with_llm import (
    INTERACT_WITH_LLM_SCHEMA,
    interact_with_llm,
)
from a11.sdk.llm import Interaction, LlmHeaders, Role
```

## Build and start the action

Construct the action from its schema, bind the handler, and set the provider /
model / key as headers. `.run()` starts it in the background and hands back the
running action, whose ports you now read and write:

```python
import os

interact = (
    a11.Action(INTERACT_WITH_LLM_SCHEMA)
    .bind_handler(interact_with_llm)
    .set_header(LlmHeaders.PROVIDER.value, "gemini")
    .set_header(LlmHeaders.MODEL.value, "gemini-3.5-flash")
    .set_header(LlmHeaders.API_KEY.value, os.environ["GEMINI_API_KEY"])
    .run()
)
```

Ports are async nodes, reached with `interact["<port>"]` — the same
[`AsyncNode`][a11.nodes.async_node.AsyncNode] you met in
[streaming](streaming.md).

## Stream the reply as it arrives

The assistant's visible text lands, already extracted from the raw provider
events, on the `text_output` port. Draining it in a task lets tokens print while
the rest of the interaction is still in flight:

```python
async def stream_text():
    async for chunk in interact["text_output"]:
        print(chunk, end="", flush=True)


stream_task = asyncio.create_task(stream_text())
```

## Feed the conversation in

The input side takes three ports:

- `interactions` — the conversation so far, ending with the new user turn;
- `config` — model settings; close it empty to use backend defaults;
- `tools` — tool definitions; here there are none, so we close it empty.

An `Interaction` is a role plus content chunks:

```python
user_turn = Interaction(
    role=Role.USER,
    content=[a11.to_chunk({"role": "user",
                           "content": [{"type": "text", "text": "Hi!"}]})],
)

await interact["interactions"].finalize(user_turn)  # the input turn, ended
await interact["config"].finalize()   # empty: the backend's own defaults
await interact["tools"].finalize()    # close without tool definitions
```

`finalize()` on each port is what tells the handler that side is complete: it
marks the end of the data and closes the port. A port left open is a port the
handler waits on.

## Collect the result

The turns the model produced — its text, and any tool calls — arrive on
`new_interactions`. Read it to completion, then await the streaming task so the
last tokens have printed:

```python
new_interactions = []
async for interaction in interact["new_interactions"]:
    new_interactions.append(interaction)
await stream_task
await interact.wait()
```

Keep `new_interactions` around and prepend them (plus the user turn) to the next
call's `interactions` to carry the conversation forward.

An `Interaction` records assistant messages, tool calls, tool results, usage,
and provider continuation IDs. It is completed conversation state, not a live
stream or an agent checkpoint. Store it as ordinary application data; use the
output streams for content that must be rendered or processed incrementally.

## Putting it together

```python
import asyncio
import os

import a11
from a11.sdk.interact_with_llm import (
    INTERACT_WITH_LLM_SCHEMA,
    interact_with_llm,
)
from a11.sdk.llm import Interaction, LlmHeaders, Role


async def ask(text: str) -> list[Interaction]:
    interact = (
        a11.Action(INTERACT_WITH_LLM_SCHEMA)
        .bind_handler(interact_with_llm)
        .set_header(LlmHeaders.PROVIDER.value, "gemini")
        .set_header(LlmHeaders.MODEL.value, "gemini-3.5-flash")
        .set_header(LlmHeaders.API_KEY.value, os.environ["GEMINI_API_KEY"])
        .run()
    )

    async def stream_text():
        async for chunk in interact["text_output"]:
            print(chunk, end="", flush=True)

    async def collect_interactions():
        return [item async for item in interact["new_interactions"]]

    text_task = asyncio.create_task(stream_text())
    state_task = asyncio.create_task(collect_interactions())

    user_turn = Interaction(
        role=Role.USER,
        content=[a11.to_chunk({"role": "user",
                               "content": [{"type": "text", "text": text}]})],
    )
    await interact["interactions"].finalize(user_turn)
    await interact["config"].finalize()
    await interact["tools"].finalize()

    await text_task
    new_interactions = await state_task
    await interact.wait()
    return [user_turn, *new_interactions]


history = asyncio.run(ask("Say hello in three languages."))
```

`history` is ready for the next model call or application storage. The visible
text and durable conversation state travel independently, so neither needs to
be reconstructed from the other.

The full multi-turn, multi-provider version is `examples/002-llm-interactions`.

Next: the model call above ran **in your process**. See how to move it behind a
server you [call over the network](local-to-remote.md), then give the model a
[tool it can call back into](agent-tool.md).
