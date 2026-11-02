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

import assert from 'node:assert/strict';
import test from 'node:test';
import {NodeRef, StatusCode, isOk} from '../dist/index.js';
const need = value => { assert.ok(isOk(value), value.message); return value; };

test('reference credentials are typed, copied, and redacted', () => {
  const original = new NodeRef({id: 'target', storage: {id: 'shared', services: [{role: 'payload'}]}});
  const qualified = need(original.withCredentials({scheme: 'bearer', fields: {token: 'namespace-secret'}}));
  const service = need(qualified.withCredentials({scheme: 'bearer', fields: {token: 'service-secret'}}, 'payload'));
  assert.equal(original.storage.credentials, undefined);
  assert.equal(qualified.storage.credentials.fields.token, 'namespace-secret');
  assert.equal(service.storage.services[0].credentials.fields.token, 'service-secret');
  assert.equal(need(NodeRef.fromMsgpack(need(service.toMsgpack()))).storage.services[0].credentials.fields.token, 'service-secret');
  assert.ok(!String(service).includes('namespace-secret'));
  assert.equal(new NodeRef({id: 'local'}).withCredentials({scheme: 'bearer'}).code, StatusCode.FAILED_PRECONDITION);
});
