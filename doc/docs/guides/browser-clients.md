# Call an A11 service from a browser

This guide connects a TypeScript page to a Python `echo` service over HTTP/2
Server-Sent Events (SSE). Both sides describe the same action and its input and
output streams. The page can therefore construct, call, and observe the action
without a browser-specific request format.

!!! note "Before you start"

    Install the Python package and
    `npm install a11@npm:@curiositystack/a11`. Browsers
    negotiate HTTP/2 through TLS. Browsers do not support clear-text HTTP/2
    prior knowledge (`h2c`), so local browser testing also requires a trusted
    development certificate.

## Try it

Say something and watch what crosses the wire. The wire
inspector records both action messages and node fragments; select a row to see
its action names, node IDs, and encoded byte size. **Half-close** says that the
client will send no more data while allowing already-sent work to drain.
**Reconnect** creates a fresh transport and session.

<link rel="stylesheet" href="../assets/browser-clients.css">
<div id="echo-demo" class="echo-demo">
  <div class="echo-toolbar">
    <input id="echo-server" aria-label="Echo server URL" value="https://a11.services:9443/demos/echo">
    <button id="echo-half-close" type="button">Half-close</button>
    <button id="echo-reconnect" type="button">Reconnect</button>
  </div>
  <div id="echo-errors" class="echo-errors" role="alert" aria-live="polite"></div>
  <div class="echo-workspace">
    <section class="echo-chat" aria-label="Echo chat">
      <div id="echo-messages" class="echo-messages"></div>
      <form id="echo-form" class="echo-compose">
        <input id="echo-input" aria-label="Message" autocomplete="off" placeholder="Say something...">
        <button type="submit">Send</button>
      </form>
    </section>
    <aside class="echo-side" aria-label="Wire inspector">
      <div id="echo-wire-log" class="echo-wire-log"></div>
      <div id="echo-wire-details" class="echo-wire-details">Select a wire message to inspect it.</div>
    </aside>
  </div>
</div>
<script type="module" src="../assets/browser-clients.js"></script>

## 1. Define the action contract

Both peers must agree on this schema. The Python service declares:

```python
ECHO_SCHEMA = a11.ActionSchema(
    name="echo",
    description="Return the supplied text unchanged.",
    inputs={"input": a11.ActionPortSchema(
        name="input", type="text/plain", typeinfo=str, required=True
    )},
    outputs={"output": a11.ActionPortSchema(
        name="output", type="text/plain", typeinfo=str, required=True
    )},
)
```

The browser creates the equivalent `ActionSchema` and `ActionPortSchema`,
describing the same runnable action and streaming ports on each side.

```ts
const echoSchema = new ActionSchema({
    name: 'echo',
    inputs: {
        input: new ActionPortSchema({
            name: 'input', type: 'text/plain', required: true,
        }),
    },
    outputs: {
        output: new ActionPortSchema({
            name: 'output', type: 'text/plain', required: true,
        }),
    },
});
```

## 2. Implement the server-only handler

Only the server registers a handler. Inputs and outputs are `AsyncNode`s, so
the handler consumes the final input value and puts the same value into the
output node:

```python
async def echo(action: a11.Action) -> None:
    value = await action["input"].consume(str)
    await action["output"].put(value, final=True)
```

The browser registers the schema without a handler. Calling it therefore
creates an action message for the remote session instead of executing code in
the page.

## 3. Prepare and run the Python service

Each SSE connection becomes an accepting `Session` with `echo` registered as a
handler. The endpoint pair shares the `/demos/echo` prefix:

```python
registry = a11.ActionRegistry()
registry.register("echo", ECHO_SCHEMA, echo)


async def accept(stream):
    session = a11.Session(action_registry=registry)
    await session.add_stream(stream, mode="accept")
    await session.done.wait()


options = a11.HttpSseOptions()
options.connect_endpoint = "/demos/echo/connect"
options.message_endpoint = "/demos/echo/streams/{id}/message"
server = a11.HttpSseServer.create("127.0.0.1", 80, accept, options)
```

The complete deployable module is
[`a11/demos/echo_server.py`](https://github.com/hpnkv/a11/blob/main/a11/demos/echo_server.py).
Create and trust a localhost certificate with
[mkcert](https://github.com/FiloSottile/mkcert), then run the service with TLS:

```sh
mkcert localhost 127.0.0.1 ::1
python -m a11.demos.echo_server \
  --host 127.0.0.1 --port 9000 \
  --certificate ./localhost+2.pem \
  --private-key ./localhost+2-key.pem
```

## 4. Create a browser session and connect

Create the client registry and session, then attach an SSE stream in `START`
mode. The server attaches the other end in `ACCEPT` mode. Because the service
URL includes a path, pass its endpoint paths explicitly:

```ts
const registry = new ActionRegistry();
need(registry.register('echo', echoSchema));
const session = need(Session.create({actionRegistry: registry}));
const stream = need(HttpSseClientWireStream.create(server.origin, {
    connectEndpoint: '/demos/echo/connect',
    messageEndpoint: '/demos/echo/streams/{id}/message',
}));
need(await session.addStream(stream, StreamMode.START));
```

## 5. Wire the interface through `AsyncNode`s

Make the action against the session's shared node map and stream. `call()`
sends the action description; writing the input and marking it final sends its
node fragments. The response arrives through the output `AsyncNode`:

```ts
const action = need(registry.makeAction('echo', {
    nodeMap: session.getNodeMap(), stream, session,
}));
need(await action.call());
const input = need(await action.getInput('input'));
need(await input.finalize(text));
need(await action.waitForDispatch(10_000));
const output = need(await action.getOutput('output', false));
const reply = need(await output.next({timeoutMs: 10_000}));
need(await action.wait(30_000));
```

The interface field remains an A11 node, while the action and session retain
their identities and lifecycle on both peers. The page can therefore use the
same streaming and completion operations as the service.

## 6. Display failures

A11 APIs return a `StatusOr<T>`: success values pass `isOk`, while failures
carry a status code, message, and structured details. Convert failures at the
UI boundary and render them in a live error region:

```ts
const need = <T>(value: T | Status): T => {
    if (!isOk(value)) {
        throw new Error(`${StatusCode[value.code]}: ${value.message}`);
    }
    return value as T;
};

try {
    await sendEcho(text);
} catch (error) {
    errorRegion.textContent = error instanceof Error
        ? error.message
        : String(error);
}
```
