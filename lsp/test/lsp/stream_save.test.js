import test from 'node:test';
import assert from 'node:assert/strict';
import { loadModule } from './helpers/module_loader.js';

function plain(v) {
  return JSON.parse(JSON.stringify(v));
}

function makeInputMatch(pattern, value) {
  const p = String(pattern || '');
  const v = String(value || '');
  if (p.indexOf('*') >= 0) {
    const prefix = p.split('*')[0];
    return v.indexOf(prefix) === 0;
  }
  return p === v;
}

test('streamSave handles push rename, stream keys, and field pruning', () => {
  let sent = null;
  const nav = [];
  const ctx = loadModule('modules/streams/stream_save.js');

  ctx.navto = (tab, param) => { nav.push([tab, param]); };
  ctx.streamHints.findStreamKeys = () => ['oldkey', 'unused'];
  ctx.mistHelpers.inputMatch = makeInputMatch;
  ctx.mist.data = {
    streams: {
      oldstream: { source: 'push://old' }
    },
    streamkeys: {
      oldkey: 'oldstream'
    },
    capabilities: {
      inputs: {
        PushInput: {
          source_match: 'push://*',
          optional: {
            allowed: true,
            streamkeys: true,
            streamkey_only: true,
            always_on: true
          },
          required: {
            required_field: true
          },
          always_match: 'push://*'
        }
      }
    }
  };
  ctx.apiClient.send = (cb, data) => {
    sent = data;
    cb();
  };

  const saveas = {
    name: 'newstream',
    source: 'push://origin-host',
    streamkeys: ['newkey'],
    streamkey_only: true,
    allowed: 1,
    required_field: 'x',
    always_on: true,
    random_field: 'drop-me',
    stop_sessions: true,
    online: true,
    error: 'bad'
  };

  ctx.streamSave(saveas, 'oldstream', 'Preview');

  assert.ok(sent);
  assert.deepEqual(plain(sent.deletestream), ['oldstream']);
  assert.equal(sent.stop_sessions, 'oldstream');
  assert.deepEqual(plain(sent.streamkey_del).sort(), ['oldkey', 'unused']);
  assert.deepEqual(plain(sent.streamkey_add), { newkey: 'newstream' });
  assert.equal(saveas.source, 'push://invalid,host');
  assert.equal(saveas.random_field, undefined);
  assert.equal(saveas.stop_sessions, undefined);
  assert.deepEqual(nav, [['Preview', 'newstream']]);
});

test('streamPresetHelpers detect/apply updates values predictably', () => {
  const ctx = loadModule('modules/streams/stream_save.js');
  const H = ctx.streamPresetHelpers;
  const saveas = { streamkeys: [], streamkey_only: false };

  assert.equal(H.detect(saveas), 'name_only');

  H.randomKey = () => 'RANDOMKEY';
  H.apply(saveas, 'name_or_key');
  assert.equal(saveas.streamkey_only, false);
  assert.deepEqual(plain(saveas.streamkeys), ['RANDOMKEY']);
  assert.equal(H.detect(saveas), 'name_or_key');

  H.apply(saveas, 'key_only');
  assert.equal(saveas.streamkey_only, true);
  assert.deepEqual(plain(saveas.streamkeys), ['RANDOMKEY']);

  H.apply(saveas, 'name_only');
  assert.equal(saveas.streamkey_only, false);
  assert.deepEqual(plain(saveas.streamkeys), []);
});

test('streamSave exits early when save object is invalid', () => {
  let calls = 0;
  const ctx = loadModule('modules/streams/stream_save.js', {
    console: { warn() {}, log() {}, error() {} }
  });
  ctx.apiClient.send = () => { calls++; };

  ctx.streamSave(null, '', 'Streams');
  ctx.streamSave({ source: 'push://x' }, '', 'Streams');

  assert.equal(calls, 0);
});
