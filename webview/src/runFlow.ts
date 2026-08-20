/**
 * Running a flow on the gateway, with its ports as streams.
 *
 * The gateway's `run_flow` takes a composition's *source* and leaves the flow's
 * own ports as nodes: this side writes each input and reads each output while the
 * flow runs, rather than sending one object of values and waiting for another.
 * Both ends work the node ids out from the call's own id — `<call id>-flow#<port>`
 * — so nothing has to be announced, and subscribing before the source is sent
 * means nothing produced early can be missed.
 *
 * Arity is not a second mechanism here any more than it is on the wire: a port
 * declared `stream` takes the values it is given one after another, a port that
 * carries one takes one, and either way this side closes it when it is done.
 */

import {
  Action,
  ActionPortSchema,
  ActionSchema,
  StatusCode,
  isOk,
  isStatusChunk,
  logRecordFromChunk,
  logText,
  type AsyncNode,
  type Session,
  type Status,
  type WireStream,
} from '@curiositystack/a11';

const need = <T>(value: T | Status): T => {
  if (!isOk(value)) throw new Error(`${StatusCode[(value as Status).code]}: ${(value as Status).message}`);
  return value as T;
};

/**
 * What `run_flow` names the action it runs the composition as, relative to its
 * own id — the other half of the node-id contract
 * (`a11.sdk.flow_tools.FLOW_ACTION_SUFFIX`).
 */
const FLOW_ACTION_SUFFIX = '-flow';

/** Mirrors the gateway's `flow_run` schema (`a11/sdk/flow_tools/schemas.py`). */
const FLOW_RUN_SCHEMA = new ActionSchema({
  name: 'flow_run',
  description: 'Run a flow: a composition of actions, dispatched as one step.',
  inputs: {
    source: new ActionPortSchema({ name: 'source', type: 'text/plain', unary: true, required: true }),
    inputs: new ActionPortSchema({ name: 'inputs', type: 'application/json', unary: true }),
    input_streams: new ActionPortSchema({ name: 'input_streams', type: 'application/json', unary: true }),
    flow: new ActionPortSchema({ name: 'flow', type: 'text/plain', unary: true }),
  },
  outputs: {
    result: new ActionPortSchema({ name: 'result', type: 'application/json', unary: true, required: true }),
  },
});

/** Everything one `run_flow` call needs, beyond the session it goes out on. */
export interface FlowRun {
  /** The flow's text. */
  source: string;
  /** Which flow in the source; the first one declared by default. */
  flow?: string;
  /** Headers to set on the call, e.g. the LLM provider and the allow-list. */
  headers?: Record<string, string>;
  /**
   * Values for the flow's input ports, keyed by port name — always a list,
   * because one value and several are the same thing here. A port left out is
   * closed empty, which is what a port carrying no values is.
   */
  inputs?: Record<string, unknown[]>;
  /**
   * Output ports to read as they fill, keyed by port name. Each callback is
   * handed one value at a time, in the order the flow produced them.
   */
  outputs?: Record<string, (value: unknown) => void>;
  /** The run log the gateway narrates, for a UI that wants to show it. */
  onLog?: (log: string) => void;
  /** How long to wait for the whole composition. */
  timeoutMs?: number;
}

/**
 * Run one flow on the gateway and return its collected outputs.
 *
 * The return value is `run_flow`'s own `result`: every output port keyed by name,
 * one value for a port that carries one and a list for a `stream`. It arrives at
 * the end, which is exactly why `outputs` exists — a caller that wants to show
 * values as they are produced reads them there and can ignore what comes back.
 */
export async function runFlow(
  session: Session,
  stream: WireStream,
  run: FlowRun,
): Promise<Record<string, unknown>> {
  const timeoutMs = run.timeoutMs ?? 600_000;
  const call = need(Action.create(FLOW_RUN_SCHEMA, { session, stream, nodeMap: session.getNodeMap() }));
  for (const [name, value] of Object.entries(run.headers ?? {})) {
    if (value) need(call.setHeader(name, value));
  }
  need(await call.call());

  // Before a single input is written: a node holds what it was given from the
  // moment it exists, so subscribing first is what makes "as they are produced"
  // true for the first value as well as the hundredth.
  const flowPort = async (port: string): Promise<AsyncNode> => {
    const id = need(Action.makeNodeId(call.getId() + FLOW_ACTION_SUFFIX, port));
    return need(await session.getNodeMap().get(id));
  };
  const reading = Object.entries(run.outputs ?? {}).map(([port, sink]) => {
    const task = (async () => {
      const node = await flowPort(port);
      for (;;) {
        const value = need(await node.next({ timeoutMs }));
        if (value === null) break;
        sink(value);
      }
    })();
    // A read left behind because the call failed before the flow started must
    // not resurface as an unhandled rejection when it eventually times out. The
    // call's own status is the failure worth reporting, and it is raised below.
    task.catch(() => undefined);
    return task;
  });

  // The flow's own input ports, filled and closed by this side. Written before
  // the source for the same reason the outputs are subscribed first: it cannot
  // be too early, and it can be too late.
  const streamed = Object.keys(run.inputs ?? {});
  for (const [port, values] of Object.entries(run.inputs ?? {})) {
    const node = await flowPort(port);
    // A port this end writes has to reach the other end.
    need(node.attachStream(stream));
    for (const value of values) need(await node.put(value));
    need(await node.finalize());
  }

  // Every input port of the *call*, including the ones there is nothing for: an
  // input nobody writes and nobody closes is one the gateway waits on, and with
  // no deadline on this call it would wait for good.
  const write = async (port: string, value: unknown | null): Promise<void> => {
    const node = need(await call.getInput(port));
    need(await node.finalize(value ?? undefined));
  };
  // `input_streams` is how the gateway knows to leave those ports open for the
  // writes above rather than closing them empty before the flow reads them.
  await write('input_streams', streamed.length > 0 ? streamed : null);
  // Nothing goes here: a value written into the call would arrive as plain JSON
  // and be decided before the flow starts, which is what the nodes are for.
  await write('inputs', null);
  await write('flow', run.flow ?? null);
  await write('source', run.source);
  // What the flow narrated, off the log port every action has. Not one of
  // `flow_run`'s outputs -- it is not in its schema, which is what keeps it out
  // of the result -- and nothing has to close it: the action does.
  const logging = (async () => {
    const node = need(await call.getLogNode());
    for (;;) {
      const chunk = need(await node.nextChunk(timeoutMs));
      if (chunk === null) break;
      if (isStatusChunk(chunk)) continue;
      run.onLog?.(logText(logRecordFromChunk(chunk)));
    }
  })();
  logging.catch(() => undefined);
  const collected = need(await call.getOutput('result', false));
  const collecting = collected.consume({ timeoutMs, allowNone: true });
  collecting.catch(() => undefined);

  // The call's own status first, because it is the only thing here that reports
  // a failure. A composition refused before it started -- a flow that names a
  // port an action does not have, an action this caller may not reach -- closes
  // its outputs empty and never writes the nodes above, so waiting on those
  // would be waiting for a flow that is already over. This is what turns that
  // into the error it is.
  need(await call.wait(timeoutMs));

  const result = await collecting;
  need(result);
  // The flow has finished, so everything it wrote has been sent and the readers
  // are draining what is already here. Bounded all the same: a port that never
  // sees its end -- one this caller named and the flow does not declare -- must
  // not hold up the answer.
  await Promise.race([Promise.all([...reading, logging]), delay(DRAIN_GRACE_MS)]);
  return (result as Record<string, unknown> | null) ?? {};
}

/** How long to let the output readers finish once the flow itself has. */
const DRAIN_GRACE_MS = 5_000;

const delay = (ms: number): Promise<void> =>
  new Promise((resolve) => setTimeout(resolve, ms));
