import test from 'node:test';
import assert from 'node:assert/strict';
import { loadModule } from './helpers/module_loader.js';

function plain(v) {
  return JSON.parse(JSON.stringify(v));
}

function wildcardMatch(pattern, value) {
  const re = '^' + String(pattern).replace(/[.+?^${}()|[\]\\]/g, '\\$&').replace(/\*/g, '.*') + '$';
  return new RegExp(re).test(String(value));
}

function boot() {
  const ctx = loadModule('modules/pushes/push_scenarios.js');
  ctx.mistHelpers.inputMatch = wildcardMatch;
  ctx.mist.data.capabilities = {
    connectors: {
      RTMP: { push_urls: ['rtmp://*', '/recordings/*.mp4'] },
      SRT: { push_urls: ['srt://*'] }
    },
    internal_writers: ['s3']
  };
  ctx.mist.data.external_writer_list = [
    ['writer', 'bin', ['ftp']]
  ];
  return ctx.pushScenarios;
}

test('pushScenarios validateTarget and connector mapping follow capability patterns', () => {
  const P = boot();
  P.invalidateCache();

  assert.equal(P.validateTarget('rtmp://example/live'), false);
  assert.equal(P.validateTarget('/recordings/a.mp4'), false);
  assert.equal(P.validateTarget('s3://recordings/a.mp4'), false);
  assert.equal(P.validateTarget('ftp://recordings/a.mp4'), false);
  assert.equal(P.validateTarget('not-a-target'), 'Does not match a valid push target.');

  assert.equal(P.getMatchingConnector('rtmp://example/live'), 'RTMP');
  assert.equal(P.getMatchingConnector('s3://recordings/a.mp4'), 'RTMP');
  assert.equal(P.getMatchingConnector('unknown://x'), null);

  assert.deepEqual(plain(P.getValidFileExtensions()), ['.mp4']);
  assert.deepEqual(plain(P.getValidProtocols()), ['rtmp://', 'srt://']);
});

test('pushScenarios detect/friendly/icon/label helpers map targets', () => {
  const P = boot();
  P.invalidateCache();

  assert.equal(P.detectScenario('rtmp://a.rtmp.youtube.com/live2/key'), 'push_rtmp');
  assert.equal(P.detectScenario('srt://host:9999'), 'push_srt');
  assert.equal(P.detectScenario('/recordings/a.mp4'), 'record_file');
  assert.equal(P.detectScenario('custom://x'), 'push_other');

  assert.equal(P.friendlyTarget('rtmp://live.twitch.tv/app/key'), 'Twitch');
  assert.equal(P.getIcon('rtmp://a.rtmp.youtube.com/live2/key'), 'cast');
  assert.equal(P.getLabel('rtmp://a.rtmp.youtube.com/live2/key'), 'RTMP / RTMPS');
  assert.equal(P.formatCondition('bitrate', 10, '5000'), '$bitrate > 5000');
});
