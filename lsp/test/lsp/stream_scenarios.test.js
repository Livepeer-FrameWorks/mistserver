import test from 'node:test';
import assert from 'node:assert/strict';
import { loadModule } from './helpers/module_loader.js';

function plain(v) {
  return JSON.parse(JSON.stringify(v));
}

function boot(capabilities) {
  const ctx = loadModule('modules/streams/stream_scenarios.js');
  ctx.mist.data.capabilities = capabilities || { inputs: {} };
  return ctx.streamScenarios;
}

test('streamScenarios detects source type', () => {
  const S = boot();
  assert.equal(S.detectScenario('push://x'), 'push');
  assert.equal(S.detectScenario('srt://x'), 'srt');
  assert.equal(S.detectScenario('rist://x'), 'rist');
  assert.equal(S.detectScenario('rtsp://x'), 'rtsp');
  assert.equal(S.detectScenario('ndi:source'), 'ndi');
  assert.equal(S.detectScenario('/dev/video0'), 'v4l2');
  assert.equal(S.detectScenario('/vod/folder/'), 'folder');
  assert.equal(S.detectScenario('/movie.mp4'), 'file');
  assert.equal(S.detectScenario('https://cdn/x.m3u8'), 'pull');
  assert.equal(S.detectScenario(''), null);
});

test('streamScenarios hasInput/getInput are case-insensitive', () => {
  const S = boot({
    inputs: {
      RTSP_INPUT: { hrn: 'RTSP' },
      NDI_Source: { hrn: 'NDI' }
    }
  });

  assert.equal(S.hasInput('rtsp'), true);
  assert.equal(S.hasInput('ndi'), true);
  assert.equal(S.hasInput('srt'), false);
  assert.equal(S.getInput('ndi').hrn, 'NDI');
  assert.equal(S.getInput('missing'), null);
});

test('streamScenarios getAllFiletypes flattens source_match values', () => {
  const S = boot({
    inputs: {
      FileA: { source_match: '/path/*.mp4' },
      FileB: { source_match: ['/path/*.mkv', '/path/*.ts'] },
      NoMatch: {}
    }
  });

  assert.deepEqual(plain(S.getAllFiletypes()), ['/path/*.mp4', '/path/*.mkv', '/path/*.ts']);
});
