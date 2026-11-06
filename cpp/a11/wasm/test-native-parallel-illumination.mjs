import assert from 'node:assert/strict';
import {createA11WasmRuntime} from './native-index.mjs';

const runtime = await createA11WasmRuntime();
const source = `flow illuminate {
  in ids: integer stream required
  in seconds: integer stream required
  out flashes: integer stream
  for id, duration in std::zip(ids, seconds) parallel 5 {
    int flash_count = duration + duration + duration + duration
    for colour in ["red", "blue", "red", "blue",
                   "red", "blue", "red", "blue",
                   "red", "blue", "red", "blue",
                   "red", "blue", "red", "blue"]
        | flatten | first flash_count | pace 250ms {
      changed = run set_color(ids: id, colors: colour)
      changed.recoloured -> flashes
    }
  }
}
declare flow set_color {
  in ids: integer stream required
  in colors: string stream required
  out recoloured: integer
}`;
const chunk = value => ({
  data: Buffer.from(JSON.stringify(value)).toString('base64'),
  metadata: {mimetype: 'application/json'},
});
const read = async (action, port) => {
  const input = await action.input(port);
  const values = [];
  for (;;) {
    const next = await input.nextChunk({timeoutMs: 5000});
    if (next === null) break;
    values.push(JSON.parse(Buffer.from(next.data, 'base64').toString()));
  }
  return values;
};
const counts = new Map();
runtime.registerAction({
  name: 'set_color',
  inputs: [
    {name: 'ids', type: 'integer', required: true},
    {name: 'colors', type: 'string', required: true},
  ],
  outputs: [{name: 'recoloured', type: 'integer', unary: true}],
}, async action => {
  const [ids, colors] = await Promise.all([read(action, 'ids'),
    read(action, 'colors')]);
  assert.equal(ids.length, 1);
  assert.equal(colors.length, 1);
  counts.set(ids[0], (counts.get(ids[0]) ?? 0) + 1);
  const output = await action.output('recoloured');
  await output.putChunk(chunk(1));
  await output.finalize();
});

try {
  const result = await runtime.runFlow({source, flow: 'illuminate',
    inputs: {
      ids: [0, 1, 2, 3, 4].map(chunk),
      seconds: [0, 1, 2, 3, 4].map(chunk),
    }, timeoutMs: 5000, maxOutputChunks: 200});
  assert.equal(result.flashes.length, 40);
  assert.deepEqual([...counts].sort((a, b) => a[0] - b[0]),
    [[1, 4], [2, 8], [3, 12], [4, 16]]);
} finally {
  await runtime.dispose();
}
