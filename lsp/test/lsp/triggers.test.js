import test from 'node:test';
import assert from 'node:assert/strict';
import { loadModule } from './helpers/module_loader.js';

function plain(value) {
  return JSON.parse(JSON.stringify(value));
}

function loadTriggers() {
  return loadModule('modules/pages/triggers.js', {
    registerDynamicProvider() {},
    registerTab() {},
    getTabHandler() { return function() {}; }
  });
}

test('trigger payload persists onfail only for blocking handlers', () => {
  const ctx = loadTriggers();
  const cap = { stream_specific: true };
  const base = {
    url: 'http://handler.test/',
    streams: ['camera', '', 'camera', 'backup'],
    params: 'threshold=2',
    default: 'configured',
    onfail: 'offline'
  };

  const blocking = ctx.buildTriggerPayload(Object.assign({}, base, { blocking: true }), cap);
  assert.deepEqual(plain(blocking), {
    handler: 'http://handler.test/',
    sync: true,
    streams: ['camera', 'backup'],
    params: 'threshold=2',
    default: 'configured',
    onfail: 'offline'
  });

  const asynchronous = ctx.buildTriggerPayload(Object.assign({}, base, { blocking: false }), cap);
  assert.equal(asynchronous.sync, false);
  assert.equal(Object.hasOwn(asynchronous, 'onfail'), false);
});

test('trigger form offers only value-free failure actions', () => {
  const ctx = loadTriggers();
  const form = ctx.buildConfigForm({}, {
    response: 'when-blocking',
    stream_specific: false,
    actions: ['value', 'deny', 'keep']
  });
  const failureField = form.advanced.find((field) => field.label === 'On handler failure');
  assert.ok(failureField);
  assert.deepEqual(plain(failureField.select), [
    ['', 'Legacy default response'],
    ['deny', 'deny'],
    ['keep', 'keep']
  ]);
  assert.equal(failureField.pointer.index, 'onfail');
});
