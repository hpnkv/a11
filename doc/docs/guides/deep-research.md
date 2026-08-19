# Deep research, as a composition

This guide builds a "deep research" agent — plan a topic, investigate its parts
at the same time, write one report — and then shows that the whole of it is a
text file. There is no orchestration code: the backend serves
[`interact_with_llm`](../llm-sdk/interactions.md) and one text-splitting action,
and the composition that puts them together is
[Flow](../api/flow.md).

The page dispatches one action, `deep-research`, and reads three of its ports.

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

    A run costs one model call to plan, one per brief, and one to synthesise —
    five or six on the defaults, which is why the keyless local model is the one
    to watch it with.

## Try it

Give it a topic. The middle pane is the plan as the planner produces it, the right
pane is `user_log`, and the report streams into the left pane as it is written.
Watch the `[investigate]` lines: they overlap, because three investigations are in
flight, and their intermediate reports never cross the socket.

<link rel="stylesheet" href="../assets/web-demos.css">
<div id="research-demo" class="a11-demo">
  <div class="a11-toolbar">
    <input id="research-server" class="wide" aria-label="Demo server URL" value="wss://a11.services:9443/a11-demos">
    <select id="research-provider" aria-label="Provider">
      <option value="ollama">Ollama</option>
      <option value="claude">Claude</option>
      <option value="gemini">Gemini</option>
    </select>
    <input id="research-model" aria-label="Model" value="glm-4.7-flash">
    <input id="research-api-key" type="password" aria-label="API key" placeholder="API key (Claude or Gemini)">
    <input id="research-base-url" aria-label="Base URL" value="http://127.0.0.1:11434">
  </div>
  <div id="research-errors" class="a11-errors" role="alert" aria-live="polite"></div>
  <form id="research-form" class="a11-compose">
    <input id="research-topic" aria-label="Topic" autocomplete="off" placeholder="A topic to research...">
    <button type="submit">Research</button>
  </form>
  <div class="a11-panes">
    <section class="a11-pane" aria-label="Report">
      <header>report port</header>
      <div id="research-report" class="a11-prose"></div>
    </section>
    <aside class="a11-pane" aria-label="Plan and activity">
      <header>plan port</header>
      <ol id="research-plan" class="a11-plan"></ol>
      <header>user_log port</header>
      <div id="research-log" class="a11-log"></div>
    </aside>
  </div>
</div>
<script type="module" src="../assets/deep-research.js"></script>

The page is
[`js/demo/deep_research.ts`](https://github.com/hpnkv/a11/blob/main/js/demo/deep_research.ts);
[
`examples/004-deep-research/deep-research.flow`](https://github.com/hpnkv/a11/blob/main/examples/004-deep-research/deep-research.flow)
takes the same subject further into the language — typed sources, `zip`, a flow
calling a flow.

## 1. What the composition has to say

The shape is the one the predecessor of this example wrote in about 400 lines of
Python: a planner, N investigations that do not depend on each other, and a
synthesis at the end. Written as a flow it is four declarations:

```a11flow
flow deep-research {
  in  topic:    string required
  out report:   string stream
  out plan:     string stream
  out user_log: string stream

  planned = run plan-research(topic: topic)
  planned.user_log -> user_log
  planned.briefs -> plan

  nodes research {
    findings = node()

    for brief in planned.briefs parallel 3 {
      one = run investigate(topic: topic, brief: brief)
      one.report -> findings
      one.user_log -> user_log
    }

    written = run synthesise-findings(
      topic: topic, brief: planned.synthesis, findings: findings,
    )
    written.report -> report
    written.user_log -> user_log
  }
}
```

Four things in that are worth reading twice.

**`for ... parallel 3`** is the fan-out. The planner's `briefs` port is still open
when the loop starts reading it, so an investigation begins as soon as its brief
exists rather than after the plan is complete.

**`nodes research { ... }`** gives everything inside it a
[node map][a11.nodes.async_node.NodeMap] of its own. The investigations' reports
are written, read and dropped on the backend; the page that dispatched the flow
is not sent three intermediate reports to get one report back.

**`findings = node()`** is where the investigations meet. A unary output port
cannot be written by three loop passes; a node can, and one reader reads it back.

**`user_log`** is narration. Every step writes to it, arrival order is the order
things happened, and it is what the page shows while it waits.

## 2. A model call, in a language with no model in it

`interact_with_llm` wants an `a11.sdk.llm.Interaction`, and a flow can make one: `TYPE{...}` names a type the host's
serialization registry knows, and
`to_chunk` makes the content it is built from.

```a11flow
llm = run ask_model(
  interactions: brief | map a11.sdk.Interaction{
    role: "user",
    system_instructions: [to_chunk("You are one of several research agents...")],
    content: [to_chunk({
      role: "user",
      content: [{type: "text", text: join([
        strformat("The research topic is: %s", topic),
        strformat("Your brief is: %s", it)
      ], "\n\n")}]
    })]
  },
  config: {}
)
```

Note what the flow does *not* say: which provider, which model, which key. A
nested action is given its parent's `x-a11-` headers, so the page names the
backend once, on the `deep-research` call, and every model call inside inherits
it.

`ask_model` is `interact_with_llm` without the conversation recording (the demo
server registers both). A step of a composition is not a chat turn — recorded,
every investigation would show up in the
[chat guide](chat-sessions.md)'s conversation list as a conversation of its own.

## 3. The one thing the language will not do

The planner answers with one brief per line, and the fan-out needs those lines as
a *stream*. Flow has `split`, but a list is one value: `for` iterates a stream,
so a list-valued expression is a single pass. There is deliberately no stage that
explodes a list — the language composes actions rather than growing a string
library — so the missing primitive is supplied the way everything else is, as an
action:

```python
--8<-- "a11/demos/split_lines.py:43:53"
```

and the flow calls it:

```a11flow
lines = run split_lines(text: llm.text_output | join "")

lines.lines | where not starts-with(it, "FINALLY:") -> briefs
lines.lines | where starts-with(it, "FINALLY:") | first 1 -> synthesis
```

`| join ""` puts the streamed tokens back into one text; `split_lines` makes them
values again; `where` sorts the plan's briefs from the instruction about writing
the report.

## 4. Serving it

A flow is an action. Compiling the file and registering it is two lines, and from
then on nothing can tell the composition from a handler:

```python
from a11 import flow

program = flow.load("a11/demos/deep_research.flow")
program.register_all(registry)  # deep-research, plan-research, investigate, ...
```

## 5. Calling it from the browser

The page declares the flow's ports by hand — a flow's contract is its ports, and
this side neither knows nor cares that the action is a composition:

```ts
const DEEP_RESEARCH_SCHEMA = new ActionSchema({
    name: 'deep-research',
    inputs: {topic: new ActionPortSchema({name: 'topic', type: 'text/plain', unary: true, required: true})},
    outputs: {
        report: new ActionPortSchema({name: 'report', type: 'text/plain'}),
        plan: new ActionPortSchema({name: 'plan', type: 'text/plain'}),
        user_log: new ActionPortSchema({name: 'user_log', type: 'text/plain'}),
    },
});
```

All three outputs are read at once, because they fill at once:

```ts
await Promise.all([
    readPort(call, 'user_log', (value) => addLine(log, String(value))),
    readPort(call, 'plan', (value) => addPlanItem(String(value))),
    readPort(call, 'report', (value) => appendReport(String(value))),
]);
need(await call.wait(600_000));
```

An output nobody drains stalls the step producing it, so reading them in sequence
would not merely be slower — it would hold the composition up.

## The whole composition

```a11flow
--8<-- "a11/demos/deep_research.flow"
```

