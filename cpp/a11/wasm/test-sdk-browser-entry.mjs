import {ActionPortSchema, ActionSchema} from '../../../js/dist/action_schema.js';
import {createA11Runtime} from '../../../js/dist/runtime_backend.js';
import {isOk} from '../../../js/dist/status.js';

const result = document.getElementById('result');
let runtime;
try {
  runtime = await createA11Runtime({wasm: {
    assetsBaseUrl: new URL('./build/dist/', location.href),
  }});
  if (!isOk(runtime)) throw new Error(runtime.message);
  const node = await runtime.createNode('sdk-browser-node');
  if (!isOk(node)) throw new Error(node.message);
  const ended = await node.finalize('browser');
  if (!isOk(ended)) throw new Error(ended.message);
  const value = await node.next(2000);
  if (value !== 'browser') throw new Error('native node value differs');

  const graph = runtime.graph();
  if (!isOk(graph)) throw new Error(graph.message);
  const schema = new ActionSchema({name: 'sdk_browser_action', outputs: {
    result: new ActionPortSchema({name: 'result', type: 'string'}),
  }});
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
