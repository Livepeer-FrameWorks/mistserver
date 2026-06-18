import test from 'node:test';
import assert from 'node:assert/strict';
import { loadModule } from './helpers/module_loader.js';

function flush() {
  return new Promise(resolve => { setImmediate(resolve); });
}

test('apiClient.parseURL parses url components', () => {
  const ctx = loadModule('modules/core/api_client.js');
  const out = ctx.apiClient.parseURL('https://example.com:4443/path/to?a=1');

  assert.equal(out.full, 'https://example.com:4443/path/to?a=1');
  assert.equal(out.protocol, 'https://');
  assert.equal(out.host, 'example.com');
  assert.equal(out.port, ':4443');
  assert.equal(out.pathname, '/path/to');
  assert.equal(out.search, 'a=1');
});

test('apiClient.send handles CHALL by updating authstring and retrying', async () => {
  let fetchCalls = 0;
  const ctx = loadModule('modules/core/api_client.js', {
    fetch() {
      fetchCalls++;
      if (fetchCalls === 1) {
        return Promise.resolve({
          ok: true,
          json() {
            return Promise.resolve({ authorize: { status: 'CHALL', challenge: 'new-challenge' } });
          }
        });
      }
      return Promise.resolve({
        ok: true,
        json() {
          return Promise.resolve({ authorize: { status: 'OK' }, config: { time: 0 }, log: [] });
        }
      });
    }
  });
  ctx.mist.user = {
    name: 'alice',
    password: 'pw',
    host: 'http://mist.local/api',
    authstring: 'old'
  };

  ctx.apiClient.send(null, { capabilities: true }, { hide: true });
  await flush();
  await flush();
  await flush();

  assert.equal(fetchCalls, 2);
  assert.equal(ctx.mist.user.authstring, 'new-challenge');
  assert.ok(ctx.sessionStorage.getItem('mistLogin'));
});

test('apiClient.send handles OK and updates connection state', async () => {
  let callbackPayload = null;
  const ctx = loadModule('modules/core/api_client.js', {
    fetch() {
      return Promise.resolve({
        ok: true,
        json() {
          return Promise.resolve({
            authorize: { status: 'OK' },
            streams: { alpha: { source: '/a.mp4' } },
            camera_list: [{ id: 'cam1' }],
            config: { time: 1700000000 },
            log: []
          });
        }
      });
    }
  });
  ctx.mist.user = {
    name: 'bob',
    password: 'pw',
    host: 'http://mist.local/api',
    authstring: 'chal'
  };
  ctx.mist.data = { streams: {}, capabilities: { inputs: {} } };

  ctx.apiClient.send(d => { callbackPayload = d; }, {}, { hide: true });

  await flush();
  await flush();

  assert.equal(ctx.mist.user.loggedin, true);
  assert.equal(ctx.UI.elements.connection.status.textValue, 'Connected');
  assert.equal(ctx.UI.elements.connection.user_and_host.textValue, 'bob @ http://mist.local/api');
  assert.deepEqual(ctx.mist.data.streams, { alpha: { source: '/a.mp4' } });
  assert.deepEqual(ctx.mist.data.camera_list, [{ id: 'cam1' }]);
  assert.equal(callbackPayload.authorize.status, 'OK');
});

test('apiClient.send handles request error by navigating to Login', async () => {
  const navCalls = [];
  const ctx = loadModule('modules/core/api_client.js', {
    console: { warn() {}, log() {}, error() {} },
    navto(tab) { navCalls.push(tab); },
    fetch() {
      const e = new Error('aborted');
      e.name = 'AbortError';
      return Promise.reject(e);
    }
  });
  ctx.mist.user = { name: 'eve', password: 'pw', host: 'http://mist.local/api', authstring: 'x' };

  ctx.apiClient.send(null, { config: true }, { hide: true });
  await flush();
  await flush();

  assert.equal(navCalls[0], 'Login');
});

test('apiClient.send supports noLoginOnError and invokes onError callback', async () => {
  let onErrorCalled = 0;
  let errName = '';
  const navCalls = [];
  const ctx = loadModule('modules/core/api_client.js', {
    console: { warn() {}, log() {}, error() {} },
    navto(tab) { navCalls.push(tab); },
    fetch() {
      const e = new Error('network down');
      e.name = 'NetworkError';
      return Promise.reject(e);
    }
  });
  ctx.mist.user = { name: 'eve', password: 'pw', host: 'http://mist.local/api', authstring: 'x' };

  ctx.apiClient.send(null, { config: true }, {
    hide: true,
    noLoginOnError: true,
    onError(err) {
      onErrorCalled++;
      errName = err.name;
    }
  });
  await flush();
  await flush();

  assert.equal(onErrorCalled, 1);
  assert.equal(errName, 'NetworkError');
  assert.equal(navCalls.length, 0);
});

test('apiClient.send normalizes nullable payload fields from server replies', async () => {
  let callbackPayload = null;
  const ctx = loadModule('modules/core/api_client.js', {
    fetch() {
      return Promise.resolve({
        ok: true,
        json() {
          return Promise.resolve({
            authorize: { status: 'OK' },
            active_streams: null,
            camera_list: null,
            capabilities: null,
            config: null,
            totals: null,
            log: []
          });
        }
      });
    }
  });
  ctx.mist.user = {
    name: 'jane',
    password: 'pw',
    host: 'http://mist.local/api',
    authstring: 'chal'
  };
  ctx.mist.data = { streams: {}, capabilities: { inputs: {} }, config: { time: 1700000000 } };

  ctx.apiClient.send(d => { callbackPayload = d; }, { totals: [{ fields: ['clients'], end: -15 }] }, { hide: true });

  await flush();
  await flush();

  assert.equal(Array.isArray(ctx.mist.data.active_streams), true);
  assert.equal(Array.isArray(ctx.mist.data.camera_list), true);
  assert.equal(typeof ctx.mist.data.capabilities, 'object');
  assert.equal(typeof ctx.mist.data.config, 'object');
  assert.equal(typeof ctx.mist.data.totals, 'object');
  assert.equal(callbackPayload.authorize.status, 'OK');
});
