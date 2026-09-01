import test from 'node:test';
import assert from 'node:assert/strict';
import { loadModule } from './helpers/module_loader.js';

function plain(v) {
  return JSON.parse(JSON.stringify(v));
}

test('http.api.subscribe merges command and reuses active interval', () => {
  const sentCommands = [];
  const ctx = loadModule('modules/core/sockets.js');

  ctx.UI.interval = {
    list: {},
    set(fn, delay) {
      this.list[42] = { id: 42, fn, delay };
      return 42;
    }
  };
  ctx.apiClient.send = (cb, command) => {
    sentCommands.push(Object.assign({}, command));
    cb({ ok: true, clients: [] });
  };

  let n1 = 0;
  let n2 = 0;
  ctx.sockets.http.api.subscribe(() => { n1++; }, { one: 1 });
  ctx.sockets.http.api.subscribe(() => { n2++; }, { two: 2 });

  assert.equal(ctx.sockets.http.api.interval, 42);
  assert.deepEqual(plain(ctx.sockets.http.api.command), { one: 1, two: 2 });
  assert.equal(sentCommands.length, 2);
  assert.equal(n1 >= 1, true);
  assert.equal(n2 >= 1, true);
});

test('http.api.subscribe resets stale command when prior interval is inactive', () => {
  const sentCommands = [];
  const ctx = loadModule('modules/core/sockets.js');

  ctx.UI.interval = {
    list: {},
    set(fn, delay) {
      this.list[77] = { id: 77, fn, delay };
      return 77;
    }
  };
  ctx.apiClient.send = (cb, command) => {
    sentCommands.push(JSON.parse(JSON.stringify(command)));
    cb({ ok: true, clients: [] });
  };

  ctx.sockets.http.api.subscribe(() => {}, { push_list: 1 });
  assert.deepEqual(plain(ctx.sockets.http.api.command), { push_list: 1 });

  delete ctx.UI.interval.list[77];
  ctx.sockets.http.api.subscribe(() => {}, { clients: [{ streams: ['alpha'] }] });

  assert.deepEqual(
    plain(ctx.sockets.http.api.command),
    { clients: [{ streams: ['alpha'] }] }
  );
  assert.deepEqual(
    sentCommands[sentCommands.length - 1],
    { clients: [{ streams: ['alpha'] }] }
  );
});

test('http.api.get coalesces overlapping in-flight requests', () => {
  let calls = 0;
  let pendingCb = null;
  const ctx = loadModule('modules/core/sockets.js');

  ctx.UI.interval = {
    list: {},
    set(fn, delay) {
      this.list[88] = { id: 88, fn, delay };
      return 88;
    }
  };
  ctx.apiClient.send = (cb) => {
    calls++;
    pendingCb = cb;
  };

  let notifications = 0;
  ctx.sockets.http.api.subscribe(() => { notifications++; }, { one: 1 });

  assert.equal(calls, 1);
  ctx.sockets.http.api.get();
  assert.equal(calls, 1);
  assert.equal(ctx.sockets.http.api.queued, true);

  pendingCb({ ok: true, clients: [] });
  assert.equal(calls, 2);
  assert.equal(notifications, 1);
});

test('http.api.get applies retry backoff and stops scheduling when interval is stale', () => {
  let sendCalls = 0;
  let latestOpts = null;
  const timers = [];
  const ctx = loadModule('modules/core/sockets.js');

  ctx.setTimeout = (fn, delay) => {
    const id = timers.length + 1;
    timers.push({ id, fn, delay });
    return id;
  };
  ctx.clearTimeout = () => {};
  ctx.UI.interval = {
    list: {},
    set(fn, delay) {
      this.list[91] = { id: 91, fn, delay };
      return 91;
    }
  };
  ctx.apiClient.send = (cb, command, opts) => {
    sendCalls++;
    latestOpts = opts;
  };

  ctx.sockets.http.api.subscribe(() => {}, { push_list: 1 });
  assert.equal(sendCalls, 1);

  latestOpts.onError(new Error('down'));
  assert.equal(ctx.sockets.http.api.failures, 1);
  assert.equal(timers.length, 1);
  assert.equal(timers[0].delay, 500);
  assert.equal(ctx.sockets.http.api.retryTimer, 1);

  ctx.sockets.http.api.get();
  assert.equal(sendCalls, 1);

  timers[0].fn();
  assert.equal(sendCalls, 2);
  latestOpts.onError(new Error('down-again'));
  assert.equal(ctx.sockets.http.api.failures, 2);
  assert.equal(timers.length, 2);
  assert.equal(timers[1].delay, 1000);

  timers[1].fn();
  assert.equal(sendCalls, 3);
  delete ctx.UI.interval.list[91];
  latestOpts.onError(new Error('down-stale'));
  assert.equal(timers.length, 2);
});

test('http.api.clear cancels pending retry timer and resets state', () => {
  let latestOpts = null;
  const clearedTimers = [];
  const ctx = loadModule('modules/core/sockets.js');

  ctx.setTimeout = () => 1234;
  ctx.clearTimeout = (id) => { clearedTimers.push(id); };
  ctx.UI.interval = {
    list: {},
    set(fn, delay) {
      this.list[42] = { id: 42, fn, delay };
      return 42;
    }
  };
  ctx.apiClient.send = (cb, command, opts) => {
    latestOpts = opts;
  };

  ctx.sockets.http.api.subscribe(() => {}, { clients: 1 });
  latestOpts.onError(new Error('down'));
  assert.equal(ctx.sockets.http.api.retryTimer, 1234);

  ctx.sockets.http.api.clear();

  assert.deepEqual(clearedTimers, [1234]);
  assert.equal(ctx.sockets.http.api.retryTimer, false);
  assert.equal(ctx.sockets.http.api.failures, 0);
  assert.equal(ctx.sockets.http.api.inFlight, false);
  assert.equal(ctx.sockets.http.api.queued, false);
  assert.equal(ctx.sockets.http.api.interval, false);
  assert.deepEqual(plain(ctx.sockets.http.api.command), {});
  assert.deepEqual(plain(ctx.sockets.http.api.listeners), []);
});

test('ws.info_json.subscribe builds url, receives, and replays cached messages', () => {
  let createdUrl = null;
  const ws = {
    readyState: 1,
    close() {}
  };
  const ctx = loadModule('modules/core/sockets.js');
  ctx.sockets.http_host = 'http://mist.local/';
  ctx.UI.websockets.create = (url) => {
    createdUrl = url;
    return ws;
  };

  const first = [];
  ctx.sockets.ws.info_json.subscribe((msg) => { first.push(msg); }, 'stream/one');

  ws.onmessage({ data: JSON.stringify({ hello: true }) });

  const replayed = [];
  ctx.sockets.ws.info_json.subscribe((msg) => { replayed.push(msg); }, 'stream/one');

  assert.equal(createdUrl, 'ws://mist.local/json_stream%2Fone.js');
  assert.deepEqual(first, [{ hello: true }]);
  assert.deepEqual(replayed, [{ hello: true }]);
});

test('ws.active_streams handles auth and emits stream events', () => {
  const sentFrames = [];
  const ws = {
    readyState: 1,
    send(frame) { sentFrames.push(frame); },
    close() {}
  };
  const ctx = loadModule('modules/core/sockets.js');
  ctx.mist.user = {
    name: 'alice',
    password: 'pw',
    authstring: 'ch0',
    host: 'http://mist.local/api'
  };
  ctx.UI.websockets.create = () => ws;

  const events = [];
  ctx.sockets.ws.active_streams.subscribe((type, data) => {
    events.push([type, data]);
  });

  ws.onmessage({ data: JSON.stringify(['auth', false]) });
  ws.onmessage({ data: JSON.stringify(['auth', { challenge: 'next' }]) });
  ws.onmessage({ data: JSON.stringify(['auth', { status: 'OK' }]) });
  ws.onmessage({ data: JSON.stringify(['stream', { id: 7 }]) });

  assert.equal(ws.authState, 2);
  assert.equal(sentFrames.length, 2);
  assert.equal(sentFrames[0].indexOf('"password":"md5:pwch0"') >= 0, true);
  assert.equal(sentFrames[1].indexOf('"password":"md5:pwnext"') >= 0, true);
  assert.deepEqual(plain(events), [['stream', { id: 7 }]]);

  ws.close();
  assert.equal(ctx.sockets.ws.active_streams.ws, false);
  assert.deepEqual(plain(ctx.sockets.ws.active_streams.messages), []);
});
