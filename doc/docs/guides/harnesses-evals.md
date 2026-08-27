# Build a focused agent harness or evaluation path

An agent harness usually owns the model loop, tools, conversation state,
permissions, progress reporting, and one or more user interfaces. An evaluation
framework adds tasks, repeated trials, verifiers, artifacts, and metrics.

A11 provides the runtime layer shared by those systems. Applications can
assemble the parts they need from actions, nodes, registries, sessions, and
stores, without adopting a complete personal-agent product or benchmark
platform.

## Assemble the harness your application needs

[Hermes Agent](https://hermes-agent.nousresearch.com/docs/) demonstrates the
scope of a full open-source harness: persistent sessions and memory, Agent
Skills, MCP tools, command approvals, delegated agents, scheduled work, and
delivery through a CLI, desktop application, or messaging gateway.

That integrated product is useful when those facilities should arrive
together. A product with a narrower job can use A11 at a lower level:

- `interact_with_llm` owns the provider-specific model/tool loop;
- `Interaction` records messages, tool calls, results, usage, and continuation
  identifiers as application data;
- an `ActionRegistry` supplies tools, while a per-turn allow-list applies user
  and workflow policy;
- MCP tools become actions in the same registry;
- an SQLite node store retains a conversation across process restarts;
- a `Session` carries the same action calls to a browser, service, or another
  machine;
- separate output ports carry text, reasoning, logs, and completed interaction
  state to the consumers that need them.

For example, a support application may need durable customer conversations,
three approved business tools, and a streamed web interface. It can build those
requirements directly. It does not need a global agent profile, autonomous
memory, skill management, scheduling, or subagent delegation.

This separation also helps maintenance. The business operation has one
`ActionSchema` whether application code calls it, a model selects it as a tool,
a Flow composes it, or a remote client dispatches it. Moving the handler to a
service does not create another tool wrapper or conversation loop.

## Keep repeatable work outside the model loop

Harnesses often use skills or delegated agents for multi-step work. These are
appropriate when the work needs model judgment or an isolated context. If the
steps and data dependencies are known, a [Flow](flow.md) can be smaller and more
predictable.

A Flow starts actions concurrently and synchronizes them through streamed data.
Intermediate tool results pipe directly between ports. This avoids another
model turn for each transfer, reduces input-token use, and prevents a long tool
result from displacing the task instructions from the context window. The
runtime checks the composition before work starts. When a model submits the
Flow through `flow_run`, the same check covers its per-turn action allow-list.
The application can accept a different checked composition on each request
without deploying a new handler or exposing operations outside its registry.

The [Flow tools guide](../llm-sdk/flow-skill.md) compares executable Flow
semantics with instruction-based Agent Skills in detail.

## Express application evaluations with the same contracts

[Harbor](https://harborframework.com/docs) defines a broad evaluation system:
a task combines an instruction, container environment, and verifier; a trial is
one agent attempt; a job runs trials across tasks, models, and agents. It also
records trajectories, rewards, timing, token use, verifier output, and sandbox
artifacts.

Use Harbor when an evaluation needs portable container environments, published
benchmark datasets, standardized agent adapters, or large experiment grids.
A11 is useful when evaluation is part of the application itself or follows the
same topology as production:

- expose the dataset source, model call, tool, verifier, and result sink as
  actions;
- change a model or remote execution location while retaining port contracts;
- stream tokens, tool activity, partial scores, and completed cases on separate
  nodes;
- use a durable store when trial streams must remain available for inspection;
- use Flow to fan cases out concurrently and collect only declared results;
- keep retrieved documents and other large intermediates in local nodes.

An evaluation can then invoke the production actions. It does not need a second
implementation of tool dispatch or streaming accumulation, and a verifier can
consume the same typed outputs that an application client receives.

This approach is intentionally narrower than a benchmark framework. A11 does
not provide Harbor's container isolation, dataset registry, reward conventions,
or results viewer. It provides reusable execution and data contracts for teams
whose evaluation requirements fit their own application services.

## Choose the layer by the problem

- Choose a full harness such as Hermes for a ready personal agent with memory,
  channels, scheduling, and managed skills.
- Choose Harbor for reproducible, containerized agent or model benchmarks.
- Choose A11 when agent behavior belongs inside an application and must stream,
  cross process or language boundaries, or share contracts with production
  services.
- Combine them when useful: a harness or evaluation runner can call an A11
  service, and A11 actions can wrap tools exposed through open protocols such as
  MCP.
