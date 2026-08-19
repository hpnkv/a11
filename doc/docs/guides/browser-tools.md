# The model calls back into the page

The interesting half of an agent in a browser is not the page calling the model.
It is the model calling the page: the state worth acting on — the canvas, the
selection, the editor — is in the browser, and only the browser can touch it.

This guide serves three actions from a web page, tells the backend they exist, and
lets a model use them. The page's handlers run in the page; the backend never
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

    A model that follows tool schemas well makes this much less frustrating.
    `glm-4.7-flash` on Ollama, Claude and Gemini all work.

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

— rather than as one opaque `request` blob the model has to guess the shape of.

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

## 2. Narration is not part of the result

A tool's account of its own run goes to `action.log()`, for the person watching.
No port declares it, so it cannot become part of the tool result the model is
shown; the backend's [tool runner](../llm-sdk/tool-runner.md) reads it separately,
files it under the call id, and records it in the turn's metadata — so a
conversation replayed later still shows what a tool *did* rather than only that it
ran.

```ts
need(await action.log(`Recoloured ${recoloured} blob(s).`));
```

Nothing to close, and nothing to remember to strip.

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
    need(await result.putFinal(blobs.length));
    need(await result.drainAndClose());
    return okStatus();
}));
```

### Validate, then act

A tool call is the least trustworthy input a page gets: every value in it is a
model's idea of what the schema said. So each argument is checked **before
anything is touched**, and a call that cannot be honoured comes back as a status:

```ts
const dx = rawDx === null ? 0 : finiteNumber(rawDx, 'dx', scene.width);
if (isStatus(dx)) return await refuse(action, dx, onLog);
```

Returning `invalidArgumentError('dx must be a number of pixels; got "a bit left".')`
is not a dead end — the tool runner hands it to the model as *this call's result*,
so the model sees what was wrong and can call again with a number. Coercing
instead is what hurts: `Number('a bit left')` is `NaN`, `blob.x + NaN` is `NaN`,
and the blob leaves the canvas for good while the tool reports "moved 5 blobs".

Two habits fall out of that, and both are the tool's job rather than the model's:

- **Nothing is written until every argument has been read.** A refused call leaves
  the scene exactly as it was; a half-applied one is harder to undo than to
  prevent.
- **The scene's own limits are enforced by the scene.** A move that would take a
  blob off the canvas stops at the edge, and the run log says so, because "off the
  left edge" is not a place the page can show or the model can name again.

The registry is bound to the session **before** the stream is attached, so an
inbound call cannot arrive before there is something to serve it:

```ts
const session = need(Session.create({actionRegistry: registry}));
const stream = need(WebSocketWireStream.connect(serverUrl));
need(await session.addStream(stream, StreamMode.START));
```

## 4. The handshake

The backend cannot dispatch to an action it has never heard of. Once per
connection the page announces its schemas on the reserved `__register_tools__`
action, and the backend's
`a11.gateway.tool_bridge.RemoteToolBridge` registers a proxy
per descriptor — on *this connection's own* registry copy, because these actions
belong to one page:

```ts
const announce = makeCall(connection, REGISTER_TOOLS_SCHEMA);
need(await announce.call());
const tools = need(await announce.getInput('tools'));
for (const schema of schemas) need(await tools.put(describeTool(schema)));
need(await tools.putNullFinal());
```

`describeTool` sends the *port* description, and flags the log port:

```ts
outputs: [...schema.outputs.values()].map((port) =>
    describePort(port)),
```

!!! danger "Two documents, one word"

    The descriptor above is not the JSON-Schema tool definition the model is
    shown. They go on different ports of different actions, and sending the
    definition where the descriptor belongs yields a proxy with no inputs at all
    — the model's arguments then have nowhere to land and every call fails with
    "unexpected input".

## 5. The turn

From there it is one ordinary `interact_with_llm` call. The allowed-actions header
is the request: a tool not named there is not offered to the model and cannot be
called.

```ts
need(call.setHeader(LlmHeaders.ALLOWED_LLM_ACTIONS, 'describe_scene,set_color,shift_position'));
```

What happens next is the point of the whole guide. The model's tool call reaches
the backend's runner, the runner resolves it against this connection's registry,
finds the proxy, and the proxy dispatches the call **back down the same
WebSocket** to the page — which runs the real handler and streams the outputs
back. The model sees one A11 action; the work happens where the canvas is.

## 6. What was left out, and why

The predecessor of this example took a WebGL screenshot and fed it to the model
mid-turn. A11's tool contract has no place for that today: a tool result is JSON,
and images ride as *message content*, not as tool results. So instead of
photographing the scene the model reads it — `describe_scene` returns one
`{id, x, y, radius, color}` per blob — which serves the same purpose (find out
what is there before changing it) within the contract that exists.

