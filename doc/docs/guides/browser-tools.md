# Let a model use tools in the browser

Browser-hosted actions let a model operate on state that exists only in the
page, such as a canvas, selection, or editor.

This guide serves three actions from a web page, registers them with the backend,
and lets a model use them. The page's handlers run in the page; the backend never
touches the canvas; and the model sees three ordinary A11 actions.

!!! note "Before you start"

    The demo talks to `wss://a11.services:9443/a11-demos`, which runs an Ollama
    beside itself, so the default needs no key; Claude and Gemini want one. To run
    the backend yourself:

    ```sh
    python -m a11.demos.web_demos_server   # ws://127.0.0.1:9010/a11-demos
    ```

    A page loaded over HTTPS may refuse a plaintext `ws://` socket even to
    localhost (Chrome allows it, Firefox does not), so give a local backend
    the `--certificate` / `--private-key` flags and a trusted certificate —
    [mkcert](https://github.com/FiloSottile/mkcert) makes one — if the
    browser blocks it.

    Use a model with reliable tool-schema support. The example has been tested
    with `glm-4.7-flash` on Ollama, Claude, and Gemini.

## Try it

Try "make blob 2 red and move it left", "spread them out", or "give them a warm
palette". The right pane logs every call the page served, including what each
tool narrated; the model's own sentence arrives underneath the canvas.

<link rel="stylesheet" href="../assets/web-demos.css">
<div id="tools-demo" class="a11-demo">
  <div class="a11-toolbar">
    <input id="tools-server" class="wide" aria-label="Demo server URL" value="wss://a11.services:9443/a11-demos">
    <select id="tools-provider" aria-label="Provider">
      <option value="ollama">Ollama</option>
      <option value="claude">Claude</option>
      <option value="gemini">Gemini</option>
    </select>
    <input id="tools-model" aria-label="Model" value="glm-4.7-flash">
    <input id="tools-api-key" type="password" aria-label="API key" placeholder="API key (Claude or Gemini)">
    <input id="tools-base-url" aria-label="Base URL" value="http://127.0.0.1:11434">
  </div>
  <div id="tools-errors" class="a11-errors" role="alert" aria-live="polite"></div>
  <div class="a11-panes tools">
    <section class="a11-pane" aria-label="The scene this page serves">
      <header>the page</header>
      <div class="a11-canvas-wrap">
        <canvas id="tools-canvas" width="620" height="300" aria-label="Five coloured blobs"></canvas>
      </div>
      <form id="tools-form" class="a11-compose">
        <input id="tools-input" aria-label="Instruction" autocomplete="off" placeholder="Make blob 2 red and move it left...">
        <button type="submit">Send</button>
      </form>
    </section>
    <aside class="a11-pane" aria-label="The model, and the calls served by this page">
      <header>the model</header>
      <div id="tools-messages" class="a11-messages"></div>
      <header>served here</header>
      <div id="tools-log" class="a11-log"></div>
    </aside>
  </div>
</div>
<script type="module" src="../assets/browser-tools.js"></script>

The page is
[`js/demo/browser_tools.ts`](https://github.com/hpnkv/a11/blob/main/js/demo/browser_tools.ts).
The IntelliJ plugin's webview does the same thing with the IDE's editor and index
instead of a canvas — see `intellij-plugin/webview/src/ideTools.ts`.

## 1. Ports are the model's arguments

An A11 action's tool definition is derived from its *ports*
([`ToolAdapter`](../llm-sdk/action-tools.md)): one port per argument,
a streaming port becoming an array. So the action is designed the way the model
should see it —

```ts
const SET_COLOR_SCHEMA = new ActionSchema({
    name: 'set_color',
    description: 'Recolour blobs: the i-th id is given the i-th colour.',
    inputs: {
        ids: new ActionPortSchema({name: 'ids', type: 'application/json', required: true,
            description: 'Which blobs to recolour.'}),
        colors: new ActionPortSchema({name: 'colors', type: 'text/plain', required: true,
            description: 'One `#rrggbb` per id, in the same order.'}),
    },
    outputs: {
        recoloured: new ActionPortSchema({name: 'recoloured', type: 'application/json',
            unary: true, required: true}),
    },
});
```

The schema exposes each input with its field description; it does not wrap them
in an opaque `request` object.

!!! warning "A TypeScript port has a MIME type, not a value type"

    `ToolAdapter` reads the MIME type, so an `application/json` port is described
    to the model as a bare `{"type": "object"}` and it will dutifully send
    `ids: [{...}, {...}]`. Pass the shape explicitly:

    ```ts
    const PORT_SCHEMAS = {
        set_color: {ids: z.number().int(), colors: z.string()},
        shift_position: {ids: z.number().int(), dx: z.number(), dy: z.number()},
    };
    ```

    Those go to `getToolDefinitions(registry, names, PORT_SCHEMAS)`. Python needs
    no equivalent: an `ActionPortSchema` there carries `typeinfo`.

## 2. Keep progress logs separate from results

A tool can report user-visible activity through `action.log()`. Because no port
declares this log, it does not become part of the model's tool result. The
backend's [tool runner](../llm-sdk/tool-runner.md) reads it separately, associates
it with the call ID, and records it in the turn metadata for later replay.

```ts
need(await action.log(`Recoloured ${recoloured} blob(s).`));
```

The log channel requires no declared port or result cleanup.

## 3. The page serves its actions

A handler in the page is a handler like any other: read the declared inputs,
do the work, write the declared outputs, close them.

```ts
const registry = new ActionRegistry();
need(registry.register(SET_COLOR_SCHEMA.name, SET_COLOR_SCHEMA, async (action) => {
    const [ids, colors] = await Promise.all([readAll(action, 'ids'), readAll(action, 'colors')]);
    const blobs = blobsFor(scene, ids);
    if (isStatus(blobs)) return await refuse(action, blobs, onLog);
    blobs.forEach((blob, index) => recolour(blob, colors[index]));
    const result = need(await action.getOutput('recoloured'));
    need(await result.finalize(blobs.length));
    return okStatus();
}));
```

### Validate, then act

Validate every model-supplied argument before modifying page state. Return a
status when the request cannot be applied:

```ts
const dx = rawDx === null ? 0 : finiteNumber(rawDx, 'dx', scene.width);
if (isStatus(dx)) return await refuse(action, dx, onLog);
```

Returning `invalidArgumentError('dx must be a number of pixels; got "a bit left".')`
is not a dead end — the tool runner hands it to the model as *this call's result*,
so the model sees what was wrong and can call again with a number. Coercing
instead is what hurts: `Number('a bit left')` is `NaN`, `blob.x + NaN` is `NaN`,
and the blob leaves the canvas for good while the tool reports "moved 5 blobs".

Apply two constraints in each handler:

- **Read and validate every argument before writing.** A refused call leaves the
  scene unchanged.
- **Enforce the scene's bounds in the handler.** Clamp moves at the canvas edge
  and record the adjustment in the action log.

The registry is bound to the session **before** the stream is attached, so an
inbound call cannot arrive before there is something to serve it:

```ts
const session = need(Session.create({actionRegistry: registry}));
const stream = need(WebSocketWireStream.connect(serverUrl));
need(await session.addStream(stream, StreamMode.START));
```

## 4. Discover page actions

The backend discovers actions through the page's `__list_actions__` response.
When a turn needs tools, it creates a reverse-dispatch proxy for each returned
schema. Those proxies belong to this connection's registry because the actions
operate on state in one page.

The page supplies its existing registry:

```ts
const connection = await connect(serverUrl, pageRegistry(scene, log));
```

The response is an `a11.actions/v1` document containing one JSON
`ActionSchema` per entry. Each port retains its JSON Schema, allowing the
backend to derive the model-facing tool definition without a second contract.

## 5. The turn

From there it is one ordinary `interact_with_llm` call. The allowed-actions header
is the request: a tool not named there is not offered to the model and cannot be
called.

```ts
need(call.setHeader(LlmHeaders.ALLOWED_LLM_ACTIONS, 'describe_scene,set_color,shift_position'));
```

The backend resolves the model's tool call against the connection registry. Its
proxy dispatches the action back through the same WebSocket, where the page runs
the handler and streams the outputs to the model.

## 6. Choose what the model can observe

`describe_scene` returns one `{id, x, y, radius, color}` object per blob, giving
the model the state needed by the mutation tools. Image inputs belong in model
message content, while tool results are JSON values. An application that needs
visual reasoning can send a screenshot as message content and keep tool results
for structured state and operation outcomes.

Use browser-hosted actions when the capability or authoritative state belongs
in the page, such as an editor selection, canvas, or local document. For tools
that belong on the backend, register them directly with
[`interact_with_llm`](../llm-sdk/interact-actions.md).
