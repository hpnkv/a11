# Build an LLM client with TypeScript

The TypeScript SDK utilities share conversation records with Python model
backends and turn those records into renderer-independent blocks. They also
translate A11 actions into model tools and run requested tools as nested
actions.

## Create portable interactions

`makeTextMessageInteraction` builds the tagged `Interaction` expected by A11's
strict interaction ports. `parseInteraction` validates stored or received
field maps, including interactions whose own status records a failed turn.

```ts
import {
    Role,
    makeTextMessageInteraction,
    parseInteraction,
    valueOrThrow,
} from '@curiositystack/a11';

const request = valueOrThrow(await makeTextMessageInteraction(
    'Compare the two deployment logs.',
    'Cite the exact timestamps.',
    Role.USER,
));

const restored = valueOrThrow(parseInteraction(JSON.parse(storedJson)));
```

Interactions carry content chunks, action calls and their fragments, usage,
provider metadata, and a terminal status. Serialization tags preserve the same
models when a conversation moves between TypeScript and Python.

## Expose actions as tools

`ToolAdapter` derives a model tool from an `ActionSchema`. TypeScript runtime
types do not retain a Python-style `typeinfo`, so pass Zod schemas for port
values that need a shape more specific than their MIME type.

```ts
import {z} from 'zod';
import {getToolDefinitions, valueOrThrow} from '@curiositystack/a11';

const tools = valueOrThrow(getToolDefinitions(
    registry,
    ['search_documents'],
    {
        search_documents: {
            query: z.string().min(3),
            limit: z.number().int().min(1).max(20),
        },
    },
));
```

`executeActionsFromInteraction` checks the action allow-list, creates nested
actions, writes the model's input fragments, waits for completion, and returns
output fragments keyed by tool-call ID. Cancelling the parent interaction
cancels these tool actions recursively.

## Render live and stored turns

`PresentationReducer` accepts live text, thought, and interaction events. Its
blocks contain no DOM or terminal assumptions. A renderer switches on
`BlockKind` and chooses its own components.

```ts
const reducer = new PresentationReducer({
    onBlockOpened: (block) => view.open(block.kind),
    onBlockAppended: (_block, delta) => view.append(delta),
    onBlockClosed: () => view.commit(),
});

reducer.onText('Searching');
reducer.onText(' three indexes…');
await reducer.onInteraction(response);
reducer.endTurn();
```

Use `presentConversation(interactions)` for stored history. It associates tool
logs with calls, omits system and tool-result carrier interactions, and emits
text, images, tool runs, failures, and usage blocks.

## Run a model in the browser

`interactWithGemma` runs the Gemma WebGPU backend and uses the same interaction
ports as remote providers. Configure its model assets and runtime URLs with
`gemmaConfigSchema`; stream text and interactions through the action outputs.

For remote providers, send the portable interactions to a Python A11 backend.
The [browser tools guide](browser-tools.md) shows a model calling back into page
handlers, while [chat sessions](chat-sessions.md) shows storage and replay.
