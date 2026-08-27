# A chat that survives a reload

This guide builds a persistent chat conversation. One action supports multiple
providers, the page retains the model's structured interaction objects, and a
reload continues the same conversation.

The session design is provider-agnostic: Ollama, Claude, and Gemini use
the same interface, configured via headers.

!!! note "Before you start"

    The demo on this page talks to the hosted backend at
    `wss://a11.services:9443/a11-demos`, which runs an Ollama beside itself — so the
    default (Ollama, `glm-4.7-flash`, base URL `http://127.0.0.1:11434`) answers
    with no key at all. Note whose localhost that is: the base URL is resolved by
    whoever *serves* the action. Claude and Gemini need a key in the key field.

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

Ask something, then reload the page: the conversation is still there, and the
next answer is given in its context. **New** starts a fresh one without dropping
the socket; the dropdown reopens any stored conversation. The right-hand pane is
the `thoughts` port — a model that thinks before it speaks shows its working
there, on a port of its own, while `text_output` streams the answer.

<link rel="stylesheet" href="../assets/web-demos.css">
<div id="chat-demo" class="a11-demo">
  <div class="a11-toolbar">
    <input id="chat-server" class="wide" aria-label="Demo server URL" value="wss://a11.services:9443/a11-demos">
    <select id="chat-provider" aria-label="Provider">
      <option value="ollama">Ollama</option>
      <option value="claude">Claude</option>
      <option value="gemini">Gemini</option>
    </select>
    <input id="chat-model" aria-label="Model" value="glm-4.7-flash">
    <input id="chat-api-key" type="password" aria-label="API key" placeholder="API key (Claude or Gemini)">
    <input id="chat-base-url" aria-label="Base URL" value="http://127.0.0.1:11434">
    <select id="chat-conversations" aria-label="Stored conversations"></select>
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
An `a11 gateway run` serves the same three actions, so a page can point at one of
those instead.

## 1. One action, every provider

[`interact_with_llm`](../llm-sdk/interactions.md) uses one action schema for all
supported model providers. The provider, model, key, and base URL are headers,
so one registration serves them and the caller selects the backend:

```python
from a11.sdk.llm import LlmHeaders

LlmHeaders.PROVIDER  # x-a11-llm-provider   claude | gemini | ollama
LlmHeaders.MODEL  # x-a11-llm-model
LlmHeaders.API_KEY  # x-a11-llm-api-key
LlmHeaders.BASE_URL  # x-a11-llm-base-url
```

In the browser they are the same names, set on the call before it is dispatched:

```ts
const call = need(Action.create(INTERACT_WITH_LLM_SCHEMA, {session, stream, nodeMap: session.getNodeMap()}));
need(call.setHeader(LlmHeaders.PROVIDER, 'ollama'));
need(call.setHeader(LlmHeaders.MODEL, 'glm-4.7-flash'));
need(call.setHeader(LlmHeaders.BASE_URL, 'http://127.0.0.1:11434'));
need(await call.call());
```

Select a hosted model by setting the provider, model, and API key. The ports,
reading code, and conversation format remain unchanged.

The action's ports are the same whoever answers: `interactions`, `tools` and
`config` in; `text_output`, `thoughts`, `event_stream` and `new_interactions`
out. A page reads the visible answer off `text_output` and never has to parse a
provider's event stream.

## 2. The conversation is a list of interactions

A turn's history is not a transcript the page rebuilt from text. It is the list
of `a11.sdk.llm.Interaction` objects the provider itself produced — including the tool calls it made and their results —
and the next turn
puts the whole list back in front of the model:

```ts
const interactions = need(await call.getInput('interactions'));
for (const interaction of history) need(await interactions.put(interaction));
need(await interactions.finalize(question));
```

The page uses the first interaction's ID as the **conversation ID**. The backend
therefore does not need to return a separate session handle.

## 3. The backend records what it answers

On the server the action is wrapped in one that stores the turn as it goes,
`a11.gateway.conversation_actions.interact_with_llm_and_persist`, and two more
actions read the recording back:

```python
from a11.gateway import conversation_actions, conversations

store = conversations.ConversationStore("/var/lib/a11/conversations")
conversation_actions.install(registry, store)
# registers: interact_with_llm (recording), get_conversation, get_conversations
```

The store is [SQLite][a11.stores.sqlite_chunk_store.SQLiteChunkStore]: one
`AsyncNode` per conversation, backed by a SQLite chunk store, plus a small table
that indexes them for the list. It needs no server, survives a restart, and
`await store.record(interactions)` is idempotent — the page replays its whole
history every turn, and only what is new is appended, by interaction id.

!!! tip "Not every model call is a conversation"

    The demo server also registers the same action, unrecorded, as `ask_model`.
    A step inside a composition is not a chat turn: recorded, each of the
    [deep-research](deep-research.md) flow's model calls would arrive in this
    guide's conversation list as a conversation of its own.

## 4. Reloading is two calls

`get_conversations` fills the picker, and `get_conversation` streams one
conversation's interactions back. The page declares both schemas by hand — they
are the backend's, mirrored:

```ts
const GET_CONVERSATION_SCHEMA = new ActionSchema({
    name: 'get_conversation',
    inputs: {id: new ActionPortSchema({name: 'id', type: 'text/plain', unary: true, required: true})},
    outputs: {interactions: new ActionPortSchema({name: 'interactions', type: 'application/json', required: true})},
});
```

What comes back is re-parsed on the way in, and that matters more than it looks:

```ts
const next = need(await node.next({timeoutMs: 30_000, expectedTag: INTERACTION_TAG}));
restored.push(need(parseInteraction(next)));
```

`parseInteraction` brands the value with its serialization tag, which is
what lets it go back out to the backend as an `a11.sdk.Interaction` on the next
turn with the `a11.sdk.Interaction` type expected by the `interactions` port.
The same tag table is what makes this work across languages at all — see
`js/src/serial_tags.ts` and `a11/data/serial_tags.py`.

Because the restored interactions *become* the history, the next turn continues
the same conversation and lands on the same conversation node on the backend: its
id is the first interaction's id, replayed unchanged.

## 5. Keep the id in the URL

The last piece of "survives a reload" is not A11 at all:

```ts
const url = new URL(window.location.href);
url.searchParams.set('conversation', this.conversationId!);
window.history.replaceState(null, '', url);
```

On load, the page lists the conversations and reopens whatever the URL names.
