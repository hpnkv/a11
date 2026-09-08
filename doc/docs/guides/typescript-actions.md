# Run actions and stream values in TypeScript

Use a local action when application work has a stable, schema-described
interface and may later move behind a session. Inputs and outputs remain
`AsyncNode` streams in either location.

## Define an action

Each port declares its wire representation, cardinality, and requirement. A
unary port carries at most one value. A streaming port carries an ordered
sequence.

```ts
import {
    ActionPortSchema,
    ActionRegistry,
    ActionSchema,
    cancelledError,
    okStatus,
    valueOrThrow,
} from '@curiositystack/a11';

const summarizeSchema = new ActionSchema({
    name: 'summarize',
    description: 'Summarize text as it arrives.',
    inputs: {
        documents: new ActionPortSchema({
            name: 'documents', type: 'text/plain', required: true,
        }),
    },
    outputs: {
        summary: new ActionPortSchema({
            name: 'summary', type: 'text/plain', unary: true, required: true,
        }),
    },
});
```

## Implement the handler

Iterate over a streaming input and finalize the output with its last value.
Returning a non-OK status ends the action and propagates that status to output
readers.

```ts
const summarize = async (action) => {
    const documents = valueOrThrow(await action.getInput('documents'));
    const paragraphs = [];
    for await (const document of documents) paragraphs.push(String(document));

    if (action.signal.aborted) return cancelledError('Summary cancelled.');

    const summary = valueOrThrow(await action.getOutput('summary'));
    valueOrThrow(await summary.finalize(paragraphs.join(' ').slice(0, 240)));
    return okStatus();
};

const registry = new ActionRegistry();
valueOrThrow(registry.register('summarize', summarizeSchema, summarize));
```

## Run and read the action

Create the action from its registry, start the handler, and write the input.
`run()` starts work and returns immediately. `wait()` covers handler completion,
output closure, and terminal status publication.

```ts
const action = valueOrThrow(registry.makeAction('summarize'));
valueOrThrow(action.run());

const documents = valueOrThrow(await action.getInput('documents'));
valueOrThrow(await documents.put('A stream can publish partial work.'));
valueOrThrow(await documents.finalize('The final write closes the input.'));

const summary = valueOrThrow(await action.getOutput('summary', false));
console.log(valueOrThrow(await summary.next()));
valueOrThrow(await action.wait());
```

Pass `false` as the second `getOutput` argument when reading a received output.
Binding that node to the action stream would send received fragments back to
their producer.

## Cancel nested work

A handler creates a child with `makeNested`. The child inherits the registry
and, by default, the parent's node map, session, transport, and framework
headers. Parent cancellation recursively cancels active children, including
children created with `propagateIo` set to `false`.

```ts
const child = valueOrThrow(action.makeNested('retrieve_context'));
valueOrThrow(child.run());

action.signal.addEventListener('abort', () => closeLocalResource(), {
    once: true,
});
```

Handlers should observe `action.signal` around browser APIs, provider requests,
and long local loops. Call `action.cancel()`, then await `action.wait()` when
resource teardown must finish before the surrounding task continues.

## Choose stream operations

- `put` appends one value.
- `finalize(value)` appends the final value and closes the writer.
- `next` reads the next value; `for await` reads values one-by-one.
- `nextChunk` and `nextFragment` retain wire metadata and sequence details.
- `abortWithStatus` ends the stream with a structured failure.
- `close` ends a writer that cannot mark a specific value as final.

See the [AsyncNode lifecycle](../lifecycles/async-node.md) for the distinction
between finality and closure.
