# Build a parallel research agent in Python

This guide builds one `deep-research` action in ordinary asynchronous Python.
The agent asks a model to plan a topic, investigates several briefs with
bounded concurrency, and streams a synthesized report to its caller.

The implementation uses A11 for the model boundary, action nesting, output
streams, logs, and remote serving. Python retains direct control of task
creation, prompts, parsing, and application policy.

## Define the agent's public boundary

The caller supplies one topic and can read the plan and report as each is
produced. The model headers from `interact_with_llm` let a caller choose Claude,
Gemini, or Ollama once for the whole run.

```python
import asyncio

import a11
from a11.sdk.interact_with_llm import (
    INTERACT_WITH_LLM_SCHEMA,
    interact_with_llm,
)
from a11.sdk.llm import Interaction, Role


DEEP_RESEARCH_SCHEMA = a11.ActionSchema(
    name="deep-research",
    description=(
        "Plan a topic, investigate its parts concurrently, and write a report."
    ),
    inputs={
        "topic": a11.ActionPortSchema(
            "topic", "text/plain", typeinfo=str, unary=True, required=True
        )
    },
    outputs={
        "plan": a11.ActionPortSchema(
            "plan", "text/plain", typeinfo=str, required=True
        ),
        "report": a11.ActionPortSchema(
            "report", "text/plain", typeinfo=str, required=True
        ),
    },
    headers=INTERACT_WITH_LLM_SCHEMA.headers,
)
```

This schema is the product boundary. A browser or another service only needs
the action name and ports; it does not need to know how the research is
orchestrated.

## Run model calls as nested actions

The helper below starts the included provider-neutral model action beneath the
research action. `propagate_io=False` keeps its raw events, reasoning, and
completed interactions in the process. Only text copied to a parent output is
sent to the research caller.

Every output is drained concurrently. This matters for model SDKs that produce
text, reasoning, events, and completed interactions at the same time.

```python
async def _drain(node: a11.AsyncNode) -> None:
    async for _ in node:
        pass


async def _read_text(
    node: a11.AsyncNode,
    destination: a11.AsyncNode | None = None,
) -> str:
    parts: list[str] = []
    async for value in node:
        text = str(value)
        parts.append(text)
        if destination is not None:
            await destination.put(text)
    return "".join(parts)


async def ask_model(
    parent: a11.Action,
    prompt: str,
    *,
    stream_to: a11.AsyncNode | None = None,
) -> str:
    model = parent.make_nested(
        "interact_with_llm",
        propagate_io=False,
    ).run()

    text = asyncio.create_task(
        _read_text(model["text_output"], stream_to)
    )
    drains = [
        asyncio.create_task(_drain(model[name]))
        for name in ("thoughts", "event_stream", "new_interactions")
    ]

    turn = Interaction(
        role=Role.USER,
        content=[a11.to_chunk({
            "role": "user",
            "content": [{"type": "text", "text": prompt}],
        })],
    )
    await model["interactions"].finalize(turn)
    await model["config"].finalize({})
    await model["tools"].finalize()

    answer = await text
    await asyncio.gather(*drains)
    await model.wait()
    return answer
```

`make_nested` forwards the parent's A11 headers by default. The provider,
model, credentials, deadline, and tracing context set on `deep-research`
therefore reach every model call without becoming global state.

## Orchestrate planning, investigation, and synthesis

The handler is a regular coroutine. A semaphore limits model concurrency, and
`asyncio.gather` makes the barrier before synthesis explicit.

```python
async def deep_research(action: a11.Action) -> None:
    topic = await action["topic"].consume(str)
    await action.logf("planning research on: %s", topic)

    plan_text = await ask_model(action, f"""
Plan research on this topic: {topic}

Write one independent investigation brief per line. Write at most three.
End with a line beginning `FINALLY:` that tells a writer how to synthesize
the findings. Do not number the lines.
""")
    lines = [line.strip() for line in plan_text.splitlines() if line.strip()]
    if len(lines) < 2 or not lines[-1].startswith("FINALLY:"):
        raise ValueError("the planner did not return briefs and a final task")

    briefs = lines[:-1]
    synthesis_brief = lines[-1].removeprefix("FINALLY:").strip()
    for brief in briefs:
        await action["plan"].put(brief)
    await action["plan"].finalize()

    limit = asyncio.Semaphore(3)

    async def investigate(brief: str) -> str:
        async with limit:
            await action.logf("investigating: %s", brief)
            return await ask_model(action, f"""
Research this brief in the context of `{topic}`:

{brief}

Return concise findings and name the sources you relied on.
""")

    findings = await asyncio.gather(
        *(investigate(brief) for brief in briefs)
    )

    await action.logf("synthesizing %d investigations", len(findings))
    joined_findings = "\n\n---\n\n".join(findings)
    await ask_model(
        action,
        f"""
Write the final report about `{topic}`.

Instruction: {synthesis_brief}

Investigation findings:
{joined_findings}
""",
        stream_to=action["report"],
    )
    await action["report"].finalize()
```

The planner completes before investigations start. Investigations overlap up
to the explicit limit, and synthesis waits for their returned strings. The
final model call writes tokens directly to the public `report` node, so the
caller can render the report before the model finishes it.

If any child action or Python task fails, the parent action finishes with a
non-OK status and its open outputs are aborted. Callers do not receive a normal
end-of-stream for a partial report.

## Register and serve the agent

Register the included model action and the Python research handler in the same
registry:

```python
registry = a11.ActionRegistry()
registry.register(
    "interact_with_llm",
    INTERACT_WITH_LLM_SCHEMA,
    interact_with_llm,
)
registry.register(
    "deep-research",
    DEEP_RESEARCH_SCHEMA,
    deep_research,
)

service = a11.Service(action_registry=registry)
```

The service can run over WebSocket, HTTP SSE, WebRTC, or another `WireStream`.
The research handler stays unchanged when it moves behind a service or when a
browser becomes its caller.

## Try the deployed agent

Give the hosted demo a topic. The plan appears while the agent is preparing
its investigations, activity follows the standard action log, and the report
streams as it is synthesized. Several `[investigate]` entries overlap, while
their full intermediate reports stay on the backend.

The hosted demo currently uses the optional Flow spelling described below. It
exposes the same `deep-research` action boundary as the Python handler.

!!! note "Running the demo backend"

    The default endpoint runs Ollama beside the service and needs no key.
    Claude and Gemini require one. To run the backend locally:

    ```sh
    python -m a11.demos.web_demos_server
    ```

    This listens at `ws://127.0.0.1:9010/a11-demos`. A page loaded over HTTPS
    may block a plaintext WebSocket. Supply `--certificate` and
    `--private-key` with a trusted development certificate when needed.

    A run makes one model call to plan, one per brief, and one to synthesize.

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
      <header>action log</header>
      <div id="research-log" class="a11-log"></div>
    </aside>
  </div>
</div>
<script type="module" src="../assets/deep-research.js"></script>

## Call it from a browser

The browser declares the same public ports and does not depend on the Python
implementation:

```ts
const DEEP_RESEARCH_SCHEMA = new ActionSchema({
    name: 'deep-research',
    inputs: {
        topic: new ActionPortSchema({
            name: 'topic', type: 'text/plain', unary: true, required: true,
        }),
    },
    outputs: {
        report: new ActionPortSchema({name: 'report', type: 'text/plain'}),
        plan: new ActionPortSchema({name: 'plan', type: 'text/plain'}),
    },
});
```

Claim the log before dispatch, then read the plan and report concurrently:

```ts
const logs = await claimLog(call);
need(await call.call());

await Promise.all([
    readLogFrom(logs, (line) => addLine(log, line)),
    readPort(call, 'plan', (value) => addPlanItem(String(value))),
    readPort(call, 'report', (value) => appendReport(String(value))),
]);
need(await call.wait(600_000));
```

Reading all outputs concurrently prevents one bounded stream from stalling the
others.

## Optional: express the composition with Flow

Flow can describe the same orchestration when the composition should be loaded,
checked, or changed at runtime. The corresponding section is concise because
actions and streams provide the same execution model:

```a11flow
planned = run plan-research(topic: topic)
planned.briefs -> plan

nodes research {
  findings = node()

  for brief in planned.briefs parallel 3 {
    one = run investigate(topic: topic, brief: brief) timeout 2m
    one.report -> findings
  }

  written = run synthesise-findings(
    topic: topic, brief: planned.synthesis, findings: findings,
  )
  written.report -> report
}
```

This short form also handles work that the Python version spells out:

- The runtime drains every declared action output that the flow does not read.
  An unused model event stream cannot fill its buffer and stall the action.
  `skip output` remains available when the composition should record that an
  output is intentionally ignored.
- `parallel 3` starts up to three investigations without task creation,
  semaphore, and gather code. The bound is part of the composition and applies
  when the source is loaded at runtime.
- `timeout 2m` limits each investigation step. Nested actions also inherit the
  parent's A11 headers, including its overall deadline, so one caller-supplied
  budget reaches planning, investigation, and synthesis.
- An unhandled action or stream failure ends the flow with the same structured
  status. The runtime stops outstanding steps and ends flow-owned streams, so
  the caller does not mistake partial output for success. `cancel step` can
  stop a named action when the composition implements a race or user abort.

Partial research needs an explicit recovery policy. `try run investigate(...)`
allows the flow to inspect the step's status and continue; without `try`, the
failure propagates. The `nodes research` block also keeps investigation ports
and fragments on the service instead of sending them to the caller.

The Python handler is appropriate when orchestration is application code and
normal Python control flow is the clearest expression. Flow is useful when the
composition itself is runtime data, such as a checked plan supplied by a user
or model, and it provides these lifecycle rules without more orchestration
code. The deployed demo's complete Flow source is
[`a11/demos/deep_research.flow`](https://github.com/hpnkv/a11/blob/main/a11/demos/deep_research.flow).
