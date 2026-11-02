/**
 * Copyright 2026 The A11 Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

import {
  Action, ActionPortSchema, ActionRegistry, ActionSchema,
  FlowProgram, FlowTooling, createA11Runtime, valueOrThrow,
} from '../dist/runtime.js';

const runtime = valueOrThrow(await createA11Runtime({wasm: {
  moduleUrl: new URL('../../cpp/a11/wasm/build/dist/index.mjs', import.meta.url),
  flowRuntimeUrl: new URL('../../cpp/a11/wasm/build/dist/native.mjs', import.meta.url),
}}));
try {
  const schema = new ActionSchema({name: 'greet', inputs: {
    name: new ActionPortSchema({name: 'name', type: 'string', required: true,
      unary: true}),
  }, outputs: {
    result: new ActionPortSchema({name: 'result', type: 'string', unary: true}),
  }});
  const registry = new ActionRegistry(runtime);
  valueOrThrow(await registry.register(schema.name, schema,
    async action => {
      const input = valueOrThrow(await action.getInput('name'));
      const name = valueOrThrow(await input.next());
      const output = valueOrThrow(await action.getOutput('result'));
      return output.finalize(`Hello, ${name}`);
    }));
  try {
    const action = valueOrThrow(await Action.create(schema, {runtime}));
    const input = valueOrThrow(await action.getInput('name'));
    const output = valueOrThrow(await action.getOutput('result'));
    valueOrThrow(await input.finalize('A11'));
    valueOrThrow(await action.run());
    const greeting = valueOrThrow(await output.next(2000));
    valueOrThrow(await action.wait(2000));
    console.log(greeting);

    const source = 'flow hello { out result: string\n "hi" -> result\n }';
    valueOrThrow(new FlowTooling(runtime).check(source));
    const flow = valueOrThrow(await new FlowProgram(runtime, source,
      'hello').run());
    console.log(`Flow returned ${flow.result.length} chunk`);
  } finally {
    valueOrThrow(await registry.unregister(schema.name));
  }
} finally {
  valueOrThrow(await runtime.dispose());
}
