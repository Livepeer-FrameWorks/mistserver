/**
 * Camera Detail sub-tab - dashboard layout with tab nav, live snapshot,
 * device info, protocols (accordion), streams, and analytics.
 */
import { APP_NAME } from '@brand';
import { el } from '../core/dom_helpers.js';
import { accordionTree } from '../components/accordion_tree.js';
import { Component } from './component.js';
import { api } from './api.js';
import { dom } from './dom.js';
import { StreamDialog } from './stream_dialog.js';
import { formEngine } from '../core/form_engine.js';
import { navto } from '../core/navigation.js';
import { sockets } from '../core/sockets.js';

function CameraDetail() {}
  CameraDetail.prototype = new Component();

  CameraDetail.prototype.build = function() {
    const camId = this.params.other;
    if (!camId) { navto('Devices'); return; }
    if (!this.requireCameras('Camera Detail', camId)) return;

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
      pageHeader.appendChild(dom.cameraSubtabHeader(cam, camId, 'Camera Detail'));
    }

    const dashboard = el('div', {class: 'dashboard camera-detail-dashboard slab-shell'});
    container.appendChild(dashboard);

    this.renderPreview(cam, dashboard);
    this.renderDeviceInfo(cam, dashboard);
    this.renderProtocols(cam, camId, dashboard);
    this.renderPTZSettings(cam, camId, dashboard);
    this.renderStreams(cam, camId, dashboard);
    this.renderAnalytics(cam, dashboard);

    var cameraStreams = this.getStreamNames(cam);
    var defaultIdx = cam.defaultStream != null && cam.defaultStream >= 0 ? cam.defaultStream : 0;
    if (defaultIdx >= cameraStreams.length) defaultIdx = 0;
    if (cameraStreams.length) {
      var streamName = cameraStreams[defaultIdx].name;
      var streamConfig = mist.data.streams[streamName];
      var previewSection = dashboard.querySelector('.cam-section-preview');
      var hasMjpeg = false;
      if (streamConfig && streamConfig.processes) {
        for (var pn in streamConfig.processes) {
          if (streamConfig.processes[pn].codec === 'JPEG') { hasMjpeg = true; break; }
        }
      }
      if (hasMjpeg) {
        var imgEl = dashboard.querySelector('.cam-preview img');
        if (!imgEl) {
          imgEl = el('img', {alt: 'Camera preview'});
          previewSection.appendChild(imgEl);
        }
        imgEl.hidden = true;
        var loading = el('em', {class: 'cam-preview-loading'});
        loading.textContent = 'Connecting to MJPEG preview\u2026';
        previewSection.appendChild(loading);
        this.activateMjpegPreview(streamName, imgEl, loading);
      } else {
        previewSection.innerHTML = '';
        var empty = el('div', {class: 'cam-preview-empty'});
        var msg = el('em');
        msg.textContent = 'No MJPEG preview process on this stream.';
        empty.appendChild(msg);
        var self = this;
        var caps = (mist.data.capabilities && mist.data.capabilities.processes) || {};
        if (caps.AV || caps.FFMPEG) {
          var addBtn = el('button');
          addBtn.textContent = 'Add MJPEG process';
          addBtn.addEventListener('click', function() {
            addBtn.disabled = true;
            addBtn.textContent = 'Adding\u2026';
            self.addMjpegProcess(streamName, function(ok) {
              if (ok) navto('Camera Detail', camId);
              else { addBtn.disabled = false; addBtn.textContent = 'Add MJPEG process'; }
            });
          });
          empty.appendChild(addBtn);
        }
        previewSection.appendChild(empty);
      }
    } else if (cam.snapshotUri) {
      var uri = cam.snapshotUri;
      var imgEl = dashboard.querySelector('.cam-preview img');
      UI.interval.set(function() {
        if (imgEl) {
          imgEl.setAttribute('src', uri + (uri.indexOf('?') >= 0 ? '&' : '?') + '_t=' + Date.now());
        }
      }, 3e3);
    }
  };

  CameraDetail.prototype.renderPreview = function(cam, dashboard) {
    dashboard.appendChild(dom.dashboardSectionHeader('monitor', 'Preview'));
    var section = el('section', {class: 'cam-preview cam-section-preview'});
    var img = el('img');
    img.setAttribute('alt', 'Camera preview');
    if (cam.snapshotUri) {
      img.setAttribute('src', cam.snapshotUri);
    }
    img.addEventListener('error', function() {
      var fallback = el('em');
      fallback.textContent = 'Preview unavailable';
      this.replaceWith(fallback);
    });
    if (cam.snapshotUri || this.getStreamNames(cam).length) {
      section.appendChild(img);
    } else {
      var noSnap = el('em');
      noSnap.textContent = 'No preview available for this device.';
      section.appendChild(noSnap);
    }
    dashboard.appendChild(section);
  };

  CameraDetail.prototype.renderDeviceInfo = function(cam, dashboard) {
    dashboard.appendChild(dom.dashboardSectionHeader('info', 'Device Info'));
    const section = el('section', {class: 'cam-device-info'});
    section.appendChild(dom.infoGrid([
      {label: 'Name', val: cam.name},
      {label: 'Host', val: cam.host},
      {label: 'Status', val: cam.status},
      {label: 'Manufacturer', val: cam.manufacturer},
      {label: 'Model', val: cam.model},
      {label: 'Firmware', val: cam.firmwareVersion},
      {label: 'Serial', val: cam.serialNumber}
    ]));

    const allFeats = (cam.features || []).concat(cam.ptzFeatures || []);
    if (allFeats.length) {
      const badges = el('div', {class: 'cam-feature-badges'});
      for (let i = 0; i < allFeats.length; i++) {
        badges.appendChild(dom.pill(allFeats[i], 'green'));
      }
      section.appendChild(badges);
    }
    dashboard.appendChild(section);
  };

  CameraDetail.prototype.renderProtocols = function(cam, camId, dashboard) {
    if (!cam.protocols || !cam.protocols.length) return;

    dashboard.appendChild(dom.dashboardSectionHeader('link', 'Connectivity'));

    const sections = [];
    for (let i = 0; i < cam.protocols.length; i++) {
      const proto = cam.protocols[i];
      const pType = (proto.type || proto.protocol || 'unknown').toUpperCase();
      const subtitle = proto.address ? proto.address + (proto.port ? ':' + proto.port : '') : '';

      const content = el('div');
      const pCaps = [];
      if (proto.capabilities) {
        if (proto.capabilities.hasPTZ) pCaps.push('PTZ');
        if (proto.capabilities.hasAudio) pCaps.push('Audio');
        if (proto.capabilities.hasVideo) pCaps.push('Video');
        if (proto.capabilities.hasMetadata) pCaps.push('Metadata');
        if (proto.capabilities.hasRecording) pCaps.push('Recording');
        if (proto.capabilities.hasWebControl) pCaps.push('Web Control');
        if (proto.capabilities.hasTally) pCaps.push('Tally');
      }
      if (pCaps.length) {
        const capsDiv = el('div', {class: 'cam-proto-caps'});
        for (let ci = 0; ci < pCaps.length; ci++) {
          capsDiv.appendChild(dom.pill(pCaps[ci], 'green'));
        }
        content.appendChild(capsDiv);
      }

      if ((proto.type || proto.protocol) === 'onvif') {
        content.appendChild(this.buildCredentialsForm(camId));
      }

      sections.push({title: pType, subtitle: subtitle, content: content});
    }

    const sectionEl = el('section', {class: 'cam-connectivity'});
    sectionEl.appendChild(accordionTree({sections: sections, groupId: 'cam-protocols-' + camId}).$el);
    dashboard.appendChild(sectionEl);
  };

  CameraDetail.prototype.buildCredentialsForm = function(camId) {
    const credData = {username: '', password: ''};
    const wrapper = el('div', {class: 'cam-credentials-form'});
    const heading = el('strong', {class: 'cam-credentials-heading'});
    heading.textContent = 'ONVIF Credentials';
    wrapper.appendChild(heading);
    wrapper.appendChild(formEngine.buildUI([
      {label: 'Username', type: 'str', pointer: {main: credData, index: 'username'}},
      {label: 'Password', type: 'password', pointer: {main: credData, index: 'password'}},
      {type: 'buttons', buttons: [{
        label: 'Save Credentials', type: 'save', 'function': function() {
          api.updateCamera({
            id: camId,
            credentials: {protocol: 'onvif', username: credData.username, password: credData.password}
          }).then(function() {
            navto('Camera Detail', camId);
          })['catch'](function(err) {
            alert('Failed to save credentials: ' + err.message);
          });
        }
      }]}
    ]));
    return wrapper;
  };

  CameraDetail.prototype.renderPTZSettings = function(cam, camId, dashboard) {
    if (!cam.hasPTZ) return;

    dashboard.appendChild(dom.dashboardSectionHeader('arrows', 'PTZ Settings'));

    var ptzProtos = [['', 'Automatic (prefer ONVIF/VISCA)']];
    if (cam.protocols) {
      for (var i = 0; i < cam.protocols.length; i++) {
        var p = cam.protocols[i];
        if (p.capabilities && p.capabilities.hasPTZ) {
          var pName = (p.type || p.protocol || '').toLowerCase();
          ptzProtos.push([pName, pName.toUpperCase()]);
        }
      }
    }

    var settingsData = {ptzProtocol: cam.ptzProtocol || ''};
    var section = el('section', {class: 'cam-ptz-settings'});
    section.appendChild(formEngine.buildUI([
      {label: 'PTZ Protocol', type: 'select', select: ptzProtos, pointer: {main: settingsData, index: 'ptzProtocol'}},
      {type: 'buttons', buttons: [{
        label: 'Save', type: 'save', 'function': function() {
          api.updateCamera({id: camId, ptzProtocol: settingsData.ptzProtocol}).then(function() {
            navto('Camera Detail', camId);
          })['catch'](function(err) {
            alert('Failed to save PTZ settings: ' + err.message);
          });
        }
      }]}
    ]));
    dashboard.appendChild(section);
  };

  CameraDetail.prototype.renderStreams = function(cam, camId, dashboard) {
    if (!cam.streams || !cam.streams.length) return;

    dashboard.appendChild(dom.dashboardSectionHeader('play', 'Streams'));

    const existingStreamSources = {};
    if (mist.data.streams) {
      for (const sn in mist.data.streams) {
        if (mist.data.streams[sn].source) {
          existingStreamSources[mist.data.streams[sn].source] = sn;
        }
      }
    }

    const section = el('section', {class: 'cam-streams'});

    if (cam.streams.length > 1) {
      var currentDefault = cam.defaultStream != null && cam.defaultStream >= 0 ? cam.defaultStream : -1;
      var defaultBar = el('div', {class: 'cam-default-stream'});
      var defaultLabel = el('span', {class: 'cam-default-stream-label'});
      defaultLabel.textContent = 'Default playback:';
      defaultBar.appendChild(defaultLabel);
      var pillWrap = el('div', {class: 'cam-default-stream-pills'});
      for (var di = 0; di < cam.streams.length; di++) {
        (function(idx) {
          var st = cam.streams[idx];
          var pill = el('button', {class: 'cam-default-pill'});
          pill.textContent = (st.protocol || '').toUpperCase() +
            (st.resolution ? ' ' + st.resolution : st.width && st.height ? ' ' + st.width + 'x' + st.height : '') ||
            st.profile || st.name || 'Stream ' + (idx + 1);
          if (idx === currentDefault) pill.classList.add('active');
          pill.addEventListener('click', function() {
            var allPills = pillWrap.querySelectorAll('.cam-default-pill');
            for (var pi = 0; pi < allPills.length; pi++) allPills[pi].classList.remove('active');
            pill.classList.add('active');
            api.updateCamera({id: camId, defaultStream: idx}).then(function() {
              cam.defaultStream = idx;
            })['catch'](function(err) {
              console.warn('Failed to save default stream:', err.message);
            });
          });
          pillWrap.appendChild(pill);
        })(di);
      }
      defaultBar.appendChild(pillWrap);
      section.appendChild(defaultBar);
    }

    const table = el('table');
    const thead = el('thead');
    const headerRow = el('tr');
    const headers = ['Profile', 'Protocol', 'Resolution', 'FPS', 'URI', 'Actions'];
    for (let h = 0; h < headers.length; h++) {
      const th = el('th');
      th.textContent = headers[h];
      headerRow.appendChild(th);
    }
    thead.appendChild(headerRow);
    table.appendChild(thead);

    const tbody = el('tbody');
    for (let i = 0; i < cam.streams.length; i++) {
      const st = cam.streams[i];
      const actions = el('span');

      if (st.uri && existingStreamSources[st.uri]) {
        const streamingPill = dom.pill('Streaming', 'green');
        streamingPill.setAttribute('title', 'Active as: ' + existingStreamSources[st.uri]);
        actions.appendChild(streamingPill);
      }

      (function(stream, idx) {
        const addBtn = el('button');
        addBtn.textContent = 'Add to '+APP_NAME;
        addBtn.setAttribute('data-icon', 'plus');
        addBtn.addEventListener('click', function() {
          StreamDialog.show(camId, cam.name, stream, idx);
        });
        actions.appendChild(addBtn);
      })(st, i);

      const tr = el('tr');
      const tdProfile = el('td');
      tdProfile.textContent = st.profile || st.name || '-';
      const tdProto = el('td');
      tdProto.textContent = st.protocol || '-';
      const tdRes = el('td');
      tdRes.textContent = st.resolution || (st.width && st.height ? st.width + 'x' + st.height : '-');
      const tdFps = el('td');
      tdFps.textContent = st.framerate || st.fps || '-';
      const tdUri = el('td', {class: 'cam-uri'});
      tdUri.textContent = st.uri || '-';
      tdUri.setAttribute('title', st.uri || '');
      const tdActions = el('td');
      tdActions.appendChild(actions);
      tr.appendChild(tdProfile);
      tr.appendChild(tdProto);
      tr.appendChild(tdRes);
      tr.appendChild(tdFps);
      tr.appendChild(tdUri);
      tr.appendChild(tdActions);
      tbody.appendChild(tr);
    }
    table.appendChild(tbody);
    const scrollDiv = el('div', {class: 'table-scroll cam-streams-table'});
    scrollDiv.appendChild(table);
    section.appendChild(scrollDiv);
    dashboard.appendChild(section);
  };

  CameraDetail.prototype.renderAnalytics = function(cam, dashboard) {
    if (!cam.analytics || !cam.analytics.hasAnalytics) return;

    const analytics = cam.analytics;
    dashboard.appendChild(dom.dashboardSectionHeader('cpu', 'Analytics'));
    const section = el('section', {class: 'cam-analytics'});

    if (analytics.supportedModules && analytics.supportedModules.length) {
      const h4mods = el('h4');
      h4mods.textContent = 'Supported Modules';
      section.appendChild(h4mods);
      const modsDiv = el('div');
      for (let i = 0; i < analytics.supportedModules.length; i++) {
        const mod = analytics.supportedModules[i];
        modsDiv.appendChild(dom.pill(dom.analyticsTypeName(mod.type || mod.name)));
      }
      section.appendChild(modsDiv);
    }

    if (analytics.activeRules && analytics.activeRules.length) {
      const h4rules = el('h4');
      h4rules.textContent = 'Active Rules';
      section.appendChild(h4rules);
      const rulesDiv = el('div');
      for (let ri = 0; ri < analytics.activeRules.length; ri++) {
        const rule = analytics.activeRules[ri];
        rulesDiv.appendChild(dom.pill(rule.name || rule.type, rule.active ? 'green' : 'red'));
      }
      section.appendChild(rulesDiv);
    }

    if (analytics.objectClassifications && analytics.objectClassifications.length) {
      const h4obj = el('h4');
      h4obj.textContent = 'Object Classifications';
      section.appendChild(h4obj);
      const objDiv = el('div');
      for (let oi = 0; oi < analytics.objectClassifications.length; oi++) {
        objDiv.appendChild(dom.pill(analytics.objectClassifications[oi]));
      }
      section.appendChild(objDiv);
    }

    dashboard.appendChild(section);
  };

  CameraDetail.prototype.activateMjpegPreview = function(streamname, imgEl, loadingEl) {
    sockets.ws.info_json.subscribe(function(data) {
      if (!data.source) return;
      var mjpegUrl = null;
      var jpgUrl = null;
      for (var i in data.source) {
        if (data.source[i].type === 'html5/image/jpeg') {
          var url = data.source[i].url;
          if (url.indexOf('.mjpg') > -1) { mjpegUrl = url; }
          else if (!jpgUrl) { jpgUrl = url; }
        }
      }
      var displayUrl = mjpegUrl || jpgUrl;
      if (displayUrl && imgEl && imgEl.parentNode) {
        imgEl.setAttribute('src', displayUrl);
        imgEl.hidden = false;
        if (loadingEl && loadingEl.parentNode) loadingEl.parentNode.removeChild(loadingEl);
      }
    }, streamname);
  };

export { CameraDetail };
