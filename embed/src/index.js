// @mistserver/player-core — ESM entry point

// Core
export { MistUtil } from './core/util.js';
export { MistPlayer, MistVideo, mistPlay } from './core/player.js';
export { mistplayers, registerWrapper } from './core/registry.js';
export { ControlChannel, DataChannel2WebSocket, ControlChannelAPI } from './core/shared.js';
export { PlayerState } from './core/state.js';
export { MistThemes, resolveTheme } from './core/themes.js';
export { locales } from './core/locales.js';

// UI
export { MistSkins } from './ui/default-skin.js';
export { MistSkin } from './ui/skin-loader.js';
export { MistUI } from './ui/controls.js';

// Wrappers — side-effect imports (register themselves)
import './wrappers/html5.js';
import './wrappers/hlsjs.js';
import './wrappers/dashjs.js';
import './wrappers/videojs.js';
import './wrappers/webrtc.js';
import './wrappers/wheprtc.js';
import './wrappers/mews.js';
import './wrappers/rawwscanvas.js';
import './wrappers/rawws.js';
import './wrappers/flv.js';

// Public API: createPlayer()
import { MistVideo } from './core/player.js';
import { MistUtil } from './core/util.js';
import { resolveTheme as _resolveTheme } from './core/themes.js';

export function createPlayer(options) {
  if (!options || !options.target) {
    throw new Error('createPlayer requires a target element in options');
  }
  if (!options.stream) {
    throw new Error('createPlayer requires a stream name in options');
  }

  var streamName = options.stream;
  delete options.stream;

  var mv = new MistVideo(streamName, options);

  return {
    // ── Queries ──
    get video() { return mv.video; },
    get info() { return mv.info; },
    get source() { return mv.source; },
    get playerName() { return mv.playerName; },
    get logs() { return mv.logs; },
    get options() { return mv.options; },
    get currentTime() { return mv.api ? mv.api.currentTime : 0; },
    get duration() { return mv.api ? mv.api.duration : 0; },
    get volume() { return mv.api ? mv.api.volume : 1; },
    get muted() { return mv.api ? mv.api.muted : false; },
    get paused() { return mv.api ? mv.api.paused : true; },
    get playbackRate() { return mv.api ? mv.api.playbackRate : 1; },
    get loop() { return mv.api ? mv.api.loop : false; },
    get buffered() { return mv.api ? mv.api.buffered : null; },
    get fullscreen() { return mv.fullscreen ? mv.fullscreen.active : false; },
    get pip() { return mv.pip ? mv.pip.active : false; },
    get tracks() {
      if (mv.info && mv.info.meta && mv.info.meta.tracks) {
        return MistUtil.tracks.parse(mv.info.meta.tracks);
      }
      return null;
    },
    get size() { return mv.size || null; },
    get capabilities() { return mv.getCapabilities ? mv.getCapabilities() : {}; },
    get quality() { return (mv.monitor && mv.monitor.vars) ? mv.monitor.vars.score : null; },
    get streamState() { return mv.state || null; },

    // ── Mutations ──
    play: function() { return mv.api ? mv.api.play() : Promise.resolve(); },
    pause: function() { if (mv.api) mv.api.pause(); },
    set currentTime(val) { if (mv.api) mv.api.currentTime = val; },
    set volume(val) { if (mv.api) mv.api.volume = val; },
    set muted(val) { if (mv.api) mv.api.muted = val; },
    set playbackRate(val) { if (mv.api) mv.api.playbackRate = val; },
    set loop(val) { if (mv.api) mv.api.loop = val; },
    set fullscreen(val) {
      if (!mv.fullscreen) return;
      if (val) mv.fullscreen.request();
      else mv.fullscreen.exit();
    },
    set pip(val) {
      if (!mv.pip) return;
      if (val) mv.pip.request();
      else mv.pip.exit();
    },
    setTrack: function(type, trackid) {
      return mv.api ? mv.api.setTrack(type, trackid) : false;
    },
    getStats: function() {
      return mv.api ? mv.api.getStats() : null;
    },
    destroy: function() { mv.unload('destroy() called'); },
    reload: function() { mv.reload('reload() called'); },
    nextCombo: function() { mv.nextCombo(); },
    translate: function(key, fallback) { return mv.translate(key, fallback); },
    setTheme: function(themeOrTokens, mode) {
      var tokens = _resolveTheme(themeOrTokens, mode);
      if (mv.container && tokens) {
        for (var name in tokens) {
          var prop = name.indexOf('--') === 0 ? name : '--mist-' + name;
          mv.container.style.setProperty(prop, tokens[name]);
        }
      }
    },

    // ── Subscriptions ──
    state: {
      on: function(prop, cb) { return mv.playerState.on(prop, cb); },
      off: function(prop, cb) { mv.playerState.off(prop, cb); },
      get: function(prop) { return mv.playerState.get(prop); },
    },
    on: function(event, callback) {
      MistUtil.event.addListener(options.target, event, callback);
    },
    off: function(event, callback) {
      options.target.removeEventListener(event, callback);
    },

    get raw() { return mv; }
  };
}
