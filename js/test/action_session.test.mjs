import assert from 'node:assert/strict';
import test from 'node:test';

import {
  ACTION_STATUS_OUTPUT,
  Action,
  ActionMessage,
  ActionPortSchema,
  ActionRegistry,
  ActionSchema,
  InProcessWireStream,
  NodeFragment,
  Session,
  SessionWithRecv,
  StatusCode,
  StreamMode,
  WireMessage,
  cancelledError,
  isOk,
  okStatus,
  statusFromChunk,
  unavailableError,
} from '../dist/index.js';

const delay = (milliseconds) => new Promise(
  (resolve) => setTimeout(resolve, milliseconds),
);

function outputSchema(name = 'task') {
  return new ActionSchema({
    name,
    outputs: {
      result: new ActionPortSchema({ name: 'result', type: 'string' }),
    },
  });
}

async function sessionPair(serverRegistry, clientRegistry = new ActionRegistry()) {
  const client = Session.create({
    id: 'client-session',
    actionRegistry: clientRegistry,
    noStreamTimeoutMs: null,
  });
  const server = Session.create({
    id: 'server-session',
    actionRegistry: serverRegistry,
    noStreamTimeoutMs: null,
  });
  const streams = InProcessWireStream.createPair();
  assert.equal(isOk(client), true);
  assert.equal(isOk(server), true);
  assert.equal(isOk(streams), true);
  const startup = await Promise.all([
    client.addStream(streams[0], StreamMode.START),
    server.addStream(streams[1], StreamMode.ACCEPT),
  ]);
  assert.ok(startup.every(isOk));
  return { client, server, clientStream: streams[0], serverStream: streams[1] };
}

async function closePair(client, server) {
  assert.equal(isOk(client.halfClose()), true);
  assert.equal(isOk(server.halfClose()), true);
  const done = await Promise.all([client.done(), server.done()]);
  assert.ok(done.every(isOk));
}

test('local Action streams output and completion status', async () => {
  const schema = outputSchema('local');
  const action = Action.create(schema, {
    handler: async (running) => {
      const output = await running.getOutput('result');
      if (!isOk(output)) return output;
      const written = await output.putFinal('complete');
      return isOk(written) ? okStatus() : written;
    },
  });
  assert.equal(isOk(action), true);
  assert.equal(isOk(action.run()), true);
  assert.equal(isOk(await action.wait(1000)), true);

  const output = await action.getOutput('result', false);
  assert.equal(isOk(output), true);
  assert.equal(await output.next(1000), 'complete');
  const statusNode = await action.getOutput(ACTION_STATUS_OUTPUT, false);
  assert.equal(isOk(statusNode), true);
  const chunk = await statusNode.nextChunk(1000);
  assert.equal(isOk(chunk), true);
  assert.notEqual(chunk, null);
  assert.equal(isOk(statusFromChunk(chunk)), true);
});

test('Action catches handler failures and cancellation callbacks', async () => {
  const failed = Action.create(outputSchema('failed'), {
    handler: async () => { throw new TypeError('handler exploded'); },
  });
  assert.equal(isOk(failed), true);
  assert.equal(isOk(failed.run()), true);
  const failure = await failed.wait(1000);
  assert.equal(isOk(failure), false);
  assert.equal(failure.code, StatusCode.UNKNOWN);
  assert.equal(failure.message, 'Action handler raised an exception.');

  const cancelled = Action.create(outputSchema('cancelled'), {
    handler: async (running) => {
      await new Promise((resolve) =>
        running.signal.addEventListener('abort', resolve, { once: true }),
      );
      return cancelledError('handler observed cancellation');
    },
  });
  assert.equal(isOk(cancelled), true);
  cancelled.setOnCancelled(() => { throw new TypeError('callback exploded'); });
  assert.equal(isOk(cancelled.run()), true);
  const cancellationCallbackStatus = cancelled.cancel();
  assert.equal(isOk(cancellationCallbackStatus), false);
  assert.equal(cancellationCallbackStatus.code, StatusCode.UNKNOWN);
  const cancellation = await cancelled.wait(1000);
  assert.equal(isOk(cancellation), false);
  assert.equal(cancellation.code, StatusCode.CANCELLED);
});

test('ActionRegistry copies schemas and can clear autofills', () => {
  const schema = new ActionSchema({
    name: 'registered',
    inputs: {
      input: new ActionPortSchema({
        name: 'input',
        type: 'string',
        autofills: [new NodeFragment()],
      }),
    },
  });
  const registry = new ActionRegistry();
  assert.equal(isOk(registry.register('registered', schema)), true);
  const copy = registry.copy(true);
  assert.equal(isOk(copy), true);
  const copiedSchema = copy.getSchema('registered');
  assert.equal(isOk(copiedSchema), true);
  assert.equal(copiedSchema.inputs.get('input').autofills.length, 0);
  assert.deepEqual(registry.listRegisteredActions(), ['registered']);
});

test('sessions call an Action with streaming input and output', async () => {
  const schema = new ActionSchema({
    name: 'echo',
    inputs: {
      input: new ActionPortSchema({ name: 'input', type: 'string' }),
    },
    outputs: {
      output: new ActionPortSchema({ name: 'output', type: 'string' }),
    },
  });
  const serverRegistry = new ActionRegistry();
  assert.equal(isOk(serverRegistry.register('echo', schema, async (action) => {
    const input = await action.getInput('input');
    if (!isOk(input)) return input;
    const value = await input.next({ timeoutMs: 2000 });
    if (!isOk(value)) return value;
    const output = await action.getOutput('output');
    if (!isOk(output)) return output;
    const stored = await output.putFinal(`reply:${value}`);
    return isOk(stored) ? okStatus() : stored;
  })), true);
  const clientRegistry = new ActionRegistry();
  assert.equal(isOk(clientRegistry.register('echo', schema)), true);
  const pair = await sessionPair(serverRegistry, clientRegistry);

  const action = clientRegistry.makeAction('echo', {
    nodeMap: pair.client.getNodeMap(),
    stream: pair.clientStream,
    session: pair.client,
  });
  assert.equal(isOk(action), true);
  assert.equal(isOk(await action.call()), true);
  const input = await action.getInput('input');
  assert.equal(isOk(input), true);
  assert.equal(isOk(await input.putFinal('request')), true);
  assert.equal(isOk(await action.waitForDispatch(2000)), true);
  assert.equal(isOk(await action.wait(2000)), true);
  const output = await action.getOutput('output', false);
  assert.equal(isOk(output), true);
  assert.equal(await output.next(2000), 'reply:request');
  assert.equal(pair.client.actions().length, 0);
  assert.equal(pair.server.actions().length, 0);
  await closePair(pair.client, pair.server);
});

test('remote dispatch and handler failures remain Action-local statuses', async () => {
  const missingSchema = outputSchema('missing');
  const clientRegistry = new ActionRegistry();
  assert.equal(isOk(clientRegistry.register('missing', missingSchema)), true);
  const emptyServerRegistry = new ActionRegistry();
  let pair = await sessionPair(emptyServerRegistry, clientRegistry);
  let action = clientRegistry.makeAction('missing', {
    nodeMap: pair.client.getNodeMap(),
    stream: pair.clientStream,
    session: pair.client,
  });
  assert.equal(isOk(action), true);
  assert.equal(isOk(await action.call()), true);
  const dispatch = await action.waitForDispatch(2000);
  assert.equal(isOk(dispatch), false);
  assert.equal(dispatch.code, StatusCode.NOT_FOUND);
  const completion = await action.wait(2000);
  assert.equal(isOk(completion), false);
  assert.equal(completion.code, StatusCode.NOT_FOUND);
  assert.equal(isOk(pair.clientStream.getStatus()), true);
  await closePair(pair.client, pair.server);

  const failingServer = new ActionRegistry();
  const failingSchema = outputSchema('remote-failure');
  assert.equal(isOk(failingServer.register(
    'remote-failure',
    failingSchema,
    () => unavailableError('model backend unavailable', [{ retry: true }]),
  )), true);
  const failingClient = new ActionRegistry();
  assert.equal(isOk(failingClient.register('remote-failure', failingSchema)), true);
  pair = await sessionPair(failingServer, failingClient);
  action = failingClient.makeAction('remote-failure', {
    nodeMap: pair.client.getNodeMap(),
    stream: pair.clientStream,
    session: pair.client,
  });
  assert.equal(isOk(action), true);
  assert.equal(isOk(await action.call()), true);
  assert.equal(isOk(await action.waitForDispatch(2000)), true);
  const failed = await action.wait(2000);
  assert.equal(isOk(failed), false);
  assert.equal(failed.code, StatusCode.UNAVAILABLE);
  assert.deepEqual(failed.details, [{ retry: true }]);
  const output = await action.getOutput('result', false);
  assert.equal(isOk(output), true);
  const outputFailure = await output.next(2000);
  assert.equal(isOk(outputFailure), false);
  assert.equal(outputFailure.code, StatusCode.UNAVAILABLE);
  await closePair(pair.client, pair.server);
});

test('remote Action cancellation propagates and releases both sessions', async () => {
  let serverObservedCancellation = false;
  const schema = outputSchema('long-task');
  const serverRegistry = new ActionRegistry();
  assert.equal(isOk(serverRegistry.register('long-task', schema, async (action) => {
    await new Promise((resolve) =>
      action.signal.addEventListener('abort', resolve, { once: true }),
    );
    serverObservedCancellation = true;
    return cancelledError('cancelled by peer');
  })), true);
  const clientRegistry = new ActionRegistry();
  assert.equal(isOk(clientRegistry.register('long-task', schema)), true);
  const pair = await sessionPair(serverRegistry, clientRegistry);
  const action = clientRegistry.makeAction('long-task', {
    nodeMap: pair.client.getNodeMap(),
    stream: pair.clientStream,
    session: pair.client,
  });
  assert.equal(isOk(action), true);
  assert.equal(isOk(await action.call()), true);
  assert.equal(isOk(await action.waitForDispatch(2000)), true);
  assert.equal(isOk(action.cancel()), true);
  const completion = await action.wait(2000);
  assert.equal(isOk(completion), false);
  assert.equal(completion.code, StatusCode.CANCELLED);
  for (let attempt = 0; attempt < 100 && !serverObservedCancellation; ++attempt) {
    await delay(10);
  }
  assert.equal(serverObservedCancellation, true);
  for (let attempt = 0; attempt < 100 && pair.client.actions().length > 0; ++attempt) {
    await delay(10);
  }
  assert.equal(pair.client.actions().length, 0);
  assert.equal(pair.server.actions().length, 0);
  await closePair(pair.client, pair.server);
});

test('SessionWithRecv provides pull-style backpressure and terminal receive', async () => {
  const sender = Session.create({ id: 'sender', noStreamTimeoutMs: null });
  const receiver = SessionWithRecv.create({ id: 'receiver', noStreamTimeoutMs: null });
  const streams = InProcessWireStream.createPair();
  assert.equal(isOk(sender), true);
  assert.equal(isOk(receiver), true);
  assert.equal(isOk(streams), true);
  assert.ok((await Promise.all([
    sender.addStream(streams[0], StreamMode.START),
    receiver.addStream(streams[1], StreamMode.ACCEPT),
  ])).every(isOk));

  const message = new WireMessage({ headers: new Map([
    ['x-test', new Uint8Array([1])],
  ]) });
  // A message with only headers is the protocol terminal, so use an Action
  // element to keep this one non-terminal without requiring a registry.
  message.actions.push(new ActionMessage({
    id: 'raw-action',
    name: 'raw',
  }));
  assert.equal(isOk(sender.send(message)), true);
  const received = await receiver.receiveWithStreamId(2000);
  assert.equal(isOk(received), true);
  assert.notEqual(received, null);
  assert.equal(received.streamId, streams[1].getId());
  assert.equal(received.message.actions[0].name, 'raw');

  const timeout = await receiver.receive(5);
  assert.equal(isOk(timeout), false);
  assert.equal(timeout.code, StatusCode.DEADLINE_EXCEEDED);
  assert.equal(isOk(sender.halfClose()), true);
  assert.equal(await receiver.receive(2000), null);
  assert.equal(isOk(receiver.halfClose()), true);
  assert.ok((await Promise.all([sender.done(), receiver.done()])).every(isOk));
});

test('Session validates fulfilled statuses from foreign WireStreams', async () => {
  const methods = {
    send: () => undefined,
    start: async () => ({ unexpected: true }),
    accept: async () => ({ unexpected: true }),
    halfClose: () => undefined,
    drainOutgoingMessages: async () => ({ unexpected: true }),
    abort: () => undefined,
    setDeadline: () => undefined,
    getDeadline: () => null,
    getStatus: () => okStatus(),
    getTrailers: () => null,
    getId: () => 'foreign-stream',
    getImpl: () => null,
    wait: async () => ({ unexpected: true }),
  };
  const rejectedSession = Session.create({ noStreamTimeoutMs: null });
  assert.equal(isOk(rejectedSession), true);
  const startup = await rejectedSession.addStream(methods);
  assert.equal(isOk(startup), false);
  assert.equal(startup.code, StatusCode.INTERNAL);

  let onDone;
  const attachedStream = {
    ...methods,
    getId: () => 'attached-stream',
    start: async (_onMessage, callback) => {
      onDone = callback;
      return okStatus();
    },
  };
  const session = Session.create({ noStreamTimeoutMs: null });
  assert.equal(isOk(session), true);
  assert.equal(isOk(await session.addStream(attachedStream)), true);
  const sent = session.send(new WireMessage({
    actions: [new ActionMessage({ id: 'test-action', name: 'test' })],
  }));
  assert.equal(isOk(sent), false);
  assert.equal(sent.code, StatusCode.INTERNAL);
  assert.equal(isOk(await onDone()), true);
});
