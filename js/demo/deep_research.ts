/**
 * "Deep research in Flow" guide demo.
 *
 * The page dispatches one action, `deep-research`, and reads two of its output
 * ports plus its log. That action is not code on the backend: it is the
 * composition in `a11/demos/deep_research.flow`, which plans the topic,
 * investigates the parts of it at the same time, and writes one report. The
 * intermediate findings stay in the flow's node map. The socket carries the
 * plan, report, and log.
 *
 * The narration arrives on the action's **log**, not on a port of its own.
 * Every action has a log, so clients can read progress without a dedicated
 * output port name.
 */

import {ActionPortSchema, ActionSchema} from '../src/index.js';

import {
  BackendControls,
  DEFAULT_SERVER_URL,
  LlmHeadersFor,
  addLine,
  claimLog,
  connect,
  makeCall,
  need,
  readLogFrom,
  readPort,
  showError,
  whileBusy,
} from './demo_support.js';

/**
 * The ports of `flow deep-research`, declared here by hand.
 *
 * A flow uses the action interface; its port names and shapes define the page's
 * contract.
 */
const DEEP_RESEARCH_SCHEMA = new ActionSchema({
  name: 'deep-research',
  description: 'Plan a topic, investigate its parts at once, and write a report.',
  inputs: {topic: new ActionPortSchema({name: 'topic', type: 'text/plain', unary: true, required: true})},
  outputs: {
    report: new ActionPortSchema({name: 'report', type: 'text/plain'}),
    plan: new ActionPortSchema({name: 'plan', type: 'text/plain'}),
  },
});

class DeepResearchDemo {
  private readonly backend = new BackendControls('research');
  private readonly errors = document.querySelector<HTMLDivElement>('#research-errors')!;
  private readonly report = document.querySelector<HTMLDivElement>('#research-report')!;
  private readonly plan = document.querySelector<HTMLOListElement>('#research-plan')!;
  private readonly log = document.querySelector<HTMLDivElement>('#research-log')!;

  async run(topic: string): Promise<void> {
    this.errors.textContent = '';
    this.report.textContent = '';
    this.plan.replaceChildren();
    this.log.replaceChildren();

    try {
      const connection = await connect(this.backend.server.value.trim() || DEFAULT_SERVER_URL);
      const call = makeCall(connection, DEEP_RESEARCH_SCHEMA);
      // Nested model calls inherit the composition's `x-a11-` headers, so the
      // provider is configured once on the outer action.
      for (const [header, value] of LlmHeadersFor(this.backend.value)) {
        need(call.setHeader(header, value));
      }
      // Claimed before dispatch, which is the only time it can be: a line
      // logged before anything holds the port goes to the server's log.
      const logs = await claimLog(call);
      need(await call.call());

      const topicInput = need(await call.getInput('topic'));
      need(await topicInput.finalize(topic));

      // Read narration and report output concurrently so progress arrives
      // while the report is still being written.
      await Promise.all([
        readLogFrom(logs, (line, level) => addLine(this.log, line, level === 'info' ? '' : level)),
        readPort(call, 'plan', (value) => {
          const item = document.createElement('li');
          item.textContent = String(value);
          this.plan.append(item);
        }),
        readPort(call, 'report', (value) => {
          this.report.textContent = `${this.report.textContent ?? ''}${String(value)}`;
        }),
      ]);
      need(await call.wait(600_000));
      addLine(this.log, 'done.', 'done');
      connection.session.halfClose();
    } catch (error) {
      showError(this.errors, error);
    }
  }
}

const root = document.querySelector('#research-demo');
if (root) {
  const demo = new DeepResearchDemo();
  const form = document.querySelector<HTMLFormElement>('#research-form')!;
  const input = document.querySelector<HTMLInputElement>('#research-topic')!;
  form.onsubmit = (event) => {
    event.preventDefault();
    const topic = input.value.trim();
    if (!topic) return;
    void whileBusy(form, () => demo.run(topic));
  };
}
