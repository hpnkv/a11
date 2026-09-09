# Interactions

An `Interaction` is one durable step in a model conversation. It can represent a user message, an assistant response, a
set of tool calls, or the tool results returned to the model. A list of interactions therefore holds both the
conversation and the work performed during it.

Provider APIs variously call this state messages, contents, responses, or interaction history. A11 keeps the ordered
turns in one application-owned model while retaining provider-native content needed to continue a turn.

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
                    {"type": "text", "text": "Which orders are delayed?"}
                ],
            }
        )
    ],
)
```

Keep the user interaction and the interactions emitted by the model in order. Feed that list into the next call to
continue the conversation:

```python
history: list[Interaction] = []
history.append(question)
history.extend(model_interactions)
```

When an assistant interaction requests actions, it remains unchanged. A11 runs the calls and appends a separate user
interaction containing their outputs; the model continues from that new turn. The included `interact_with_*` actions
perform this step and emit both interactions on `new_interactions`.

Provider responses may retain their native content shape so no information is lost. A11 also records which backend
produced them and can normalise text, images, tool calls, and tool results when a later turn switches provider.

## Treat interactions as application state

Interactions are Pydantic models, so they can be validated, copied, and stored as JSON alongside the rest of an
application’s state:

```python
payload = interaction.model_dump_json()
restored = Interaction.model_validate_json(payload)
```

An interaction records one step; it is not a live model connection. Streaming tokens travel through action output nodes.
The completed interaction supports the next step, auditing, and replay.

## Read the schema

`Interaction` is registered as `a11.sdk.Interaction`. Its fields use A11 wire types, so the same record can cross
Python, TypeScript, and native action boundaries. In Python, its effective shape is:

```python
Interaction(
    id: str,
role: Role,
created_at_millis: int | None,
previous_interaction_id: str,
model: str,
status: Status | None,
system_instructions: list[Chunk],
action_configs: dict[str, A11ActionConfig],
content: list[Chunk],
action_calls: list[ActionMessage],
action_inputs: dict[str, list[NodeFragment]],
action_outputs: dict[str, list[NodeFragment]],
backend_specific_metadata: dict[str, bytes],
usage_metadata: UsageMetadata | None,
)
```

The identity and sequencing fields place the interaction in a conversation:

| Field                     | Default     | Meaning                                                                                                                                          |
|---------------------------|-------------|--------------------------------------------------------------------------------------------------------------------------------------------------|
| `id`                      | A new UUID  | Identifies this interaction and supplies the target for `previous_interaction_id`.                                                               |
| `role`                    | `Role.USER` | Identifies the producer: `user`, `model`, or `system`. Assistant interactions use `Role.ASSISTANT`, whose wire value is `model`.                 |
| `created_at_millis`       | `None`      | Records creation time as milliseconds since the Unix epoch when the producer supplies it.                                                        |
| `previous_interaction_id` | `""`        | Links to the preceding interaction. Stateful providers can use this link to resume server-side conversation state.                               |
| `model`                   | `""`        | Names the model that produced the interaction. User and tool-result interactions commonly leave it empty.                                        |
| `status`                  | OK          | Records the outcome of this conversational step. A failed status remains data about the turn; deserialisation reports its own status separately. |

The prompt and response fields carry model-facing data:

| Field                 | Default | Meaning                                                                                                                                                                          |
|-----------------------|---------|----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `system_instructions` | `[]`    | Holds system prompt material as chunks, separate from conversational content.                                                                                                    |
| `action_configs`      | `{}`    | Maps an action name to routing and header autofill settings. Each `A11ActionConfig` selects a peer and supplies binary headers needed when that action runs.                     |
| `content`             | `[]`    | Holds text, images, or provider-native response structures as chunks. Each chunk's metadata describes how to decode its bytes. Consecutive chunks may omit a repeated MIME type. |

Tool work uses the action protocol directly, with `ActionMessage` and
`NodeFragment` as its records:

| Field            | Default | Meaning                                                                                                                                                                                     |
|------------------|---------|---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `action_calls`   | `[]`    | Lists the `ActionMessage` values requested by the model. Each message identifies the action call, action name, port mappings, and call headers.                                             |
| `action_inputs`  | `{}`    | Maps an action-call ID to the `NodeFragment` values written to that call's input ports. Fragment IDs select concrete port nodes; sequence and continuation fields preserve streaming order. |
| `action_outputs` | `{}`    | Maps an action-call ID to output fragments returned by the action. These usually appear on a separate user interaction that follows the assistant's call request.                           |

The remaining fields retain accounting and provider details:

| Field                       | Default | Meaning                                                                                                                                                                            |
|-----------------------------|---------|------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `backend_specific_metadata` | `{}`    | Stores opaque byte values needed by one backend, such as its producer name, stop reason, container ID, or resumable session ID. JSON serialisation represents the bytes as base64. |
| `usage_metadata`            | `None`  | Stores provider-independent input, output, total, cached-input, cache-write, and reasoning token counts. Unreported counters remain `None`.                                        |

The schema permits content, action calls, and status information in the same interaction. `role` records provenance;
every role admits the same payload fields. The ordered interaction list remains the conversation record, while
`previous_interaction_id` makes the link explicit for backends that address stored state by ID.
