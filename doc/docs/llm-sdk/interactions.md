# Understanding interactions

An `Interaction` is one durable step in a model
conversation. It can represent a user message, an assistant response, a set of
tool calls, or the tool results returned to the model. A list of interactions
therefore holds both the conversation and the work performed during it.

Provider APIs variously call this state messages, contents, responses, or
interaction history. A11 keeps the ordered turns in one application-owned
model while retaining provider-native content needed to continue a turn.

An interaction carries structured A11 values:

- `content` contains serialised text, images, or provider-native response data;
- `action_calls` and `action_inputs` describe actions requested by the model;
- `action_outputs` belongs to a new user interaction that returns requested
  action results to the model;
- `usage_metadata` records provider-independent token counts;
- `previous_interaction_id` links turns when a provider can resume server-side
  state.

## Create a portable user turn

The following envelope is understood by every included backend. `a11.to_chunk`
keeps the content within A11’s ordinary serialisation model.

```python
import a11
from a11.sdk.llm import Interaction, Role

question = Interaction(
    role=Role.USER,
    content=[
        a11.to_chunk(
            {
                "role": "user",
                "content": [
                    # Parts leave room for images or other content later.
                    {"type": "text", "text": "Which orders are delayed?"}
                ],
            }
        )
    ],
)
```

Keep the user interaction and the interactions emitted by the model in order.
Feed that list into the next call to continue the conversation:

```python
history: list[Interaction] = []
history.append(question)
history.extend(model_interactions)
```

When an assistant interaction requests actions, it remains unchanged. A11 runs
the calls and appends a separate user interaction containing their outputs;
the model continues from that new turn. The included `interact_with_*` actions
perform this step and emit both interactions on `new_interactions`.

Provider responses may retain their native content shape so no information is
lost. A11 also records which backend produced them and can normalise text,
images, tool calls, and tool results when a later turn switches provider.

## Treat interactions as application state

Interactions are Pydantic models, so they can be validated, copied, and stored
as JSON alongside the rest of an application’s state:

```python
# Persist after each turn; restore with Interaction.model_validate_json.
payload = interaction.model_dump_json()
restored = Interaction.model_validate_json(payload)
```

An interaction is the record of a step, not a live model connection. Streaming
tokens still travel through action output nodes; the completed interaction is
what you retain for the next step, auditing, or replay.
