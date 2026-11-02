import assert from 'node:assert/strict';
import test from 'node:test';

import {
  channelBridge,
  installChannelBridge,
  removeChannelBridge,
} from '../dist/internal/wasm/native_channel.js';

test('new channels use the newest live runtime bridge', () => {
  const first = {createChannelStream() {}};
  const second = {createChannelStream() {}};
  installChannelBridge(first);
  installChannelBridge(second);
  try {
    assert.equal(channelBridge(), second);
    removeChannelBridge(first);
    assert.equal(channelBridge(), second);
  } finally {
    removeChannelBridge(second);
    removeChannelBridge(first);
  }
  assert.equal(channelBridge(), null);
});
