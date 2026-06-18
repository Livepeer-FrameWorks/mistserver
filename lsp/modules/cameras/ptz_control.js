/**
 * PTZ Control sub-tab - side-by-side layout with live snapshot and controls.
 * D-pad, zoom, focus, iris, presets. Mousedown sends continuous move; mouseup sends stop.
 */
import { el } from '../core/dom_helpers.js';
import { Component, DEVICES_SOURCE_PRIORITY } from './component.js';
import { api } from './api.js';
import { dom } from './dom.js';
import { navto } from '../core/navigation.js';
import { sockets } from '../core/sockets.js';
import { mistPlay } from '@player';
import { getActiveThemeId } from '../core/themes.js';
import { stream } from '../streams/stream_utils.js';

function PTZControl() {}
  PTZControl.prototype = new Component();

  PTZControl.prototype.build = function() {
    const camId = this.params.other;
    if (!camId) { navto('Devices'); return; }
    if (!this.requireCameras('PTZ Control', camId)) return;

    const cam = this.findCamera(camId);
    if (!cam) {
      const errP = el('p', {class: 'red'});
      errP.textContent = 'Camera not found: ' + camId;
      this.$el.appendChild(errP);
      this.$el.appendChild(dom.backButton('Back to Devices', 'Devices'));
      return;
    }

    const container = this.$el;
    const pageHeader = this.params.$pageHeader;

    if (pageHeader) {
      pageHeader.innerHTML = '';
      pageHeader.appendChild(dom.cameraSubtabHeader(cam, camId, 'PTZ Control'));
    }

    if (!this._ptzSpeed) this._ptzSpeed = 50;

    var self = this;
    var layout = el('div', {class: 'ptz-layout ptz-layout--slab ptz-layout--console slab-shell'});

    var preview = el('div', {class: 'ptz-preview ptz-preview-slab ptz-console-preview'});
    var previewFrame = el('div', {class: 'ptz-preview-frame'});
    preview.appendChild(previewFrame);

    var hud = el('div', {class: 'ptz-hud'});
    var isOnline = cam.status === 'online' || cam.status === 'connected';
    hud.appendChild(el('span', {class: 'ptz-hud-pill ' + (isOnline ? 'green' : 'red')}, isOnline ? 'Live' : (cam.status || 'Unknown')));
    hud.appendChild(el('span', {class: 'ptz-hud-pill'}, cam.id || cam.name || camId));
    if (cam.model) hud.appendChild(el('span', {class: 'ptz-hud-pill'}, cam.model));
    if (cam.host) hud.appendChild(el('span', {class: 'ptz-hud-pill mono'}, cam.host));
    preview.appendChild(hud);
    layout.appendChild(preview);

    const panel = el('div', {class: 'ptz-panel ptz-panel-slab ptz-console-panel'});
    const intro = el('div', {class: 'ptz-console-intro'});
    intro.textContent = 'Press and hold movement controls for continuous motion. Release to stop.';
    panel.appendChild(intro);

    this.renderSpeed(panel);
    this.renderDPad(camId, panel);
    this.renderZoom(camId, panel);
    this.renderFocusIris(cam, camId, panel);
    this.renderHome(camId, panel);
    this.renderPresets(camId, panel);

    layout.appendChild(panel);
    container.appendChild(layout);

    var cameraStreams = this.getStreamNames(cam);
    var defaultIdx = cam.defaultStream != null && cam.defaultStream >= 0 ? cam.defaultStream : 0;
    if (defaultIdx >= cameraStreams.length) defaultIdx = 0;

    if (cameraStreams.length) {
      var startPlayer = function(host) {
        window.ptzMV = {};
        mistPlay(cameraStreams[defaultIdx].name, {
          target: previewFrame,
          host: host,
          theme: getActiveThemeId(),
          controls: true,
          loop: true,
          MistVideoObject: window.ptzMV,
          forcePriority: {source: DEVICES_SOURCE_PRIORITY},
          callback: function(MistVideo) {
            var old = preview.querySelectorAll('.ptz-protocol-bar, .ptz-audio-warning');
            for (var r = 0; r < old.length; r++) old[r].parentNode.removeChild(old[r]);

            var sName = cameraStreams[defaultIdx].name;
            var protoBar = dom.protocolBar(MistVideo, function(codecs) {
              var caps = (mist.data.capabilities && mist.data.capabilities.processes) || {};
              var hasTranscoder = !!(caps.AV || caps.FFMPEG);
              preview.appendChild(dom.audioWarningBanner(codecs,
                hasTranscoder ? function(done) {
                  self.addAudioTranscode(sName, 'opus', function(ok) {
                    done(ok);
                    if (ok) navto('PTZ Control', camId);
                  });
                } : null
              ));
            });
            preview.appendChild(protoBar);
          }
        });
      };
      if (sockets.http_host) {
        startPlayer(sockets.http_host);
      } else {
        var finder = stream.findMist(function(url) {
          startPlayer(url);
        });
        previewFrame.appendChild(finder);
      }
    } else if (cam.snapshotUri) {
      var img = el('img', {class: 'ptz-snapshot'});
      img.setAttribute('src', cam.snapshotUri);
      img.setAttribute('alt', 'Live snapshot');
      img.addEventListener('error', function() {
        var fallback = el('em', {class: 'ptz-no-preview'});
        fallback.textContent = 'Snapshot unavailable';
        this.replaceWith(fallback);
      });
      previewFrame.appendChild(img);
      var uri = cam.snapshotUri;
      UI.interval.set(function() {
        if (img && img.parentNode) {
          img.setAttribute('src', uri + (uri.indexOf('?') >= 0 ? '&' : '?') + '_t=' + Date.now());
        }
      }, 2e3);
    } else {
      var noPreview = el('em', {class: 'ptz-no-preview'});
      noPreview.textContent = 'No live preview available.';
      previewFrame.appendChild(noPreview);
    }
  };

  PTZControl.prototype.sendPTZ = function(camId, command, args) {
    if (this._ptzInFlight) {
      this._ptzQueued = {camId: camId, command: command, args: args};
      return;
    }
    this._ptzInFlight = true;
    var self = this;
    api.sendPTZ(camId, command, args).then(function(d) {
      if (d && d.camera_query && d.camera_query.success === false) {
        console.warn('PTZ command rejected:', d.camera_query.error || 'unknown error');
      }
    })['catch'](function(err) {
      console.warn('PTZ request failed:', err.message);
    }).then(function() {
      self._ptzInFlight = false;
      if (self._ptzQueued) {
        var q = self._ptzQueued;
        self._ptzQueued = null;
        self.sendPTZ(q.camId, q.command, q.args);
      }
    });
  };

  PTZControl.prototype.renderSpeed = function(panel) {
    const self = this;
    const section = el('section', {class: 'ptz-section ptz-section-speed'});
    const lbl = el('label');
    lbl.textContent = 'Motion speed';
    section.appendChild(lbl);

    const speedWrap = el('div', {class: 'ptz-speed-wrap'});
    const range = el('input', {type: 'range', min: '10', max: '100', step: '10'});
    range.value = String(self._ptzSpeed || 50);
    const value = el('span', {class: 'ptz-speed-value'}, range.value + '%');
    range.addEventListener('input', function() {
      self._ptzSpeed = Number(this.value) || 50;
      value.textContent = self._ptzSpeed + '%';
    });
    speedWrap.appendChild(range);
    speedWrap.appendChild(value);
    section.appendChild(speedWrap);

    panel.appendChild(section);
  };

  PTZControl.prototype.renderDPad = function(camId, panel) {
    const self = this;
    const section = el('section', {class: 'ptz-section ptz-section-direction'});
    const lbl = el('label');
    lbl.textContent = 'Direction';
    section.appendChild(lbl);
    const grid = el('div', {class: 'ptz-grid'});

    const dirs = [
      {label: '\u2196', pan: -1, tilt: 1},  {label: '\u2191', pan: 0, tilt: 1},   {label: '\u2197', pan: 1, tilt: 1},
      {label: '\u2190', pan: -1, tilt: 0},   {label: '\u25A0', stop: true},          {label: '\u2192', pan: 1, tilt: 0},
      {label: '\u2199', pan: -1, tilt: -1}, {label: '\u2193', pan: 0, tilt: -1},  {label: '\u2198', pan: 1, tilt: -1}
    ];

    for (let i = 0; i < dirs.length; i++) {
      (function(dir) {
        const btn = el('button');
        btn.textContent = dir.label;
        if (dir.stop) {
          btn.classList.add('stop');
          btn.addEventListener('click', function() { self.sendPTZ(camId, 'stop', {}); });
        } else {
          btn.addEventListener('mousedown', function(e) {
            e.preventDefault();
            const speed = self._ptzSpeed || 50;
            self.sendPTZ(camId, 'pan_tilt', {pan: dir.pan * speed, tilt: dir.tilt * speed});
          });
          btn.addEventListener('touchstart', function(e) {
            e.preventDefault();
            const speed = self._ptzSpeed || 50;
            self.sendPTZ(camId, 'pan_tilt', {pan: dir.pan * speed, tilt: dir.tilt * speed});
          });
          btn.addEventListener('mouseup', function(e) {
            e.preventDefault();
            self.sendPTZ(camId, 'stop', {});
          });
          btn.addEventListener('touchend', function(e) {
            e.preventDefault();
            self.sendPTZ(camId, 'stop', {});
          });
          btn.addEventListener('mouseleave', function(e) {
            e.preventDefault();
            self.sendPTZ(camId, 'stop', {});
          });
        }
        grid.appendChild(btn);
      })(dirs[i]);
    }
    section.appendChild(grid);
    panel.appendChild(section);
  };

  PTZControl.prototype.renderZoom = function(camId, panel) {
    const self = this;
    const section = el('section', {class: 'ptz-section ptz-section-zoom'});
    const lbl = el('label');
    lbl.textContent = 'Zoom';
    section.appendChild(lbl);

    const outBtn = el('button');
    outBtn.textContent = '\u2212';
    const inBtn = el('button');
    inBtn.textContent = '+';

    outBtn.addEventListener('mousedown', function(e) { e.preventDefault(); self.sendPTZ(camId, 'zoom', {speed: -(self._ptzSpeed || 50)}); });
    outBtn.addEventListener('touchstart', function(e) { e.preventDefault(); self.sendPTZ(camId, 'zoom', {speed: -(self._ptzSpeed || 50)}); });
    outBtn.addEventListener('mouseup', function(e) { e.preventDefault(); self.sendPTZ(camId, 'stop', {}); });
    outBtn.addEventListener('touchend', function(e) { e.preventDefault(); self.sendPTZ(camId, 'stop', {}); });
    outBtn.addEventListener('mouseleave', function(e) { e.preventDefault(); self.sendPTZ(camId, 'stop', {}); });

    inBtn.addEventListener('mousedown', function(e) { e.preventDefault(); self.sendPTZ(camId, 'zoom', {speed: self._ptzSpeed || 50}); });
    inBtn.addEventListener('touchstart', function(e) { e.preventDefault(); self.sendPTZ(camId, 'zoom', {speed: self._ptzSpeed || 50}); });
    inBtn.addEventListener('mouseup', function(e) { e.preventDefault(); self.sendPTZ(camId, 'stop', {}); });
    inBtn.addEventListener('touchend', function(e) { e.preventDefault(); self.sendPTZ(camId, 'stop', {}); });
    inBtn.addEventListener('mouseleave', function(e) { e.preventDefault(); self.sendPTZ(camId, 'stop', {}); });

    const wrapper = el('div', {class: 'ptz-pair'});
    wrapper.appendChild(outBtn);
    wrapper.appendChild(inBtn);
    section.appendChild(wrapper);
    panel.appendChild(section);
  };

  PTZControl.prototype.renderFocusIris = function(cam, camId, panel) {
    const self = this;
    const feats = cam.ptzFeatures || [];
    const featsLower = feats.map(function(f) { return f.toLowerCase(); });

    if (featsLower.indexOf('focus') >= 0) {
      const section = el('section', {class: 'ptz-section ptz-section-focus'});
      const lbl = el('label');
      lbl.textContent = 'Focus';
      section.appendChild(lbl);
      const nearBtn = el('button');
      nearBtn.textContent = 'Near';
      const farBtn = el('button');
      farBtn.textContent = 'Far';

      nearBtn.addEventListener('mousedown', function(e) { e.preventDefault(); self.sendPTZ(camId, 'focus', {speed: -(self._ptzSpeed || 50)}); });
      nearBtn.addEventListener('touchstart', function(e) { e.preventDefault(); self.sendPTZ(camId, 'focus', {speed: -(self._ptzSpeed || 50)}); });
      nearBtn.addEventListener('mouseup', function(e) { e.preventDefault(); self.sendPTZ(camId, 'stop', {}); });
      nearBtn.addEventListener('touchend', function(e) { e.preventDefault(); self.sendPTZ(camId, 'stop', {}); });
      nearBtn.addEventListener('mouseleave', function(e) { e.preventDefault(); self.sendPTZ(camId, 'stop', {}); });

      farBtn.addEventListener('mousedown', function(e) { e.preventDefault(); self.sendPTZ(camId, 'focus', {speed: self._ptzSpeed || 50}); });
      farBtn.addEventListener('touchstart', function(e) { e.preventDefault(); self.sendPTZ(camId, 'focus', {speed: self._ptzSpeed || 50}); });
      farBtn.addEventListener('mouseup', function(e) { e.preventDefault(); self.sendPTZ(camId, 'stop', {}); });
      farBtn.addEventListener('touchend', function(e) { e.preventDefault(); self.sendPTZ(camId, 'stop', {}); });
      farBtn.addEventListener('mouseleave', function(e) { e.preventDefault(); self.sendPTZ(camId, 'stop', {}); });

      const focusWrapper = el('div', {class: 'ptz-pair'});
      focusWrapper.appendChild(nearBtn);
      focusWrapper.appendChild(farBtn);
      section.appendChild(focusWrapper);
      panel.appendChild(section);
    }

    if (featsLower.indexOf('iris') >= 0) {
      const irisSection = el('section', {class: 'ptz-section ptz-section-iris'});
      const irisLbl = el('label');
      irisLbl.textContent = 'Iris';
      irisSection.appendChild(irisLbl);
      const closeBtn = el('button');
      closeBtn.textContent = 'Close';
      const openBtn = el('button');
      openBtn.textContent = 'Open';

      closeBtn.addEventListener('mousedown', function(e) { e.preventDefault(); self.sendPTZ(camId, 'iris', {speed: -(self._ptzSpeed || 50)}); });
      closeBtn.addEventListener('touchstart', function(e) { e.preventDefault(); self.sendPTZ(camId, 'iris', {speed: -(self._ptzSpeed || 50)}); });
      closeBtn.addEventListener('mouseup', function(e) { e.preventDefault(); self.sendPTZ(camId, 'stop', {}); });
      closeBtn.addEventListener('touchend', function(e) { e.preventDefault(); self.sendPTZ(camId, 'stop', {}); });
      closeBtn.addEventListener('mouseleave', function(e) { e.preventDefault(); self.sendPTZ(camId, 'stop', {}); });

      openBtn.addEventListener('mousedown', function(e) { e.preventDefault(); self.sendPTZ(camId, 'iris', {speed: self._ptzSpeed || 50}); });
      openBtn.addEventListener('touchstart', function(e) { e.preventDefault(); self.sendPTZ(camId, 'iris', {speed: self._ptzSpeed || 50}); });
      openBtn.addEventListener('mouseup', function(e) { e.preventDefault(); self.sendPTZ(camId, 'stop', {}); });
      openBtn.addEventListener('touchend', function(e) { e.preventDefault(); self.sendPTZ(camId, 'stop', {}); });
      openBtn.addEventListener('mouseleave', function(e) { e.preventDefault(); self.sendPTZ(camId, 'stop', {}); });

      const irisWrapper = el('div', {class: 'ptz-pair'});
      irisWrapper.appendChild(closeBtn);
      irisWrapper.appendChild(openBtn);
      irisSection.appendChild(irisWrapper);
      panel.appendChild(irisSection);
    }
  };

  PTZControl.prototype.renderHome = function(camId, panel) {
    const self = this;
    const section = el('section', {class: 'ptz-section ptz-section-home'});
    const homeBtn = el('button');
    homeBtn.setAttribute('data-icon', 'home');
    homeBtn.textContent = 'Home Position';
    homeBtn.addEventListener('click', function() {
      self.sendPTZ(camId, 'home', {});
    });
    section.appendChild(homeBtn);
    panel.appendChild(section);
  };

  PTZControl.prototype.renderPresets = function(camId, panel) {
    const self = this;
    const section = el('section', {class: 'ptz-section ptz-section-presets'});
    const lbl = el('label');
    lbl.textContent = 'Presets';
    section.appendChild(lbl);
    const note = el('div', {class: 'camera-note'});
    note.textContent = 'Click to recall. Long press to save.';
    section.appendChild(note);

    const btns = el('div', {class: 'ptz-presets'});
    for (let i = 1; i <= 9; i++) {
      (function(num) {
        let pressTimer = null;
        const btn = el('button');
        btn.textContent = num;
        btn.addEventListener('mousedown', function(e) {
          e.preventDefault();
          pressTimer = setTimeout(function() {
            pressTimer = 'long';
            self.sendPTZ(camId, 'preset', {index: num, store: true});
            btn.classList.add('green');
            setTimeout(function() { btn.classList.remove('green'); }, 500);
          }, 800);
        });
        btn.addEventListener('touchstart', function(e) {
          e.preventDefault();
          pressTimer = setTimeout(function() {
            pressTimer = 'long';
            self.sendPTZ(camId, 'preset', {index: num, store: true});
            btn.classList.add('green');
            setTimeout(function() { btn.classList.remove('green'); }, 500);
          }, 800);
        });
        btn.addEventListener('mouseup', function(e) {
          e.preventDefault();
          if (pressTimer && pressTimer !== 'long') {
            clearTimeout(pressTimer);
            self.sendPTZ(camId, 'preset', {index: num, store: false});
          }
          pressTimer = null;
        });
        btn.addEventListener('touchend', function(e) {
          e.preventDefault();
          if (pressTimer && pressTimer !== 'long') {
            clearTimeout(pressTimer);
            self.sendPTZ(camId, 'preset', {index: num, store: false});
          }
          pressTimer = null;
        });
        btn.addEventListener('mouseleave', function() {
          if (pressTimer && pressTimer !== 'long') clearTimeout(pressTimer);
          pressTimer = null;
        });
        btns.appendChild(btn);
      })(i);
    }
    section.appendChild(btns);
    panel.appendChild(section);
  };

export { PTZControl };
