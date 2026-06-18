/**
 * Multiview tab - create server-side composed multi-camera streams.
 * Uses MistProcComposer for layout + MistProcAV for encoding.
 */
import { el, getval } from '../core/dom_helpers.js';
import { registerTab } from '../core/tab_registry.js';
import { api } from './api.js';
import { dom } from './dom.js';
import { uiHelpers } from '../core/ui_helpers.js';
import { formEngine } from '../core/form_engine.js';
import { apiClient } from '../core/api_client.js';
import { navto } from '../core/navigation.js';
import { defineSection } from '../core/section_registry.js';

defineSection({
  id: 'devices/multiview',
  page: 'Devices',
  title: 'Multiview',
  subtitle: 'Multi-camera composed stream',
  keywords: ['multiview', 'multi-camera', 'composer', 'layout', 'grid', 'focussed', 'resolution', '1080p', '4K'],
  requires: ['camera_list'],
  navTo: { tab: 'Multiview', other: '' }
});

defineSection({
  id: 'devices/ptz',
  page: 'Devices',
  title: 'PTZ Control',
  subtitle: 'Pan, tilt, zoom camera controls',
  keywords: ['PTZ', 'pan', 'tilt', 'zoom', 'focus', 'iris', 'preset', 'camera control', 'D-pad', 'VISCA', 'ONVIF'],
  requires: ['camera_list'],
  navTo: { tab: 'Devices', other: '' }
});

const LAYOUT_SVGS = {
  equal: '<svg viewBox="0 0 48 36" width="48" height="36"><rect x="1" y="1" width="14" height="16" fill="none" stroke="currentColor" stroke-width="1"/><rect x="17" y="1" width="14" height="16" fill="none" stroke="currentColor" stroke-width="1"/><rect x="33" y="1" width="14" height="16" fill="none" stroke="currentColor" stroke-width="1"/><rect x="1" y="19" width="14" height="16" fill="none" stroke="currentColor" stroke-width="1"/><rect x="17" y="19" width="14" height="16" fill="none" stroke="currentColor" stroke-width="1"/><rect x="33" y="19" width="14" height="16" fill="var(--accentColor)" stroke="currentColor" stroke-width="1" opacity="0.4"/></svg>',
  focussed: '<svg viewBox="0 0 48 36" width="48" height="36"><rect x="1" y="1" width="30" height="34" fill="var(--accentColor)" stroke="currentColor" stroke-width="1" opacity="0.4"/><rect x="33" y="1" width="14" height="10" fill="none" stroke="currentColor" stroke-width="1"/><rect x="33" y="13" width="14" height="10" fill="none" stroke="currentColor" stroke-width="1"/><rect x="33" y="25" width="14" height="10" fill="none" stroke="currentColor" stroke-width="1"/></svg>'
};

const RESOLUTIONS = [
  ['1920x1080', '1920 x 1080 (Full HD)'],
  ['1280x720', '1280 x 720 (HD)'],
  ['3840x2160', '3840 x 2160 (4K)'],
  ['640x480', '640 x 480 (VGA)']
];

// Mirror Util::sanitizeName from the controller: lowercase, keep alnum/_/-/.
// The controller rejects stream names that differ from their sanitized form.
function sanitizeStreamName(name) {
  return String(name || '').toLowerCase().replace(/[^a-z0-9_.-]/g, '_');
}

function calculatePositions(n, resolution, layout) {
  if (n === 0) return [];
  const parts = resolution.split('x');
  const resW = parseInt(parts[0], 10) || 1920;
  const resH = parseInt(parts[1], 10) || 1080;
  const positions = [];

  if (layout === 'focussed' && n > 1) {
    const nCells = n + 3;
    const cols = Math.ceil(Math.sqrt(nCells));
    const rows = Math.ceil(nCells / cols);
    const cellW = Math.floor(resW / cols);
    const cellH = Math.floor(resH / rows);
    const slots = rows * cols - n + 1;
    const extraH = Math.floor(Math.sqrt(slots));
    const extraW = Math.floor(slots / extraH);

    positions.push({x: 0, y: 0, w: cellW * extraW, h: cellH * extraH});
    var idx = 1;
    for (var r = 0; r < rows && idx < n; r++) {
      for (var c = 0; c < cols && idx < n; c++) {
        if (r < extraH && c < extraW) continue;
        positions.push({x: c * cellW, y: r * cellH, w: cellW, h: cellH});
        idx++;
      }
    }
  } else {
    const cols2 = Math.ceil(Math.sqrt(n));
    const rows2 = Math.ceil(n / cols2);
    const cW = Math.floor(resW / cols2);
    const cH = Math.floor(resH / rows2);
    for (var i = 0; i < n; i++) {
      positions.push({x: (i % cols2) * cW, y: Math.floor(i / cols2) * cH, w: cW, h: cH});
    }
  }
  return positions;
}

function multiviewPage(tab, other, prev, $main, $pageHeader) {
  $main.classList.add('page-body--flex-col', 'multiview-page-body');
  $pageHeader.classList.add('camera-tab-header');

  const backBtn = dom.backButton('Devices', 'Devices');
  uiHelpers.appendPageActions($pageHeader, [backBtn]);
  uiHelpers.setPageTitle($pageHeader, 'Create Multiview');

  if (!mist.data.camera_list) {
    $main.textContent = 'Loading cameras...';
    apiClient.send(function() { navto(tab, other); }, {camera_list: true});
    return;
  }

  const hasComposer = mist.data.capabilities && mist.data.capabilities.processes &&
                      mist.data.capabilities.processes.Composer;
  const hasAV = mist.data.capabilities && mist.data.capabilities.processes &&
                (mist.data.capabilities.processes.AV || mist.data.capabilities.processes.FFMPEG);

  if (!hasComposer) {
    const warn = el('div', {class: 'streams-empty-state'});
    const warnIcon = el('div');
    warnIcon.setAttribute('data-icon', 'alert-triangle');
    const warnH3 = el('h3');
    warnH3.textContent = 'Composer not available';
    const warnP = el('p');
    warnP.textContent = 'The Composer process (MistProcComposer) is required to create multiview streams. Please ensure it is compiled and available.';
    warn.appendChild(warnIcon);
    warn.appendChild(warnH3);
    warn.appendChild(warnP);
    $main.appendChild(warn);
    return;
  }

  const cameras = mist.data.camera_list || [];
  const selected = {};
  const settings = {
    resolution: '1920x1080',
    layout: 'equal',
    codec: 'H264',
    streamName: 'multiview'
  };

  // Camera picker
  const camSection = el('div', {class: 'multiview-section'});
  const camTitle = el('h3');
  camTitle.textContent = 'Select cameras';
  camSection.appendChild(camTitle);

  const camGrid = el('div', {class: 'multiview-cam-grid'});
  for (var i = 0; i < cameras.length; i++) {
    const cam = cameras[i];
    if (!cam.streams || !cam.streams.length) continue;

    const card = el('label', {class: 'multiview-cam-card'});
    const cb = el('input');
    cb.setAttribute('type', 'checkbox');
    cb.setAttribute('data-cam-id', cam.id);

    const streamSelect = el('select', {class: 'multiview-stream-select'});
    streamSelect.setAttribute('data-cam-id', cam.id);
    for (var si = 0; si < cam.streams.length; si++) {
      const st = cam.streams[si];
      const opt = el('option');
      opt.value = si;
      var optLabel = st.name || st.profile || ('Stream ' + si);
      optLabel += ' - ' + (st.protocol || '').toUpperCase();
      if (st.width && st.height) optLabel += ' ' + st.width + 'x' + st.height;
      if (st.fps) optLabel += ' @' + st.fps + 'fps';
      opt.textContent = optLabel;
      streamSelect.appendChild(opt);
    }
    streamSelect.addEventListener('change', function(e) {
      e.stopPropagation();
      var cid = this.getAttribute('data-cam-id');
      if (selected.hasOwnProperty(cid)) {
        selected[cid] = parseInt(this.value, 10);
        updatePreview();
      }
    });
    streamSelect.addEventListener('click', function(e) { e.preventDefault(); });

    cb.addEventListener('change', function() {
      var id = this.getAttribute('data-cam-id');
      var sel = this.parentNode.querySelector('.multiview-stream-select');
      if (this.checked) {
        selected[id] = parseInt(sel.value, 10);
      } else {
        delete selected[id];
      }
      this.parentNode.classList.toggle('selected', this.checked);
      updatePreview();
    });
    card.appendChild(cb);

    const cardName = el('span', {class: 'multiview-cam-name'});
    cardName.textContent = cam.name || cam.id;
    card.appendChild(cardName);
    card.appendChild(streamSelect);

    if (cam.snapshotUri && (cam.status === 'online' || cam.status === 'connected')) {
      const thumb = el('img', {class: 'multiview-cam-thumb'});
      thumb.setAttribute('src', cam.snapshotUri);
      thumb.setAttribute('alt', cam.name || cam.id);
      card.appendChild(thumb);
    }

    camGrid.appendChild(card);
  }
  camSection.appendChild(camGrid);

  // Settings
  const settingsSection = el('div', {class: 'multiview-section'});
  const settingsTitle = el('h3');
  settingsTitle.textContent = 'Settings';
  settingsSection.appendChild(settingsTitle);

  const settingsForm = formEngine.buildUI([
    {
      label: 'Stream name',
      type: 'str',
      pointer: {main: settings, index: 'streamName'},
      help: 'Name for the multiview output stream.'
    },
    {
      label: 'Resolution',
      type: 'select',
      select: RESOLUTIONS,
      pointer: {main: settings, index: 'resolution'},
      'function': function() { updatePreview(); }
    },
    {
      label: 'Layout',
      type: 'buttons',
      buttons: [
        {label: 'Equal Grid', value: 'equal'},
        {label: 'Focused', value: 'focussed'}
      ],
      value: settings.layout,
      'function': function() {
        settings.layout = getval(this);
        updatePreview();
      }
    },
    {
      label: 'Output codec',
      type: 'select',
      select: [['H264', 'H.264'], ['H265', 'H.265 (HEVC)']],
      pointer: {main: settings, index: 'codec'},
      condition: !!hasAV
    }
  ]);
  settingsSection.appendChild(settingsForm);

  // Layout preview
  const previewSection = el('div', {class: 'multiview-section'});
  const previewTitle = el('h3');
  previewTitle.textContent = 'Layout preview';
  previewSection.appendChild(previewTitle);
  const previewCanvas = el('div', {class: 'multiview-preview'});
  previewSection.appendChild(previewCanvas);

  function updatePreview() {
    previewCanvas.innerHTML = '';
    const selectedIds = Object.keys(selected);
    if (selectedIds.length === 0) {
      previewCanvas.textContent = 'Select cameras above to preview the layout.';
      return;
    }
    const positions = calculatePositions(selectedIds.length, settings.resolution, settings.layout);
    const parts = settings.resolution.split('x');
    const resW = parseInt(parts[0], 10) || 1920;
    const resH = parseInt(parts[1], 10) || 1080;

    previewCanvas.style.aspectRatio = resW + '/' + resH;

    for (var j = 0; j < positions.length; j++) {
      const pos = positions[j];
      const cell = el('div', {class: 'multiview-preview-cell'});
      cell.style.left = ((pos.x / resW) * 100) + '%';
      cell.style.top = ((pos.y / resH) * 100) + '%';
      cell.style.width = ((pos.w / resW) * 100) + '%';
      cell.style.height = ((pos.h / resH) * 100) + '%';

      const camObj = cameras.filter(function(c) { return c.id === selectedIds[j]; })[0];
      const label = el('span', {class: 'multiview-cell-label'});
      label.textContent = camObj ? (camObj.name || camObj.id) : selectedIds[j];
      cell.appendChild(label);
      previewCanvas.appendChild(cell);
    }
  }

  // Create button
  const createBtn = el('button', {class: 'save'});
  createBtn.setAttribute('data-icon', 'play');
  createBtn.textContent = 'Create multiview stream';
  const feedback = el('span', {class: 'multiview-feedback'});

  createBtn.addEventListener('click', function() {
    const selectedIds = Object.keys(selected);
    if (selectedIds.length < 2) {
      feedback.textContent = 'Select at least 2 cameras.';
      feedback.className = 'multiview-feedback error';
      return;
    }

    createBtn.disabled = true;
    feedback.textContent = 'Creating streams and composing...';
    feedback.className = 'multiview-feedback';

    // Ensure each selected camera has a stream, then build composer config
    const streamNames = [];
    var pending = selectedIds.length;

    function onAllReady() {
      const positions = calculatePositions(streamNames.length, settings.resolution, settings.layout);
      const sources = [];
      for (var k = 0; k < streamNames.length; k++) {
        sources.push({
          stream: streamNames[k],
          x: positions[k].x,
          y: positions[k].y,
          w: positions[k].w,
          h: positions[k].h
        });
      }

      const streamConfig = {
        source: 'compose',
        tags: ['multiview'],
        processes: {}
      };
      streamConfig.processes['Composer0'] = {
        process: 'Composer',
        sources: sources,
        resolution: settings.resolution,
        layout: settings.layout
      };
      if (hasAV) {
        const avProc = mist.data.capabilities.processes.AV ? 'AV' : 'FFMPEG';
        streamConfig.processes[avProc + '0'] = {
          process: avProc,
          'x-LSP-kind': 'video',
          codec: settings.codec
        };
      }

      const streamName = sanitizeStreamName(settings.streamName) || 'multiview';
      const addReq = {};
      addReq[streamName] = streamConfig;
      apiClient.send(function(d) {
        createBtn.disabled = false;
        if (d && d.streams && d.streams[streamName]) {
          feedback.textContent = 'Multiview stream created! Opening stream…';
          feedback.className = 'multiview-feedback success';
          navto('Status', streamName);
        } else {
          feedback.textContent = 'Stream creation sent. Check Streams page for result.';
          feedback.className = 'multiview-feedback success';
        }
      }, {addstream: addReq});
    }

    for (var si = 0; si < selectedIds.length; si++) {
      (function(camId) {
        // Check if camera already has a stream
        const cam = cameras.filter(function(c) { return c.id === camId; })[0];
        if (!cam) { pending--; return; }

        // Check if an existing stream already uses this camera's URI
        const streamIdx = selected[camId] || 0;
        const camUri = cam.streams && cam.streams[streamIdx] ? cam.streams[streamIdx].uri : '';
        let existingName = null;
        if (camUri && mist.data.streams) {
          for (const sn in mist.data.streams) {
            if (mist.data.streams[sn].source === camUri) {
              existingName = sn;
              break;
            }
          }
        }

        if (existingName) {
          streamNames.push(existingName);
          pending--;
          if (pending === 0) onAllReady();
        } else {
          api.createStream({id: camId, stream_index: streamIdx}).then(function(result) {
            streamNames.push(result.stream_name || camId);
            pending--;
            if (pending === 0) onAllReady();
          })['catch'](function() {
            streamNames.push(camId);
            pending--;
            if (pending === 0) onAllReady();
          });
        }
      })(selectedIds[si]);
    }
  });

  const actionRow = el('div', {class: 'multiview-actions'});
  actionRow.appendChild(createBtn);
  actionRow.appendChild(feedback);

  $main.appendChild(camSection);
  $main.appendChild(settingsSection);
  $main.appendChild(previewSection);
  $main.appendChild(actionRow);
}

registerTab('Multiview', function(tab, other, prev, $main, $pageHeader) {
  multiviewPage(tab, other, prev, $main, $pageHeader);
});
