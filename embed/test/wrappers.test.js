import assert from 'node:assert/strict';
import test from 'node:test';

class FakeElement {
  constructor(tagName) {
    this.tagName = tagName;
    this.attributes = {};
    this.children = [];
    this.listeners = {};
    this.style = {};
    this.currentTime = 12;
    this.duration = Infinity;
    this.seekable = makeRanges([[10,20]]);
    this.buffered = makeRanges([[11,13]]);
    if (tagName == 'hlsjs-video' || tagName == 'dash-video') {
      this.target = new FakeElement('video');
      this.engine = { latestLevelDetails: globalThis.nextLevelDetails };
    }
  }

  setAttribute(name,value) {
    this.attributes[name] = String(value);
    if (name == 'src') { this.src = String(value); }
  }
  removeAttribute(name) { delete this.attributes[name]; }
  appendChild(child) { this.children.push(child); child.parentNode = this; return child; }
  removeChild(child) { this.children.splice(this.children.indexOf(child),1); }
  getElementsByTagName(name) { return this.children.filter((child) => child.tagName == name); }
  addEventListener(name,callback) { (this.listeners[name] ??= []).push(callback); }
  removeEventListener(name,callback) {
    this.listeners[name] = (this.listeners[name] || []).filter((entry) => entry !== callback);
  }
  dispatchEvent(event) {
    for (const callback of this.listeners[event.type] || []) { callback.call(this,event); }
  }
  pause() {}
  destroy() { this.destroyed = true; }
}

function makeRanges(ranges) {
  return {
    length: ranges.length,
    start(index) { return ranges[index][0]; },
    end(index) { return ranges[index][1]; }
  };
}

const appendedToHead = [];
globalThis.window = globalThis;
globalThis.location = { protocol: 'https:' };
globalThis.document = {
  head: { appendChild(element) { appendedToHead.push(element); } },
  createElement(name) { return new FakeElement(name); },
  createEvent() { return { initEvent(type) { this.type = type; } }; }
};
globalThis.customElements = { get() { return true; } };

class FakeHls {
  static Events = { MEDIA_ATTACHED: 'media-attached' };
  constructor() { this.latestLevelDetails = globalThis.nextLevelDetails; }
  attachMedia(media) { this.media = media; }
  on(event,callback) { if (event == FakeHls.Events.MEDIA_ATTACHED) { callback(); } }
  loadSource(url) { this.url = url; }
  destroy() { this.destroyed = true; }
}
globalThis.Hls = FakeHls;

const { mistplayers } = await import('../src/core/registry.js');
const { MistUtil } = await import('../src/core/util.js');
const { containerBlueprints } = await import('../src/ui/blueprints/container.js');
await import('../src/wrappers/hlsjs.js');
await import('../src/wrappers/videojs.js');

function makeMistVideo(type = 'live') {
  return {
    destroyed: false,
    errorListeners: [],
    info: {
      type,
      unixoffset: 1_700_000_000_000,
      meta: { tracks: { video: { firstms: 100_000, lastms: 500_000 } } }
    },
    options: { host: 'https://mist.example', controls: false, muted: false },
    source: { type: 'html5/application/vnd.apple.mpegurl', url: 'https://mist.example/live.m3u8' },
    log() {},
    showError(message) { this.error = message; },
    timers: { start(callback) { callback(); } },
    reload() {},
    urlappend(url) { return url; }
  };
}

test('HLS.js maps live playlist time to Mist packet time and clamps seeks', () => {
  globalThis.nextLevelDetails = {
    edge: 20,
    fragments: [{ programDateTime: 1_700_000_120_000, start: 10 }]
  };
  const player = new mistplayers.hlsjs.player();
  const mistVideo = makeMistVideo();
  mistVideo.player = player;
  let media;
  player.build(mistVideo,(element) => { media = element; });

  assert.equal(player.api.duration,130);
  assert.equal(player.api.currentTime,122);
  assert.equal(player.api.buffered.start(0),121);
  assert.equal(player.api.buffered.end(0),123);
  player.api.currentTime = 125;
  assert.equal(media.currentTime,15);
  player.api.currentTime = 999;
  assert.equal(media.currentTime,20);
});

test('HLS.js fallback keeps its initial API-edge mapping while playlists slide', () => {
  globalThis.nextLevelDetails = { edge: 20, fragments: [] };
  const player = new mistplayers.hlsjs.player();
  const mistVideo = makeMistVideo();
  mistVideo.player = player;
  player.build(mistVideo,() => {});

  assert.equal(player.api.currentTime,492);
  player.hls.latestLevelDetails.edge = 30;
  assert.equal(player.api.currentTime,492);
});

test('Video.js 10 exposes its native media element and stock control tree', () => {
  globalThis.nextLevelDetails = {
    edge: 20,
    fragments: [{ programDateTime: 1_700_000_120_000, start: 10 }]
  };
  const player = new mistplayers.videojs.player();
  const mistVideo = makeMistVideo();
  mistVideo.options.controls = 'stock';
  mistVideo.player = player;
  let media;
  player.build(mistVideo,(element) => { media = element; });

  assert.equal(media.tagName,'video');
  assert.equal(player.mediaElement,media);
  assert.equal(player.displayElement.tagName,'live-video-player');
  assert.equal(player.api.duration,130);
  assert.equal(player.api.currentTime,122);
});

test('Video.js 10 keeps DASH on its native Mist timeline', () => {
  const player = new mistplayers.videojs.player();
  const mistVideo = makeMistVideo();
  mistVideo.source = { type: 'dash/video/mp4', url: 'https://mist.example/live.mpd' };
  mistVideo.player = player;
  let media;
  player.build(mistVideo,(element) => { media = element; });

  assert.equal(mistplayers.videojs.isMimeSupported('dash/video/mp4'),true);
  assert.equal(mistplayers.videojs.getScore('cpu_viewer',mistVideo.source),9);
  assert.equal(player.source.tagName,'dash-video');
  assert.equal(media.tagName,'video');
  assert.equal(player.api.currentTime,12);
  player.api.currentTime = 18;
  assert.equal(player.source.currentTime,18);
});

test('video container inserts a wrapper display tree while retaining the native media API', () => {
  const nativeVideo = new FakeElement('video');
  const displayTree = new FakeElement('live-video-player');
  const result = containerBlueprints.video.call({
    options: { autoplay: false, rotate: false },
    video: nativeVideo,
    player: { displayElement: displayTree },
    timers: { start() {}, stop() {} },
    container: new FakeElement('div')
  });

  assert.equal(result,displayTree);
});

test('Video.js 10 loads as an ES module when its custom elements are absent', () => {
  const originalGet = customElements.get;
  customElements.get = () => false;
  MistUtil.scripts.list = {};
  appendedToHead.length = 0;
  const player = new mistplayers.videojs.player();
  const mistVideo = makeMistVideo();
  mistVideo.player = player;
  player.build(mistVideo,() => {});

  assert.equal(appendedToHead.length,1);
  assert.equal(appendedToHead[0].attributes.type,'module');
  assert.equal(appendedToHead[0].attributes.src,'https://mist.example/videojs.js');
  customElements.get = originalGet;
});
