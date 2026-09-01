import test from 'node:test';
import assert from 'node:assert/strict';
import { loadModule } from './helpers/module_loader.js';

const ctx = loadModule('modules/streams/composer_config.js');
const composer = ctx;

test('composer config is stored as standalone input options', () => {
  const stream = {
    source: 'compose',
    processes: {
      Composer0: {
        process: 'Composer',
        sources: [{stream: 'cam-a', x: 0, y: 0, w: 960, h: 1080}],
        resolution: '1920x1080',
        layout: 'focussed'
      },
      AV0: {process: 'AV', codec: 'H264', 'x-LSP-kind': 'video'}
    }
  };

  const extracted = composer.extractComposerConfig(stream);
  assert.equal(extracted.sources[0].stream, 'cam-a');
  assert.equal(extracted.layout, 'focussed');

  composer.applyComposerConfig(stream, extracted);
  assert.deepEqual(stream.sources, extracted.sources);
  assert.equal(stream.resolution, '1920x1080');
  assert.equal(stream.processes.Composer0, undefined);
  assert.equal(stream.processes.AV0.process, 'AV');
});

test('composer layout is inferred from saved source positions', () => {
  const positions = composer.calculateComposerPositions(4, '1920x1080', 'equal');
  const sources = positions.map(function(position, index) {
    return Object.assign({stream: 'cam-' + index}, position);
  });
  assert.equal(composer.inferComposerLayout(sources, '1920x1080'), 'equal');

  sources[0].w -= 10;
  assert.equal(composer.inferComposerLayout(sources, '1920x1080'), 'none');
});

test('layered composer layout overlays every source at full resolution', () => {
  const positions = JSON.parse(JSON.stringify(
    composer.calculateComposerPositions(3, '1280x720', 'lasagna')
  ));
  assert.deepEqual(positions, [
    {x: 0, y: 0, w: 1280, h: 720},
    {x: 0, y: 0, w: 1280, h: 720},
    {x: 0, y: 0, w: 1280, h: 720}
  ]);
});

test('legacy Composer entries are removed from process arrays', () => {
  const processes = [
    {process: 'Composer', sources: []},
    {process: 'AV', codec: 'H264'}
  ];
  const filtered = composer.removeComposerProcesses(processes);
  assert.deepEqual(filtered, [{process: 'AV', codec: 'H264'}]);
});

test('composer pipeline adds AV decoders to inputs and an encoder to output', () => {
  const output = {source: 'compose', processes: {}};
  const compose = {
    sources: [{stream: 'camera'}],
    _prepareInputs: true,
    _outputCodec: 'H264',
    _rawSources: []
  };
  const configured = {
    camera: {source: 'rtsp://camera/live', processes: {}}
  };

  const result = composer.prepareComposerPipeline(output, compose, configured);
  assert.equal(output.processes.AV_composer_output.codec, 'H264');
  assert.equal(result.relatedStreams.camera.processes.AV_composer_decode_video.codec, 'UYVY');
  assert.equal(result.relatedStreams.camera.processes.AV_composer_decode_video.track_select, 'video=all&audio=none');
  assert.deepEqual(configured.camera.processes, {});
});

test('composer pipeline skips decoders for raw sources and PNG images', () => {
  const output = {source: 'compose', processes: {}};
  const compose = {
    sources: [{stream: 'sdi'}, {stream: '/overlay.png'}],
    _prepareInputs: true,
    _outputCodec: 'raw',
    _rawSources: ['sdi']
  };
  const result = composer.prepareComposerPipeline(output, compose, {
    sdi: {source: 'sdi:0', processes: {}}
  });
  assert.equal(Object.keys(result.relatedStreams).length, 0);
  assert.equal(Object.keys(output.processes).length, 0);
});

test('composer pipeline removes its managed encoder when raw output is selected', () => {
  const output = {
    source: 'compose',
    processes: {
      AV_composer_output: {
        process: 'AV',
        'x-LSP-name': 'Composer output encoder',
        'x-LSP-kind': 'video',
        codec: 'H264'
      }
    }
  };
  composer.prepareComposerPipeline(output, {
    sources: [],
    _prepareInputs: false,
    _outputCodec: 'raw',
    _outputCodecChanged: true
  }, {});
  assert.equal(Object.keys(output.processes).length, 0);
});
