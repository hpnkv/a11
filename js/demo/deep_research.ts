/**
 * "Deep research in Flow" guide demo.
 *
 * The page dispatches one action, `deep-research`, and reads two of its output
 * ports plus its log. That action is not code on the backend: it is the
 * composition in `a11/demos/deep_research.flow`, which plans the topic,
 * investigates the parts of it at the same time, and writes one report. The
 * intermediate findings never reach this page — the flow keeps them in a node
 * map of its own — so what crosses the socket is the plan, the report, and the
 * narration.
 *
 * The narration arrives on the action's **log**, not on a port of its own. Every
 * action has one, so this is the reading that works for any action a page calls;
 * a narration *port* only works for a client that has been told its name.
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
 * A flow *is* an action, so this page neither knows nor cares that the thing it
 * calls is a composition. The port names and shapes are the whole contract.
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
      // The provider is named once, on the composition. Every model call inside
      // it is a nested action, and A11 hands a nested action its parent's
      // `x-a11-` headers — which is why the flow says nothing about providers.
      for (const [header, value] of LlmHeadersFor(this.backend.value)) {
        need(call.setHeader(header, value));
      }
      // Claimed before dispatch, which is the only time it can be: a line
      // logged before anything holds the port goes to the server's log.
      const logs = await claimLog(call);
      need(await call.call());

      const topicInput = need(await call.getInput('topic'));
      need(await topicInput.finalize(topic));

      // Everything at once: the narration is the point of watching, and it
      // arrives while the report is still being written.
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
