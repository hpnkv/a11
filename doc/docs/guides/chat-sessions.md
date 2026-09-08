# A chat that survives a reload

Build a persistent chat with one action for multiple providers. The page stores
the model's structured interaction objects and resumes the conversation after a
reload.

The session design is provider-agnostic: Ollama, Claude, Gemini, OpenAI, vLLM,
and OpenAI-compatible endpoints use the same interface, configured via headers.

!!! note "Before you start"

    The demo on this page talks to the hosted backend at
    `wss://a11.to/ws/demoserver`, which runs an Ollama beside itself — so the
    default (Ollama, `glm-4.7-flash`, base URL `http://127.0.0.1:11434`) answers
    without a key. The process serving the action resolves the base URL, so
    `127.0.0.1` refers to that backend host. Claude, Gemini, and OpenAI use
    their provider credentials.

    To run the backend yourself instead:

    ```sh
    pip install a11-kit
    python -m a11.demos.web_demos_server   # ws://127.0.0.1:9010/a11-demos
    ```

    A page loaded over HTTPS may refuse a plaintext `ws://` socket even to
    localhost (Chrome allows it, Firefox does not), so give a local backend
    the `--certificate` / `--private-key` flags and a trusted certificate —
    [mkcert](https://github.com/FiloSottile/mkcert) makes one — if the
    browser blocks it.

    Either address goes in the demo's first field. `https://` and `wss://` name
    the same endpoint here. Browsers connect directly because A11's WebSocket
    server supports HTTP/1.1 and HTTP/2. Install the TypeScript package with
    `npm install a11@npm:@curiositystack/a11`.

## Try it

Send a turn, reload the page, and continue with the restored context. **New**
starts another conversation on the existing socket. The right pane renders the
`thoughts` port while `text_output` streams the answer.

<link rel="stylesheet" href="../assets/web-demos.css">
<div id="chat-demo" class="a11-demo">
  <div class="a11-toolbar">
    <input id="chat-server" class="wide" aria-label="Demo server URL"
           value="wss://a11.to/ws/demoserver">
    <span class="a11-field">
      <label for="chat-provider">Provider</label>
      <select id="chat-provider">
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
      <label for="chat-model">Model</label>
      <input id="chat-model" value="glm-5.3-flash:cloud">
    </span>
    <span class="a11-field">
      <label for="chat-api-key">API key</label>
      <input id="chat-api-key" value="use-a11-demo-resources">
    </span>
    <span class="a11-field">
      <label for="chat-base-url">Base URL</label>
      <input id="chat-base-url" value="https://ollama.com">
    </span>
    <button id="chat-new" type="button">New</button>
  </div>
  <div id="chat-errors" class="a11-errors" role="alert" aria-live="polite"></div>
  <div class="a11-panes">
    <section class="a11-pane" aria-label="Chat">
      <header>Chat</header>
      <div id="chat-messages" class="a11-messages"></div>
      <form id="chat-form" class="a11-compose">
        <input id="chat-input" aria-label="Message" autocomplete="off" placeholder="Ask something...">
        <button type="submit">Send</button>
      </form>
    </section>
    <aside class="a11-pane" aria-label="Thoughts">
      <header>thoughts port</header>
      <div id="chat-thoughts" class="a11-messages"></div>
    </aside>
  </div>
</div>
<script type="module" src="../assets/chat-sessions.js"></script>

The page is
[`js/demo/chat_sessions.ts`](https://github.com/hpnkv/a11/blob/main/js/demo/chat_sessions.ts)
over
[`js/demo/demo_support.ts`](https://github.com/hpnkv/a11/blob/main/js/demo/demo_support.ts),
and the backend is
[`a11/demos/web_demos_server.py`](https://github.com/hpnkv/a11/blob/main/a11/demos/web_demos_server.py).
`a11 gateway run` serves the same three actions as another compatible endpoint.

## 1. One action, every provider

[`interact_with_llm`](../llm-sdk/interactions.md) uses one action schema for all
supported model providers. The provider, model, key, and base URL are headers,
so one registration serves them and the caller selects the backend:

```python
from a11.sdk.llm import LlmHeaders

LlmHeaders.PROVIDER  # claude | gemini | ollama | openai | vllm
LlmHeaders.MODEL  # x-a11-llm-model
LlmHeaders.API_KEY  # x-a11-llm-api-key
LlmHeaders.BASE_URL  # x-a11-llm-base-url
```

In the browser they are the same names, set on the call before it is dispatched:

```ts
const call = need(Action.create(INTERACT_WITH_LLM_SCHEMA, {
    session,
    stream,
    nodeMap: session.getNodeMap(),
}));
need(call.setHeader(LlmHeaders.PROVIDER, 'ollama'));
need(call.setHeader(LlmHeaders.MODEL, 'glm-5.3-flash:cloud'));
need(call.setHeader(LlmHeaders.BASE_URL, 'https://ollama.com'));
need(await call.call());
```

Select a hosted model by setting the provider, model, and API key. The ports,
reading code, and conversation format remain unchanged.

The demo groups OpenAI, vLLM, and generic OpenAI-compatible endpoints because
they expose the same API shape. The generic option sends `openai` as the
provider and uses the base URL entered beside it. vLLM uses its own provider
adapter and can discover the model served by an endpoint when the model field
is empty.

Every provider uses the same ports: `interactions`, `tools`, and `config` as
inputs; `text_output`, `thoughts`, `event_stream`, and `new_interactions` as
outputs. The page reads visible text from `text_output` without parsing provider
events.

## 2. Store the interaction list

A turn's history is not a transcript rebuilt from text. It is the list of
`a11.sdk.llm.Interaction` objects the provider produced, including tool calls
and results. The next turn sends that structured history back to the model:

```ts
const interactions = need(await call.getInput('interactions'));
for (const interaction of history) need(await interactions.put(interaction));
need(await interactions.finalize(question));
```

The page uses the first interaction's ID as the **conversation ID**. The backend
therefore does not need to return a separate session handle.

## 3. The backend records what it answers

On the server the action is wrapped in one that stores the turn as it goes,
`a11.gateway.conversation_actions.interact_with_llm_and_persist`, and a second
action reads the recording back:

```python
from a11.gateway import conversation_actions, conversations

store = conversations.ConversationStore("/var/lib/a11/conversations")
conversation_actions.install(registry, store)
```

The store is [SQLite][a11.stores.sqlite_chunk_store.SQLiteChunkStore]: one
`AsyncNode` per conversation, backed by a SQLite chunk store, plus a small table
that indexes them for the list. It needs no server, survives a restart, and
`await store.record(interactions)` is idempotent — the page replays its whole
history every turn, and only what is new is appended, by interaction id.

!!! tip "Not every model call is a conversation"

    The demo server also registers the same action, unrecorded, as `ask_model`.
    A step inside a composition is not a chat turn: recorded, each of the
    [deep-research](deep-research.md) agent's model calls would arrive in this
    guide's conversation list as a conversation of its own.

## 4. Reloading is one call

`get_conversation` streams one conversation's interactions back, given its id.
The page declares the schema by hand — it is the backend's, mirrored:

```ts
const GET_CONVERSATION_SCHEMA = new ActionSchema({
    name: 'get_conversation',
    inputs: {
        id: new ActionPortSchema({
            name: 'id', type: 'text/plain', unary: true, required: true,
        }),
    },
    outputs: {
        interactions: new ActionPortSchema({
            name: 'interactions', type: 'application/json', required: true,
        }),
    },
});
```

Parse each restored value with its expected serialisation tag:

```ts
const next = need(await node.next({timeoutMs: 30_000, expectedTag: INTERACTION_TAG}));
restored.push(need(parseInteraction(next)));
```

`parseInteraction` brands the value with its serialization tag, which is
what lets it go back out to the backend as an `a11.sdk.Interaction` on the next
turn with the `a11.sdk.Interaction` type expected by the `interactions` port.
The shared tag table provides the cross-language mapping; see
`js/src/serial_tags.ts` and `a11/data/serial_tags.py`.

The restored interactions become the next turn's history. Their first ID also
selects the same backend conversation node.

## 5. Keep the id in the URL

Store the conversation ID in the page URL:

```ts
const url = new URL(window.location.href);
url.searchParams.set('conversation', this.conversationId!);
window.history.replaceState(null, '', url);
```

On load, the page reopens whatever conversation the URL names.
