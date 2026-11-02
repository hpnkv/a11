import assert from 'node:assert/strict';
import test from 'node:test';

import {statusFromBridgeFailure} from '../dist/internal/wasm/status_bridge.js';

test('bridge failures retain native statuses and describe host errors', () => {
  const native = {status: {code: 14, message: 'socket closed', details: []}};
  assert.deepEqual(statusFromBridgeFailure(native, 'Channel startup failed.'),
    native.status);
  const host = statusFromBridgeFailure(new TypeError('worker unavailable'),
    'Channel startup failed.');
  assert.equal(host.message, 'Channel startup failed. worker unavailable');
});
