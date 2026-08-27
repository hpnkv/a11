# Run a model in the browser

The [Browser clients](browser-clients.md) guide calls a model hosted on a
server. This guide runs the model locally: `interact_with_gemma` loads a
[Gemma](https://ai.google.dev/gemma)-family model **into the page** and runs it
on the browser's GPU through [WebGPU](https://developer.mozilla.org/docs/Web/API/WebGPU_API).
Nothing leaves the device, and the reply streams onto an `AsyncNode` exactly as
a remote backend's would.

`interact_with_gemma` is an A11 action with the same interaction ports as the
other LLM backends: an `interactions` input, a unary `config` input, and
`text_output` / `new_interactions` outputs — so the code that drives it is the
same as for `interact_with_llm`. Its handler runs a local model instead of
calling an API.

!!! note "Before you start"

    Install `a11@npm:@curiositystack/a11`. Use a browser with WebGPU enabled and
    provide a hosted Gemma model asset (`.task` or `.litertlm`) that MediaPipe
    can load. Serve the large model file with permissive CORS. This setup does
    not require an API key or model server.

## Try it

Paste a hosted Gemma model URL and send a message. The first message downloads
and compiles the model in the browser. Later generation is local, and each reply
**streams token by token**. The browser cache avoids another download after a
reload. A WebGPU-capable browser is required.

<link rel="stylesheet" href="../assets/local-models.css">
<div id="gemma-demo" class="gemma-demo">
  <div class="gemma-toolbar">
    <input id="gemma-model" aria-label="Gemma model URL" value="https://huggingface.co/litert-community/gemma-4-E2B-it-litert-lm/resolve/main/gemma-4-E2B-it-web.litertlm?download=true">
  </div>
  <div id="gemma-status" class="gemma-status"></div>
  <div id="gemma-errors" class="gemma-errors" role="alert" aria-live="polite"></div>
  <section class="gemma-chat" aria-label="Local Gemma chat">
    <div id="gemma-messages" class="gemma-messages"></div>
    <form id="gemma-form" class="gemma-compose">
      <input id="gemma-input" aria-label="Message" autocomplete="off" placeholder="Say something...">
      <button type="submit">Send</button>
    </form>
  </section>
</div>
<script type="module" src="../assets/local-models.js"></script>

## 1. The action contract

Import the backend and SDK types. `INTERACT_WITH_GEMMA_SCHEMA` describes the
ports and registers like any other schema.

```ts
import {
    Action,
    ActionRegistry,
    INTERACT_WITH_GEMMA_SCHEMA,
    interactWithGemma,
    makeTextMessageInteraction,
    parseInteraction,
    isOk,
    StatusCode,
    type Status,
} from '@curiositystack/a11';

const need = <T>(value: T | Status): T => {
    if (!isOk(value)) throw new Error(`${StatusCode[value.code]}: ${value.message}`);
    return value as T;
};
```

## 2. Register and run the action locally

There is no session and no transport. Create the action from its schema, bind
the handler, and `run()` it. `run()` starts the handler on the same node map, so
the ports you open next are the very ones the handler reads and writes.

```ts
const registry = new ActionRegistry();
need(registry.register('interact_with_gemma', INTERACT_WITH_GEMMA_SCHEMA, interactWithGemma));

const action = need(Action.create(INTERACT_WITH_GEMMA_SCHEMA, {
    handler: interactWithGemma,
    registry,
}));
need(action.run());
```

## 3. Feed the conversation and the model URL

The `interactions` port takes the whole conversation; the unary `config` port
carries the browser-specific knobs, most importantly `model_asset_path` — the
URL of the Gemma model to download and run. `makeTextMessageInteraction`
builds a portable text turn.

```ts
const user = need(await makeTextMessageInteraction('Explain WebGPU in one sentence.'));

const interactions = need(await action.getInput('interactions'));
need(await interactions.finalize(user));

const config = need(await action.getInput('config'));
need(await config.finalize({}));
```

The default config loads Gemma 3n E2B from Hugging Face. Set
`model_asset_path` to select another compatible asset. The asset is fetched
with redirects followed (the
`resolve` URL 302s to a CDN), then handed to the runtime as bytes. The
downloaded bytes are stored in the browser's [Cache
Storage](https://developer.mozilla.org/docs/Web/API/CacheStorage), so a page
reload serves the model from disk instead of downloading it again. The first
turn triggers the download and WebGPU compilation; later turns reuse the loaded
model.

## 4. Stream the reply as it arrives

`text_output` is an `AsyncNode`. Reading it in a loop lets tokens appear the
moment the model produces them — `next()` returns each streamed piece and
`null` once the turn is complete.

```ts
const output = need(await action.getOutput('text_output', false));
let reply = '';
while (true) {
    const token = need(await output.next({timeoutMs: 120_000}));
    if (token === null) break;
    reply += token;
    render(reply); // append to your chat bubble
}
```

The completed assistant turn also lands, structured, on `new_interactions`.
Keep it and prepend it to the next `interactions` write to continue the
conversation:

```ts
const newInteractions = need(await action.getOutput('new_interactions', false));
const assistant = need(parseInteraction(need(await newInteractions.next())));
need(await action.wait(5_000));
history = [...history, user, assistant];
```

## 5. Swap the runtime (optional)

By default, the handler dynamically imports Google's MediaPipe `LlmInference`
task and runs it on WebGPU. Use `setGemmaEngineFactory` to cache a loaded model
across turns, report download progress, or provide another engine:

```ts
import {setGemmaEngineFactory, type GemmaEngine} from '@curiositystack/a11';

setGemmaEngineFactory(async (config) => {
    const engine: GemmaEngine = {
        async generate(prompt, onToken) {
            // stream pieces via onToken(delta); resolve with the full text,
            // or return a Status on failure — never throw.
            return '...';
        },
    };
    return engine;
});
```

A factory returns a `StatusOr<GemmaEngine>`. Return a status such as
`unavailableError(...)` on failure; the runtime aborts the output ports with
that status.

## 6. Display failures

Every A11 call returns a `StatusOr<T>`: success values pass `isOk`, failures
carry a code, message, and details. Convert them at the UI boundary — a missing
model URL, an absent WebGPU adapter, or a load error all arrive as ordinary
statuses, not exceptions:

```ts
try {
    await runTurn(text);
} catch (error) {
    errorRegion.textContent = error instanceof Error ? error.message : String(error);
}
```
