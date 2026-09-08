# Let a model use tools in the browser

Browser-hosted actions let a model operate on state that exists only in the
page, such as a canvas, selection, or editor.

Serve page actions through the existing A11 session and register their schemas
with the model backend. The handlers retain page state while the model sees
ordinary A11 actions.

!!! note "Before you start"

    The demo talks to `wss://a11.to/ws/demoserver`, which runs an Ollama
    beside itself, so the default needs no key. Claude, Gemini, and OpenAI use
    their provider credentials. To run the backend yourself:

    ```sh
    python -m a11.demos.web_demos_server   # ws://127.0.0.1:9010/a11-demos
    ```

    A page loaded over HTTPS may refuse a plaintext `ws://` socket even to
    localhost (Chrome allows it, Firefox does not), so give a local backend
    the `--certificate` / `--private-key` flags and a trusted certificate —
    [mkcert](https://github.com/FiloSottile/mkcert) makes one — if the
    browser blocks it.

    Use a model with reliable tool-schema support. The example has been tested
    with `glm-5.3-flash:cloud` on Ollama, Claude, and Gemini.

## Try it

Try "make blob 2 red and move it up", "spread them out", or "give them a
warm palette". Drag the scene to orbit it. The right pane logs each tool call
and its activity message. The model response appears above the log.

<link rel="stylesheet" href="../assets/web-demos.css">
<div id="tools-demo" class="a11-demo">
  <div class="a11-toolbar">
    <input id="tools-server" class="wide" aria-label="Demo server URL"
           value="wss://a11.to/ws/demoserver">
    <span class="a11-field">
      <label for="tools-provider">Provider</label>
      <select id="tools-provider">
        <option value="ollama">Ollama</option>
        <option value="claude">Claude</option>
        <option value="gemini">Gemini</option>
        <optgroup label="OpenAI API family">
          <option value="openai">OpenAI</option>
          <option value="vllm">vLLM</option>
          <option value="openai-compatible">OpenAI-compatible endpoint</option>
        </optgroup>
      </select>
    </span>
    <span class="a11-field">
      <label for="tools-model">Model</label>
      <input id="tools-model" value="glm-5.3-flash:cloud">
    </span>
    <span class="a11-field">
      <label for="tools-api-key">API key</label>
      <input id="tools-api-key" value="use-a11-demo-resources">
    </span>
    <span class="a11-field">
      <label for="tools-base-url">Base URL</label>
      <input id="tools-base-url" value="https://ollama.com">
    </span>
  </div>
  <div id="tools-errors" class="a11-errors" role="alert" aria-live="polite"></div>
  <div class="a11-panes tools">
    <section class="a11-pane" aria-label="The scene this page serves">
      <header>the page</header>
      <div class="a11-canvas-wrap">
        <canvas id="tools-canvas" width="620" height="300" aria-label="Five coloured blobs in a 3D scene; drag to orbit"></canvas>
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
[`js/demo/browser_tools.ts`](https://github.com/hpnkv/a11/blob/main/js/demo/browser_tools.ts). The IntelliJ plugin's
webview does the same thing with the IDE's editor and index instead of a scene — see
`intellij-plugin/webview/src/ideTools.ts`.

## 1. Define model arguments as ports

An A11 action's tool definition is derived from its *ports*
([`ToolAdapter`](../llm-sdk/action-tools.md)): one port per argument, with a
streaming port represented as an array. Define the action at that boundary:

```ts
const SET_COLOR_SCHEMA = new ActionSchema({
    name: 'set_color',
    description: 'Recolour blobs: the i-th id is given the i-th colour.',
    inputs: {
        ids: new ActionPortSchema({
            name: 'ids', type: 'application/json', required: true,
            description: 'Which blobs to recolour.'
        }),
        colors: new ActionPortSchema({
            name: 'colors', type: 'text/plain', required: true,
            description: 'One `#rrggbb` per id, in the same order.'
        }),
    },
    outputs: {
        recoloured: new ActionPortSchema({
            name: 'recoloured', type: 'application/json',
            unary: true, required: true
        }),
    },
});
```

The schema exposes each input with its field description; it does not wrap them in an opaque `request` object.

!!! warning "A TypeScript port has a MIME type, not a value type"

    `ToolAdapter` reads the MIME type, so an `application/json` port is described
    to the model as a bare `{"type": "object"}` and it will dutifully send
    `ids: [{...}, {...}]`. Pass the shape explicitly:

    ```ts
    const PORT_SCHEMAS = {
        set_color: {ids: z.number().int(), colors: z.string()},
        shift_position: {
            ids: z.number().int(), dx: z.number(), dy: z.number(), dz: z.number(),
        },
    };
    ```

    Those go to `getToolDefinitions(registry, names, PORT_SCHEMAS)`. Python needs
    no equivalent: an `ActionPortSchema` there carries `typeinfo`.

## 2. Keep progress logs separate from results

A tool can report user-visible activity through `action.log()`. Because no port declares this log, it does not become
part of the model's tool result. The backend's [tool runner](../llm-sdk/tool-runner.md) reads it separately, associates
it with the call ID, and records it in the turn metadata for later replay.

```ts
need(await action.log(`Recoloured ${recoloured} blob(s).`));
```

The log channel requires no declared port or result cleanup.

## 3. Serve actions from the page

A page handler reads the declared inputs, modifies page state, writes the
declared outputs, and closes them.

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

Validate every model-supplied argument before modifying page state. Return a status when the request cannot be applied:

```ts
const value = finiteNumber(raw[axis], axis, 2 * span[axis]);
if (isStatus(value)) return await refuse(action, value, onLog);
```

Return a specific status such as
`invalidArgumentError('dx must be a number of pixels; got "a bit left".')`.
The tool runner supplies it to the model as the call result, allowing a second
call with a numeric value. Validate before arithmetic because
`Number('a bit left')` produces `NaN`.

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

## 5. Run the turn

Use an ordinary `interact_with_llm` call. The allowed-actions header selects
the page tools exposed to the model for this request.

```ts
need(call.setHeader(LlmHeaders.ALLOWED_LLM_ACTIONS, 'describe_scene,set_color,shift_position'));
```

The backend resolves the model's tool call against the connection registry. Its
proxy dispatches the action back through the same WebSocket, where the page runs
the handler and streams the outputs to the model.

## 6. Choose what the model can observe

`describe_scene` returns one `{id, x, y, z, radius, color}` object per blob in
world units, giving mutation tools the scene's authoritative coordinates.
Screen coordinates would depend on the reader's current projection. Image
inputs belong in model message content, while tool results are JSON values. An
application that needs visual reasoning can send a rendered frame as message
content and retain tool results for structured state and operation outcomes.

Use browser-hosted actions when the capability or authoritative state belongs
in the page, such as an editor selection, scene, or local document. For tools
that belong on the backend, register them directly with
[`interact_with_llm`](../llm-sdk/interact-actions.md).
