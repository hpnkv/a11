import {ActionPortSchema, ActionSchema} from '../../../js/dist/action_schema.js';
import {createA11Runtime} from '../../../js/dist/runtime_backend.js';
import {NodeRef} from '../../../js/dist/data.js';
import {ByteReassembler, splitBytesIntoPackets} from '../../../js/dist/byte_chunking.js';
import {statusToChunk, decodeStatusChunk} from '../../../js/dist/action_schema.js';
import {encodeAuthorization, decodeAuthorization} from '../../../js/dist/authorization.js';
import {streamReferenceToChunk, streamReferenceFromChunk} from '../../../js/dist/stream_reference.js';
import {toChunk, fromChunk} from '../../../js/dist/serialization.js';
import {schemaToJson, schemaFromJson} from '../../../js/dist/schema_json.js';
import {isOk, StatusCode} from '../../../js/dist/status.js';
import {InProcessWireStream} from '../../../js/dist/in_process_wire_stream.js';
import {WireStreamWithRecv} from '../../../js/dist/wire_stream.js';
import {WireMessage, NodeFragment, Chunk} from '../../../js/dist/data.js';

const result = document.getElementById('result');
let runtime;
try {
  runtime = await createA11Runtime({wasm: {
    assetsBaseUrl: new URL('./build/dist/', location.href),
  }});
  if (!isOk(runtime)) throw new Error(runtime.message);
  const node = await runtime.createNode('sdk-browser-node');
  if (!isOk(node)) throw new Error(node.message);
  const waiting = node.next(2000);
  const ended = await node.finalize('browser');
  if (!isOk(ended)) throw new Error(ended.message);
  const value = await waiting;
  if (value !== 'browser') throw new Error('native node value differs');
  const cancelledNode = await runtime.createNode('sdk-browser-cancel');
  if (!isOk(cancelledNode)) throw new Error(cancelledNode.message);
  const controller = new AbortController();
  controller.abort();
  if ((await cancelledNode.next({signal: controller.signal})).code !== StatusCode.CANCELLED) {
    throw new Error('native node cancellation differs');
  }
  const bytes = new TextEncoder().encode('native framing in the browser');
  const packets = splitBytesIntoPackets(bytes, 23n, 20);
  if (!isOk(packets)) throw new Error(packets.message);
  const reassembler = ByteReassembler.create({packetSize: 20});
  if (!isOk(reassembler)) throw new Error(reassembler.message);
  let assembled = null;
  for (const packet of packets.toReversed()) assembled = reassembler.feed(packet) ?? assembled;
  if (new TextDecoder().decode(assembled) !== new TextDecoder().decode(bytes)) {
    throw new Error('native framing differs');
  }
  reassembler.dispose();
  const pair = InProcessWireStream.createPair();
  if (!isOk(pair)) throw new Error(pair.message);
  const sending = WireStreamWithRecv.create(pair[0]);
  const receiving = WireStreamWithRecv.create(pair[1]);
  const opened = await Promise.all([sending.start(), receiving.accept()]);
  if (!opened.every(isOk)) throw new Error('native channel startup differs');
  const message = new WireMessage({nodeFragments: [new NodeFragment({
    id: 'browser-channel', data: new Chunk({data: bytes}),
  })]});
  if (!isOk(await sending.send(message))) throw new Error('native channel send failed');
  const received = await receiving.receive(2000);
  if (!isOk(received) || received.nodeFragments[0].id !== 'browser-channel') {
    throw new Error('native channel receive differs');
  }
  if (!isOk(await sending.halfClose()) || await receiving.receive(2000) !== null) {
    throw new Error('native channel half-close differs');
  }
  const reference = new NodeRef({id: 'sdk-browser-node', storage: {id: 'local'}});
  const encodedRef = reference.toMsgpack();
  if (!isOk(encodedRef) || !(NodeRef.fromMsgpack(encodedRef) instanceof NodeRef)) {
    throw new Error('native reference codec differs');
  }
  const statusChunk = statusToChunk({code: StatusCode.PERMISSION_DENIED,
    message: 'denied'}, true);
  if (!isOk(statusChunk) ||
      decodeStatusChunk(statusChunk).status.code !== StatusCode.PERMISSION_DENIED) {
    throw new Error('native status chunk differs');
  }
  const envelope = {version: 1, chain: ['a.b.c']};
  const encodedAuth = encodeAuthorization(envelope);
  if (!isOk(encodedAuth) || decodeAuthorization(encodedAuth).chain[0] !== 'a.b.c') {
    throw new Error('native authorization codec differs');
  }
  const descriptor = {producer_id: 'browser', parent_producer_id: '',
    node: reference, purpose: 'llm_usage', source: {type: 'a11_action'}};
  const descriptorChunk = streamReferenceToChunk(descriptor);
  if (!isOk(descriptorChunk) ||
      !(streamReferenceFromChunk(descriptorChunk).node instanceof NodeRef)) {
    throw new Error('native stream reference differs');
  }
  const generic = await toChunk({prompt: 'hello'});
  if (!isOk(generic) || (await fromChunk(generic)).prompt !== 'hello') {
    throw new Error('native generic JSON serialization differs');
  }
  const binary = await toChunk(bytes);
  if (!isOk(binary) ||
      new TextDecoder().decode(await fromChunk(binary)) !==
        new TextDecoder().decode(bytes)) {
    throw new Error('native byte serialization differs');
  }
  const text = await toChunk('browser text');
  if (!isOk(text) || await fromChunk(text) !== 'browser text') {
    throw new Error('native text serialization differs');
  }

  const graph = runtime.graph();
  if (!isOk(graph)) throw new Error(graph.message);
  const schema = new ActionSchema({name: 'sdk_browser_action', outputs: {
    result: new ActionPortSchema({name: 'result', type: 'string'}),
  }});
  const described = schemaToJson(schema);
  if (schemaFromJson(described).name !== schema.name) {
    throw new Error('native schema description differs');
  }
  const unregister = await graph.registerAction(schema, async action => {
    const output = await action.getOutput('result');
    return isOk(output) ? output.finalize('native') : output;
  });
  if (!isOk(unregister)) throw new Error(unregister.message);
  const action = await runtime.createAction(schema);
  if (!isOk(action)) throw new Error(action.message);
  const started = await action.run();
  if (!isOk(started)) throw new Error(started.message);
  const completed = await action.wait(2000);
  if (!isOk(completed)) throw new Error(completed.message);
  const output = await action.getOutput('result');
  if (!isOk(output) || await output.next(2000) !== 'native') {
    throw new Error('native action output differs');
  }
  const session = await runtime.createSession({id: 'sdk-browser-session',
    noStreamTimeoutMs: null});
  if (!isOk(session)) throw new Error(session.message);
  const sessionPair = InProcessWireStream.createPair();
  if (!isOk(sessionPair)) throw new Error(sessionPair.message);
  const attached = await Promise.all([
    sessionPair[1].accept(), session.addStream(sessionPair[0]),
  ]);
  if (!attached.every(isOk) || !isOk(await session.createAction(schema))) {
    throw new Error('native browser session stream id differs');
  }
  const stopped = await session.abort();
  if (!isOk(stopped)) throw new Error(stopped.message);
  const flow = await runtime.runFlow({
    source: 'flow sdk_browser { out result: string\n "flow" -> result\n }',
    flow: 'sdk_browser',
  });
  if (!isOk(flow) || flow.result.length !== 1) {
    throw new Error('native Flow output differs');
  }
  const removed = await unregister();
  if (!isOk(removed)) throw new Error(removed.message);
  result.textContent = 'PASS';
} catch (error) {
  result.textContent = `FAIL: ${error?.stack ?? error}`;
} finally {
  if (runtime && isOk(runtime)) await runtime.dispose();
}
