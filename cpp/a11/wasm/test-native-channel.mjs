import assert from 'node:assert/strict';
import {createA11WasmRuntime} from './native-index.mjs';

const OK = {code: 0, message: '', details: []};

function channelPair() {
  const make = () => {
    let callbacks = null;
    let peer = null;
    let opened = false;
    return {
      setPeer(value) { peer = value; },
      callbacks() { return callbacks; },
      setCallbacks(value) { callbacks = value; return OK; },
      resetCallbacks() { callbacks = null; return OK; },
      async open() { opened = true; callbacks?.onOpen(); return OK; },
      isOpen() { return opened; },
      send(bytes) {
        queueMicrotask(() => peer.callbacks()?.onMessage(bytes));
        return OK;
      },
      bufferedAmount() { return 0; },
      async waitForBufferedAmountLow() { return OK; },
      close() {
        opened = false;
        queueMicrotask(() => peer.callbacks()?.onClosed());
        return OK;
      },
    };
  };
  const left = make();
  const right = make();
  left.setPeer(right);
  right.setPeer(left);
  return [left, right];
}

const runtime = await createA11WasmRuntime();
try {
  const [left, right] = channelPair();
  const sender = await runtime.createChannelStream(left, 'native-sender');
  const receiver = await runtime.createChannelStream(right, 'native-receiver');
  const arrivals = [];
  await Promise.all([
    sender.start(async message => { arrivals.push(['left', message]); return OK; },
      async () => OK),
    receiver.start(async message => { arrivals.push(['right', message]); return OK; },
      async () => OK),
  ]);
  await sender.send({node_fragments: [{id: 'native-channel-node',
    data: {data: 'aGVsbG8='}}]});
  for (let attempt = 0; attempt < 100 && arrivals.length === 0; attempt++) {
    await new Promise(resolve => setTimeout(resolve, 10));
  }
  assert.deepEqual(arrivals[0], ['right', {node_fragments: [{
    id: 'native-channel-node', data: {data: 'aGVsbG8='},
  }]}]);
  await sender.abort({code: 1, message: 'test finished'});
  await receiver.abort({code: 1, message: 'test finished'});
  await sender.release();
  await receiver.release();
} finally {
  await runtime.dispose();
}
