import { MistUtil } from './util.js';

export function PlayerState(MistVideo) {
  var listeners = {};
  var state = {
    paused: true,
    playing: false,
    currentTime: 0,
    duration: 0,
    volume: 1,
    muted: false,
    buffered: null,
    seeking: false,
    ended: false,
    loading: true,
    fullscreen: false,
    error: null,
    playbackRate: 1,
    loop: false,
    pip: false,
    tracks: null,
    streamState: null,
  };

  function set(prop, value) {
    if (state[prop] === value) return;
    state[prop] = value;
    var cbs = listeners[prop];
    if (cbs) {
      for (var i = 0; i < cbs.length; i++) {
        cbs[i](value, prop);
      }
    }
  }

  this.on = function(prop, callback) {
    if (!(prop in listeners)) listeners[prop] = [];
    listeners[prop].push(callback);
    callback(state[prop], prop);
    return function unsubscribe() {
      var cbs = listeners[prop];
      if (cbs) {
        var idx = cbs.indexOf(callback);
        if (idx >= 0) cbs.splice(idx, 1);
      }
    };
  };

  this.off = function(prop, callback) {
    var cbs = listeners[prop];
    if (cbs) {
      var idx = cbs.indexOf(callback);
      if (idx >= 0) cbs.splice(idx, 1);
    }
  };

  this.get = function(prop) {
    return state[prop];
  };

  this.set = function(prop, value) {
    set(prop, value);
  };

  this.bind = function(video) {
    var eventMap = {
      play:           function() { set('paused', false); set('playing', true); set('ended', false); },
      pause:          function() { set('paused', true); set('playing', false); },
      playing:        function() { set('loading', false); set('playing', true); },
      timeupdate:     function() { set('currentTime', video.currentTime); },
      durationchange: function() { set('duration', video.duration); },
      volumechange:   function() { set('volume', video.volume); set('muted', video.muted); },
      ratechange:     function() { set('playbackRate', video.playbackRate); },
      seeking:        function() { set('seeking', true); },
      seeked:         function() { set('seeking', false); },
      ended:          function() { set('ended', true); set('playing', false); },
      waiting:        function() { set('loading', true); },
      canplay:        function() { set('loading', false); },
      error:          function() { set('error', video.error); },
      progress:       function() { set('buffered', video.buffered); },
    };
    for (var event in eventMap) {
      MistUtil.event.addListener(video, event, eventMap[event]);
    }

    // PiP events
    MistUtil.event.addListener(video, 'enterpictureinpicture', function() { set('pip', true); });
    MistUtil.event.addListener(video, 'leavepictureinpicture', function() { set('pip', false); });

    // Fullscreen events (vendor-prefixed)
    var fsEvents = ['fullscreenchange','webkitfullscreenchange','mozfullscreenchange','MSFullscreenChange'];
    function onFullscreenChange() {
      var el = document.fullscreenElement || document.webkitFullscreenElement
            || document.mozFullScreenElement || document.msFullscreenElement;
      set('fullscreen', !!el);
    }
    for (var i = 0; i < fsEvents.length; i++) {
      document.addEventListener(fsEvents[i], onFullscreenChange);
    }
    // Fake fullscreen support
    MistUtil.event.addListener(document, 'fakefullscreenchange', function() {
      var container = MistVideo.container;
      set('fullscreen', !!(container && container.hasAttribute('data-fullscreen')));
    });

    // Track list changes
    MistUtil.event.addListener(video, 'metaUpdate_tracks', function(e) {
      if (e.message && e.message.meta && e.message.meta.tracks) {
        set('tracks', MistUtil.tracks.parse(e.message.meta.tracks));
      }
    });
  };

  this.destroy = function() {
    listeners = {};
  };
}
