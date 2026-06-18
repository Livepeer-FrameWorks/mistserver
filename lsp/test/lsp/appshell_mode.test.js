import test from 'node:test';
import assert from 'node:assert/strict';
import { loadModule } from './helpers/module_loader.js';

test('AppShell.normalizeMode and getMode return guided defaults', () => {
  const ctx = loadModule('modules/core/appshell.js');
  const A = ctx.AppShell;

  assert.equal(A.normalizeMode('simple'), 'guided');
  assert.equal(A.normalizeMode('advanced'), 'advanced');
  assert.equal(A.normalizeMode('bogus'), 'guided');

  ctx.document.body.setAttribute('data-mode', 'advanced');
  assert.equal(A.getMode(), 'advanced');
  ctx.document.body.setAttribute('data-mode', 'simple');
  assert.equal(A.getMode(), 'guided');
});

test('AppShell.getModeStorageKey resolves user/host from mist, hash, then session storage', () => {
  const ctx = loadModule('modules/core/appshell.js');
  const A = ctx.AppShell;

  ctx.mist.user.name = 'alice';
  ctx.mist.user.host = 'http://mist.local/api';
  assert.equal(A.getModeStorageKey(), 'mist-mode:alice%40http%3A%2F%2Fmist.local%2Fapi');

  ctx.mist.user.name = '';
  ctx.mist.user.host = '';
  ctx.location.hash = '#bob&http://fromhash/api@Overview';
  assert.equal(A.getModeStorageKey(), 'mist-mode:bob%40http%3A%2F%2Ffromhash%2Fapi');

  ctx.location.hash = '';
  ctx.sessionStorage.setItem('mistLogin', JSON.stringify({ name: 'eve', host: 'http://fromsession/api' }));
  assert.equal(A.getModeStorageKey(), 'mist-mode:eve%40http%3A%2F%2Ffromsession%2Fapi');
});

test('AppShell.modeAffectsTab checks registered mode-aware tabs', () => {
  const ctx = loadModule('modules/core/appshell.js', {
    modeAwareTabs: { Streams: true }
  });
  const A = ctx.AppShell;

  assert.equal(A.modeAffectsTab('Streams'), true);
  assert.equal(A.modeAffectsTab('Overview'), false);
});

test('AppShell.getModeHint reflects guided vs advanced mode', () => {
  const ctx = loadModule('modules/core/appshell.js');
  const A = ctx.AppShell;

  ctx.document.body.setAttribute('data-mode', 'guided');
  assert.equal(A.getModeHint(), 'Guided mode is active. Switch to Advanced for full controls.');

  ctx.document.body.setAttribute('data-mode', 'advanced');
  assert.equal(A.getModeHint(), 'Advanced mode is active. Switch to Guided for a simplified UI.');
});

test('ModeDispatch.register routes to guided/advanced handlers', () => {
  let guidedCalls = 0;
  let advancedCalls = 0;

  const ctx = loadModule('modules/core/mode_dispatch.js', {
    AppShell: {
      getMode() { return 'guided'; }
    },
    registerTab(name, fn) {
      ctx.UI.tabHandlers[name] = fn;
    }
  });

  ctx.register('Streams', {
    guided() { guidedCalls++; },
    advanced() { advancedCalls++; }
  });

  ctx.UI.tabHandlers.Streams();
  assert.equal(guidedCalls, 1);
  assert.equal(advancedCalls, 0);

  ctx.AppShell.getMode = () => 'advanced';
  ctx.UI.tabHandlers.Streams();
  assert.equal(guidedCalls, 1);
  assert.equal(advancedCalls, 1);
  assert.equal(ctx.modeAwareTabs.Streams, true);
});
