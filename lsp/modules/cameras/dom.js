/**
 * Reusable DOM builder helpers for camera UI.
 * Uses CSS classes from main.css (.cam-pill, .cam-status, etc.)
 * All functions return native DOM elements.
 */
import { el } from '../core/dom_helpers.js';
import { uiHelpers } from '../core/ui_helpers.js';
import { navto } from '../core/navigation.js';

export const dom = {};

/** Colored status dot with tooltip. Uses .cam-status CSS classes. */
dom.statusBadge = function(status) {
  const cls = (status === 'online' || status === 'connected') ? 'online'
          : (status === 'error' ? 'error' : 'unknown');
  const span = el('span', {class: 'cam-status ' + cls, title: status || 'unknown'}, '\u25CF');
  return span;
};

/**
 * Small rounded pill/badge. Uses .cam-pill CSS class.
 * @param {string} text  Display text
 * @param {string=} cls  Optional extra CSS class ('green', 'red')
 */
dom.pill = function(text, cls) {
  const pill = el('span', {class: 'cam-pill'}, text);
  if (cls) pill.classList.add(cls);
  return pill;
};

/** Protocol name pills from a camera's protocols array. */
dom.protocolPills = function(protocols) {
  const cont = el('span');
  if (!protocols) return cont;
  for (let i = 0; i < protocols.length; i++) {
    cont.appendChild(dom.pill(protocols[i].type || protocols[i].protocol));
  }
  return cont;
};

/** Capability pills (PTZ, Audio, Analytics) from camera object. */
dom.capabilityPills = function(cam) {
  const cont = el('span');
  if (cam.hasPTZ) cont.appendChild(dom.pill('PTZ', 'green'));
  if (cam.hasAudio) cont.appendChild(dom.pill('Audio', 'green'));
  if (cam.hasMetadata) cont.appendChild(dom.pill('Analytics', 'green'));
  return cont;
};

/** Table of label:value pairs. Uses .cam-info-grid CSS class. Shows '-' for missing values. */
dom.infoGrid = function(items) {
  const table = el('table', {class: 'cam-info-grid nolay'});
  for (let i = 0; i < items.length; i++) {
    table.appendChild(
      el('tr', null,
        el('td', null, items[i].label),
        el('td', null, items[i].val || '-')
      )
    );
  }
  return table;
};

/** Navigation button with return icon. */
dom.backButton = function(label, tab, param) {
  const btn = el('button', {'data-icon': 'return'}, label);
  btn.addEventListener('click', function() {
    navto(tab, param);
  });
  return btn;
};

/** Shared header for camera detail/PTZ sub-tabs. */
dom.cameraSubtabHeader = function(cam, camId, currentTab) {
  const statusText = cam.status || 'unknown';
  const isOnline = cam.status === 'online' || cam.status === 'connected';
  const statusPill = el('span', {class: 'cam-pill ' + (isOnline ? 'green' : 'red')},
    statusText.charAt(0).toUpperCase() + statusText.slice(1)
  );
  const tabs = [
    {label: 'Detail', tab: 'Camera Detail'}
  ];
  if (cam.hasPTZ) {
    tabs.push({label: 'PTZ Control', tab: 'PTZ Control'});
  }
  tabs.push(false);
  tabs.push({label: 'Devices', tab: 'Devices'});

  for (let i = 0; i < tabs.length; i++) {
    if (!tabs[i]) { continue; }
    tabs[i].active = (tabs[i].tab === currentTab);
    tabs[i].icon = tabs[i].tab;
    tabs[i].onClick = (function(target) {
      return function() {
        navto(target, target === 'Devices' ? '' : camId);
      };
    })(tabs[i].tab);
  }

  return uiHelpers.createSubtabHeader({
    classes: ['subtab-header--camera'],
    title: cam.name || cam.id,
    status: statusPill,
    currentIcon: currentTab,
    tabs: tabs
  });
};

/** Map ONVIF analytics type URIs to friendly names. */
dom.analyticsTypeName = function(uri) {
  const map = {
    'tt:LineDetector': 'Line Crossing',
    'tt:FieldDetector': 'Field Detection',
    'tt:ObjectCounter': 'Object Counter',
    'tt:Tamper': 'Tamper Detection',
    'tt:FaceDetector': 'Face Detection',
    'tt:MotionDetector': 'Motion Detection',
    'tt:CellMotionDetector': 'Cell Motion',
    'tt:LoiteringDetector': 'Loitering',
    'tt:AbandonedObjectDetector': 'Abandoned Object'
  };
  return map[uri] || uri;
};

dom.dashboardSectionHeader = uiHelpers.dashboardSectionHeader;

/**
 * Protocol selector bar for the player.
 * Shows [Auto] + one button per available protocol.
 * Calls onAudioDetected(codec) if audio track is found in stream metadata.
 */
dom.protoLabel = {
  'whep': 'WHEP',
  'ws/video/mp4': 'WS/MP4',
  'html5/video/mp4': 'MP4',
  'html5/video/x-matroska': 'MKV',
  'html5/application/vnd.apple.mpegurl': 'HLS'
};

dom.protocolBar = function(MistVideo, onAudioDetected) {
  var bar = el('div', {class: 'ptz-hud ptz-protocol-bar'});
  var sources = MistVideo.info.source || [];
  var forced = MistVideo.options.forceType || false;
  var activeType = MistVideo.source ? MistVideo.source.type : '';

  var label = el('span', {class: 'ptz-protocol-label'}, 'Protocol');
  bar.appendChild(label);

  var autoBtn = el('button', {class: 'ptz-hud-pill'});
  autoBtn.textContent = 'Auto';
  if (!forced) autoBtn.classList.add('green');
  autoBtn.addEventListener('click', function() {
    var prev = bar.querySelector('.ptz-hud-pill.green');
    if (prev) prev.classList.remove('green');
    this.classList.add('green');
    MistVideo.options.forceType = false;
    MistVideo.reload();
  });
  bar.appendChild(autoBtn);

  var available = {};
  for (var i = 0; i < sources.length; i++) available[sources[i].type] = true;
  var order = ['whep', 'ws/video/mp4', 'html5/video/mp4', 'html5/video/x-matroska', 'html5/application/vnd.apple.mpegurl'];
  for (var o = 0; o < order.length; o++) {
    if (!available[order[o]]) continue;
    (function(type) {
      var btn = el('button', {class: 'ptz-hud-pill'});
      btn.textContent = dom.protoLabel[type];
      if (forced === type) btn.classList.add('green');
      btn.addEventListener('click', function() {
        var prev = bar.querySelector('.ptz-hud-pill.green');
        if (prev) prev.classList.remove('green');
        this.classList.add('green');
        MistVideo.options.forceType = type;
        MistVideo.reload();
      });
      bar.appendChild(btn);
    })(order[o]);
  }

  if (forced) {
    var activeLabel = dom.protoLabel[activeType] || activeType;
    if (activeType !== forced) {
      var fallback = el('span', {class: 'ptz-hud-pill red'});
      fallback.textContent = dom.protoLabel[forced] + ' unavailable - fell back to ' + activeLabel;
      bar.appendChild(fallback);
    }
  }

  if (onAudioDetected && MistVideo.info.meta && MistVideo.info.meta.tracks) {
    var hasOpus = false;
    var codecs = [];
    for (var t in MistVideo.info.meta.tracks) {
      if (MistVideo.info.meta.tracks[t].type === 'audio') {
        var codec = MistVideo.info.meta.tracks[t].codec;
        codecs.push(codec);
        if (codec === 'opus') hasOpus = true;
      }
    }
    if (codecs.length && !hasOpus && available['whep']) {
      onAudioDetected(codecs);
    }
  }

  return bar;
};

/**
 * Audio incompatibility warning banner with one-click transcode action.
 * @param {string} audioCodec  Detected audio codec name
 * @param {function(function(boolean))=} onTranscode  Callback that receives a done(ok) callback
 */
dom.audioWarningBanner = function(codecs, onTranscode) {
  var banner = el('div', {class: 'ptz-hud ptz-audio-warning'});
  var icon = el('span');
  icon.setAttribute('data-icon', 'alert-triangle');
  banner.appendChild(icon);
  var text = el('span');
  text.textContent = 'Audio (' + codecs.join(', ') + ') - add Opus transcode for WHEP playback';
  banner.appendChild(text);
  if (onTranscode) {
    var btn = el('button');
    btn.textContent = 'Add Opus transcode';
    btn.addEventListener('click', function() {
      btn.disabled = true;
      btn.textContent = 'Adding\u2026';
      onTranscode(function(ok) {
        if (ok) {
          text.textContent = 'Opus transcode added.';
          btn.textContent = 'Added';
        } else {
          btn.disabled = false;
          btn.textContent = 'Add Opus transcode';
        }
      });
    });
    banner.appendChild(btn);
  }
  return banner;
};
