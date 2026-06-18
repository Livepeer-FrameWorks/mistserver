import { el, getval, toNode, deepExtend } from '../core/dom_helpers.js';
import { classifyFields } from './disclosure_form.js';
import { disclosureForm } from './disclosure_form.js';
import { AppShell } from '../core/appshell.js';
import { MistIcons } from '../core/icons.js';
import { uiHelpers } from '../core/ui_helpers.js';
import { formEngine } from '../core/form_engine.js';

const LAYOUT_SVGS = {
  equal: '<svg viewBox="0 0 48 36" width="48" height="36"><rect x="1" y="1" width="14" height="16" fill="none" stroke="currentColor" stroke-width="1"/><rect x="17" y="1" width="14" height="16" fill="none" stroke="currentColor" stroke-width="1"/><rect x="33" y="1" width="14" height="16" fill="none" stroke="currentColor" stroke-width="1"/><rect x="1" y="19" width="14" height="16" fill="none" stroke="currentColor" stroke-width="1"/><rect x="17" y="19" width="14" height="16" fill="none" stroke="currentColor" stroke-width="1"/><rect x="33" y="19" width="14" height="16" fill="var(--accentColor)" stroke="currentColor" stroke-width="1" opacity="0.4"/></svg>',
  focussed: '<svg viewBox="0 0 48 36" width="48" height="36"><rect x="1" y="1" width="30" height="34" fill="var(--accentColor)" stroke="currentColor" stroke-width="1" opacity="0.4"/><rect x="33" y="1" width="14" height="10" fill="none" stroke="currentColor" stroke-width="1"/><rect x="33" y="13" width="14" height="10" fill="none" stroke="currentColor" stroke-width="1"/><rect x="33" y="25" width="14" height="10" fill="none" stroke="currentColor" stroke-width="1"/></svg>',
  none: '<svg viewBox="0 0 48 36" width="48" height="36"><rect x="2" y="5" width="20" height="14" fill="none" stroke="currentColor" stroke-width="1" transform="rotate(-3 12 12)"/><rect x="18" y="16" width="16" height="12" fill="none" stroke="currentColor" stroke-width="1" transform="rotate(2 26 22)"/><rect x="30" y="2" width="14" height="18" fill="var(--accentColor)" stroke="currentColor" stroke-width="1" opacity="0.4"/></svg>'
};

function setStyles(node, styles) {
  for (const key in styles) {
    if (!styles.hasOwnProperty(key)) continue;
    const val = styles[key];
    if (val === null || val === undefined) continue;
    if (key.indexOf('--') === 0 || key.indexOf('-') >= 0) {
      node.style.setProperty(key, val);
    } else {
      node.style[key] = val;
    }
  }
}

function parseResolution(str) {
  const parts = (str || '1920x1080').split('x');
  return [parseInt(parts[0], 10) || 1920, parseInt(parts[1], 10) || 1080];
}

function calculatePositions(sources, resolution, layout) {
  const n = sources.length;
  if (n === 0) return [];
  const resW = resolution[0], resH = resolution[1];
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

    positions.push({ x: 0, y: 0, w: cellW * extraW, h: cellH * extraH });

    let idx = 1;
    for (let r = 0; r < rows && idx < n; r++) {
      for (let c = 0; c < cols && idx < n; c++) {
        if (r < extraH && c < extraW) continue;
        positions.push({ x: c * cellW, y: r * cellH, w: cellW, h: cellH });
        idx++;
      }
    }
  } else {
    const cols2 = Math.ceil(Math.sqrt(n));
    const rows2 = Math.ceil(n / cols2);
    const cW = Math.floor(resW / cols2);
    const cH = Math.floor(resH / rows2);
    for (let i = 0; i < n; i++) {
      const col = i % cols2;
      const row = Math.floor(i / cols2);
      positions.push({ x: col * cW, y: row * cH, w: cW, h: cH });
    }
  }
  return positions;
}

function applyPositions(sources, positions) {
  for (let i = 0; i < sources.length && i < positions.length; i++) {
    sources[i].x = positions[i].x;
    sources[i].y = positions[i].y;
    sources[i].w = positions[i].w;
    sources[i].h = positions[i].h;
  }
}

function fitText(text) {
  const outer = el('span', {class: 'fit-text'});
  const inner1 = el('span');
  inner1.appendChild(el('span', null, text));
  outer.appendChild(inner1);
  outer.appendChild(el('span', null, text));
  return outer;
}

function buildSourceElement(source, resolution, focusCallback) {
  const sourceEl = el('div', {class: 'source', tabindex: '0'});
  setStyles(sourceEl, {
    '--x': source.x || 0,
    '--y': source.y || 0,
    '--width': source.w || 640,
    '--height': source.h || 480
  });
  sourceEl.setAttribute('data-label', Math.round(source.w || 640) + '\u00d7' + Math.round(source.h || 480));
  if (source.aspect) sourceEl.setAttribute('data-aspect', source.aspect);

  const nameEl = el('div', {class: 'sourcename'});
  nameEl.appendChild(fitText(source.stream || ''));
  const textEl = el('div', {class: 'text'});
  if (source.text !== null && source.text !== undefined) {
    textEl.textContent = source.text;
  }
  const thumbEl = el('div', {class: 'thumbnail'});
  thumbEl.appendChild(el('img'));

  sourceEl.appendChild(nameEl);
  sourceEl.appendChild(textEl);
  sourceEl.appendChild(thumbEl);

  sourceEl.addEventListener('click', function(e) {
    if (focusCallback) focusCallback();
  });

  return sourceEl;
}

function updateSourceElement(sourceEl, source) {
  setStyles(sourceEl, {
    '--x': source.x || 0,
    '--y': source.y || 0,
    '--width': source.w || 640,
    '--height': source.h || 480,
    '--dx': '0px', '--dy': '0px', '--dwidth': '0px', '--dheight': '0px'
  });
  sourceEl.setAttribute('data-label', Math.round(source.w || 640) + '\u00d7' + Math.round(source.h || 480));
  const nameEl = sourceEl.querySelector('.sourcename');
  nameEl.innerHTML = '';
  nameEl.appendChild(fitText(source.stream || ''));
  const textEl = sourceEl.querySelector('.text');
  textEl.textContent = '';
  if (source.text !== null && source.text !== undefined) {
    textEl.textContent = source.text;
  }
  if (source.aspect) {
    sourceEl.setAttribute('data-aspect', source.aspect);
  } else {
    sourceEl.removeAttribute('data-aspect');
  }
}

function setupDragResize(previewEl, settings, onUpdate) {
  let dragging = null;
  let activeMove = null;
  let activeUp = null;

  function getZone(e, rect) {
    const px = (e.clientX - rect.left) / rect.width;
    const py = (e.clientY - rect.top) / rect.height;
    const edge = 0.1;
    const top = py < edge, bottom = py > 1 - edge;
    const left = px < edge, right = px > 1 - edge;

    if (top && left) return 'nw';
    if (top && right) return 'ne';
    if (bottom && left) return 'sw';
    if (bottom && right) return 'se';
    if (top) return 'n';
    if (bottom) return 's';
    if (left) return 'w';
    if (right) return 'e';
    return 'move';
  }

  function getCursor(zone) {
    const map = { nw: 'nwse-resize', se: 'nwse-resize', ne: 'nesw-resize', sw: 'nesw-resize',
                n: 'ns-resize', s: 'ns-resize', w: 'ew-resize', e: 'ew-resize', move: 'move' };
    return map[zone] || 'default';
  }

  function getSnapPoints(sources, skipIdx, resolution) {
    const points = { x: [], y: [] };
    for (let i = 0; i < sources.length; i++) {
      if (i === skipIdx) continue;
      const s = sources[i];
      points.x.push(s.x, s.x + s.w);
      points.y.push(s.y, s.y + s.h);
    }
    points.x.push(0, resolution[0]);
    points.y.push(0, resolution[1]);
    return points;
  }

  function snapValue(val, snapPoints, threshold) {
    let best = val;
    let bestDist = threshold + 1;
    for (let i = 0; i < snapPoints.length; i++) {
      const d = Math.abs(val - snapPoints[i]);
      if (d < bestDist) { bestDist = d; best = snapPoints[i]; }
    }
    return bestDist <= threshold ? best : val;
  }

  function clearOffsets(sourceEl) {
    setStyles(sourceEl, { '--dx': '0px', '--dy': '0px', '--dwidth': '0px', '--dheight': '0px' });
  }

  function onPreviewMouseMove(e) {
    const sourceTarget = e.target.closest('.source');
    if (!sourceTarget || dragging) return;
    if (settings.layout !== 'none') { sourceTarget.style.cursor = 'default'; return; }
    const rect = sourceTarget.getBoundingClientRect();
    const zone = getZone(e, rect);
    sourceTarget.style.cursor = getCursor(zone);
  }

  function onPreviewMouseDown(e) {
    const sourceTarget = e.target.closest('.source');
    if (!sourceTarget) return;
    if (settings.layout !== 'none') return;
    if (e.button !== 0) return;
    e.preventDefault();

    const allSources = previewEl.querySelectorAll('.sources .source');
    const idx = Array.from(allSources).indexOf(sourceTarget);
    if (idx < 0 || idx >= settings.sources.length) return;

    const source = settings.sources[idx];
    const rect = sourceTarget.getBoundingClientRect();
    const zone = getZone(e, rect);
    const previewRect = previewEl.getBoundingClientRect();
    const resolution = parseResolution(settings.resolution);
    const scaleX = resolution[0] / previewRect.width;
    const scaleY = resolution[1] / previewRect.height;
    const startX = e.clientX, startY = e.clientY;
    const startSource = { x: source.x, y: source.y, w: source.w, h: source.h };
    const snaps = getSnapPoints(settings.sources, idx, resolution);
    const snapThreshold = 10;

    sourceTarget.setAttribute('data-focussed', '');
    dragging = { sourceEl: sourceTarget, zone: zone, idx: idx };

    function onMouseMove(ev) {
      let dx = (ev.clientX - startX) * scaleX;
      let dy = (ev.clientY - startY) * scaleY;

      if (zone === 'move') {
        if (ev.shiftKey) {
          if (Math.abs(dx) < Math.abs(dy)) dx = 0; else dy = 0;
        }
        let newX = snapValue(startSource.x + dx, snaps.x, snapThreshold);
        let newY = snapValue(startSource.y + dy, snaps.y, snapThreshold);
        newX = Math.max(0, Math.min(newX, resolution[0] - startSource.w));
        newY = Math.max(0, Math.min(newY, resolution[1] - startSource.h));
        setStyles(sourceTarget, {
          '--dx': ((newX - startSource.x) / scaleX) + 'px',
          '--dy': ((newY - startSource.y) / scaleY) + 'px'
        });
        dragging._newX = newX;
        dragging._newY = newY;
      } else {
        let newW = startSource.w, newH = startSource.h;
        let newX2 = startSource.x, newY2 = startSource.y;

        if (zone.indexOf('e') >= 0) newW = Math.max(20, startSource.w + dx);
        if (zone.indexOf('w') >= 0) { newW = Math.max(20, startSource.w - dx); newX2 = startSource.x + startSource.w - newW; }
        if (zone.indexOf('s') >= 0) newH = Math.max(20, startSource.h + dy);
        if (zone.indexOf('n') >= 0) { newH = Math.max(20, startSource.h - dy); newY2 = startSource.y + startSource.h - newH; }

        if (ev.ctrlKey || ev.metaKey) {
          const aspect = startSource.w / startSource.h;
          if (zone === 'n' || zone === 's') { newW = newH * aspect; }
          else { newH = newW / aspect; }
        }

        if (ev.shiftKey) {
          const cX = startSource.x + startSource.w / 2;
          const cY = startSource.y + startSource.h / 2;
          newX2 = cX - newW / 2;
          newY2 = cY - newH / 2;
        }

        newX2 = Math.max(0, newX2);
        newY2 = Math.max(0, newY2);
        newW = Math.min(newW, resolution[0] - newX2);
        newH = Math.min(newH, resolution[1] - newY2);

        setStyles(sourceTarget, {
          '--dx': ((newX2 - startSource.x) / scaleX) + 'px',
          '--dy': ((newY2 - startSource.y) / scaleY) + 'px',
          '--dwidth': ((newW - startSource.w) / scaleX) + 'px',
          '--dheight': ((newH - startSource.h) / scaleY) + 'px'
        });
        dragging._newX = newX2;
        dragging._newY = newY2;
        dragging._newW = newW;
        dragging._newH = newH;
      }
    }

    function onMouseUp() {
      document.removeEventListener('mousemove', onMouseMove);
      document.removeEventListener('mouseup', onMouseUp);
      activeMove = null;
      activeUp = null;
      if (!dragging) return;
      if (zone === 'move') {
        source.x = Math.round(dragging._newX != null ? dragging._newX : startSource.x);
        source.y = Math.round(dragging._newY != null ? dragging._newY : startSource.y);
      } else {
        source.x = Math.round(dragging._newX != null ? dragging._newX : startSource.x);
        source.y = Math.round(dragging._newY != null ? dragging._newY : startSource.y);
        source.w = Math.round(dragging._newW != null ? dragging._newW : startSource.w);
        source.h = Math.round(dragging._newH != null ? dragging._newH : startSource.h);
      }
      clearOffsets(sourceTarget);
      dragging = null;
      onUpdate();
    }

    document.addEventListener('mousemove', onMouseMove);
    document.addEventListener('mouseup', onMouseUp);
    activeMove = onMouseMove;
    activeUp = onMouseUp;
  }

  function onKeyDown(e) {
    if (e.key === 'Escape' && dragging) {
      if (activeMove) document.removeEventListener('mousemove', activeMove);
      if (activeUp) document.removeEventListener('mouseup', activeUp);
      activeMove = null;
      activeUp = null;
      clearOffsets(dragging.sourceEl);
      dragging = null;
    }
  }

  previewEl.addEventListener('mousemove', onPreviewMouseMove);
  previewEl.addEventListener('mousedown', onPreviewMouseDown);
  document.addEventListener('keydown', onKeyDown);

  return {
    destroy: function() {
      previewEl.removeEventListener('mousemove', onPreviewMouseMove);
      previewEl.removeEventListener('mousedown', onPreviewMouseDown);
      document.removeEventListener('keydown', onKeyDown);
      if (activeMove) document.removeEventListener('mousemove', activeMove);
      if (activeUp) document.removeEventListener('mouseup', activeUp);
    }
  };
}

function buildDesignerInline(settings, cap, options) {
  options = options || {};
  var onChange = options.onChange || null;
  var maxPreviewHeight = options.maxPreviewHeight || '50vh';
  let resolution = parseResolution(settings.resolution);

  const designer = el('div', {class: 'multiview_designer'});

  const srcSection = el('div');
  srcSection.appendChild(el('h4', null, 'Sources and labels'));

  const srcList = el('div', {class: 'composer-source-list'});
  srcSection.appendChild(srcList);

  function notifyChange() {
    if (onChange) onChange(settings);
  }

  function renderSourceList() {
    srcList.innerHTML = '';
    for (let i = 0; i < settings.sources.length; i++) {
      (function(idx) {
        const src = settings.sources[idx];
        const row = el('div', {class: 'composer-source-row'});

        const streamInput = el('input', {type: 'text', placeholder: 'Stream name or .png path'});
        streamInput.value = src.stream || '';
        streamInput.addEventListener('change', function() {
          src.stream = this.value;
          refreshPreview();
          notifyChange();
        });

        const labelSel = el('select');
        labelSel.appendChild(el('option', {value: 'name'}, 'Show stream name'));
        labelSel.appendChild(el('option', {value: 'none'}, 'No label'));
        labelSel.appendChild(el('option', {value: 'custom'}, 'Custom label'));
        if (src.text === null || src.text === undefined) labelSel.value = 'name';
        else if (src.text === '') labelSel.value = 'none';
        else labelSel.value = 'custom';

        const customLabel = el('input', {type: 'text', placeholder: 'Label text'});
        customLabel.value = src.text || '';
        customLabel.style.display = (src.text !== null && src.text !== undefined && src.text !== '') ? '' : 'none';
        customLabel.addEventListener('change', function() {
          src.text = this.value;
          refreshPreview();
          notifyChange();
        });

        labelSel.addEventListener('change', function() {
          const v = this.value;
          if (v === 'name') {
            src.text = null;
            customLabel.style.display = 'none';
          } else if (v === 'none') {
            src.text = '';
            customLabel.style.display = 'none';
          } else {
            src.text = customLabel.value || '';
            customLabel.style.display = '';
            customLabel.focus();
          }
          refreshPreview();
          notifyChange();
        });

        const removeBtn = el('button');
        removeBtn.setAttribute('data-icon', 'trash');
        removeBtn.title = 'Remove source';
        removeBtn.addEventListener('click', function() {
          settings.sources.splice(idx, 1);
          recalcAndRefresh();
          notifyChange();
        });

        row.appendChild(streamInput);
        row.appendChild(labelSel);
        row.appendChild(customLabel);
        row.appendChild(removeBtn);
        srcList.appendChild(row);
      })(i);
    }

    const addBtn = el('button', null, 'Add source');
    addBtn.setAttribute('data-icon', 'plus');
    addBtn.addEventListener('click', function() {
      const pos = calculatePositions([{}], resolution, 'equal')[0] || { x: 0, y: 0, w: resolution[0] / 2, h: resolution[1] / 2 };
      settings.sources.push({ stream: '', text: null, x: pos.x, y: pos.y, w: pos.w, h: pos.h });
      recalcAndRefresh();
      notifyChange();
    });
    srcList.appendChild(addBtn);
  }

  const resSection = el('div');
  resSection.style.margin = '0.5em 0';
  resSection.appendChild(el('h4', null, 'Output resolution'));
  const resW = el('input', {type: 'number', min: '1'});
  resW.value = resolution[0];
  resW.style.width = '5em';
  const resX = el('span', null, ' \u00d7 ');
  const resH = el('input', {type: 'number', min: '1'});
  resH.value = resolution[1];
  resH.style.width = '5em';

  resW.addEventListener('keydown', function(e) { if (e.key === 'x') { e.preventDefault(); resH.focus(); resH.select(); } });
  resH.addEventListener('keydown', function(e) { if (e.key === 'Backspace' && !resH.value) { e.preventDefault(); resW.focus(); } });

  function updateResolution() {
    const w = parseInt(resW.value, 10) || 1920;
    const h = parseInt(resH.value, 10) || 1080;
    settings.resolution = w + 'x' + h;
    resolution = [w, h];
    setStyles(previewEl, { '--total-width': w, '--total-height': h });
    recalcAndRefresh();
    notifyChange();
  }
  resW.addEventListener('change', updateResolution);
  resH.addEventListener('change', updateResolution);
  resSection.appendChild(resW);
  resSection.appendChild(resX);
  resSection.appendChild(resH);

  const layoutSection = el('div');
  layoutSection.style.margin = '0.5em 0';
  layoutSection.appendChild(el('h4', null, 'Grid layout'));

  const layouts = [
    { id: 'equal', label: 'Standard grid' },
    { id: 'focussed', label: 'Focussed' },
    { id: 'none', label: 'Freestyle' }
  ];

  const layoutBtns = el('div');
  setStyles(layoutBtns, { display: 'flex', gap: '0.5em' });
  for (let li = 0; li < layouts.length; li++) {
    (function(layout) {
      const btn = el('button', {class: 'composer-layout-btn'});
      btn.innerHTML = LAYOUT_SVGS[layout.id] + '<span>' + layout.label + '</span>';
      btn.title = layout.label;
      if (settings.layout === layout.id) btn.classList.add('active');
      btn.addEventListener('click', function() {
        if (settings.layout === layout.id) return;
        if (settings.layout === 'none' && layout.id !== 'none') {
          if (!confirm('Switching away from freestyle will reset custom positions. Continue?')) return;
        }
        settings.layout = layout.id;
        Array.from(layoutBtns.querySelectorAll('.composer-layout-btn')).forEach(function(b) {
          b.classList.remove('active');
        });
        btn.classList.add('active');
        previewEl.setAttribute('data-layout', layout.id);
        recalcAndRefresh();
        notifyChange();
      });
      layoutBtns.appendChild(btn);
    })(layouts[li]);
  }
  layoutSection.appendChild(layoutBtns);

  const layoutHelp = el('p', null,
    "The 'standard grid' creates equal cells. The 'focussed' layout allocates at least 2\u00d72 cells for the first source. " +
    "In 'freestyle' mode, click a cell to select it, then drag to move and drag edges to resize."
  );
  setStyles(layoutHelp, { fontSize: '0.85em', color: 'var(--secondaryTextColor)' });
  layoutSection.appendChild(layoutHelp);

  const cellNav = el('div', {class: 'cellnav'});
  let focusedIdx = -1;

  function updateCellNav() {
    cellNav.innerHTML = '';
    if (!settings.sources.length) return;
    const navLabel = el('span', null, 'Select cell: ');
    setStyles(navLabel, { fontSize: '0.85em', color: 'var(--secondaryTextColor)', marginRight: '0.25em' });
    cellNav.insertBefore(navLabel, cellNav.firstChild);
    for (let i = 0; i < settings.sources.length; i++) {
      (function(idx) {
        const name = settings.sources[idx].stream || ('Source ' + (idx + 1));
        const btn = el('button', null, name);
        btn.setAttribute('data-id', idx);
        if (idx === focusedIdx) btn.setAttribute('data-active', 'true');
        btn.addEventListener('click', function() { focusCell(idx); });
        cellNav.appendChild(btn);
      })(i);
    }
  }

  const previewEl = el('div', {class: 'multiview_preview'});
  previewEl.setAttribute('data-layout', settings.layout || 'equal');
  setStyles(previewEl, {
    '--total-width': resolution[0],
    '--total-height': resolution[1],
    'aspect-ratio': resolution[0] + ' / ' + resolution[1],
    'max-height': maxPreviewHeight
  });
  const sourcesContainer = el('div', {class: 'sources'});
  previewEl.appendChild(sourcesContainer);

  const detailsContainer = el('div', {class: 'details_container'});

  function focusCell(idx) {
    focusedIdx = idx;
    Array.from(previewEl.querySelectorAll('.source')).forEach(function(s) {
      s.removeAttribute('data-focussed');
    });
    const allSources = previewEl.querySelectorAll('.source');
    if (allSources[idx]) allSources[idx].setAttribute('data-focussed', '');
    updateCellNav();
    updateDetails();
  }

  function updateDetails() {
    detailsContainer.innerHTML = '';
    if (focusedIdx < 0 || focusedIdx >= settings.sources.length) return;
    const src = settings.sources[focusedIdx];

    let elements = [];
    elements.push({ label: 'Source', type: 'str', readonly: true, pointer: { main: src, index: 'stream' } });

    elements.push({
      label: 'Aspect ratio',
      type: 'select',
      select: [['', 'Default (contain)'], ['crop', 'Crop'], ['stretch', 'Stretch'], ['pattern', 'Pattern']],
      pointer: { main: src, index: 'aspect' },
      'function': function() { src.aspect = getval(this); refreshPreview(); notifyChange(); }
    });

    const freestyleFields = [
      { label: 'X', type: 'int', pointer: { main: src, index: 'x' }, classes: ['show-for-freestyle-layout'],
        'function': function() { src.x = Number(getval(this)) || 0; refreshPreview(); notifyChange(); } },
      { label: 'Y', type: 'int', pointer: { main: src, index: 'y' }, classes: ['show-for-freestyle-layout'],
        'function': function() { src.y = Number(getval(this)) || 0; refreshPreview(); notifyChange(); } },
      { label: 'Width', type: 'int', pointer: { main: src, index: 'w' }, classes: ['show-for-freestyle-layout'],
        'function': function() { src.w = Number(getval(this)) || 100; refreshPreview(); notifyChange(); } },
      { label: 'Height', type: 'int', pointer: { main: src, index: 'h' }, classes: ['show-for-freestyle-layout'],
        'function': function() { src.h = Number(getval(this)) || 100; refreshPreview(); notifyChange(); } }
    ];
    elements = elements.concat(freestyleFields);

    const detailsUI = toNode(formEngine.buildUI(elements));
    if (detailsUI) {
      detailsContainer.appendChild(detailsUI);
    }
  }

  function recalcAndRefresh() {
    if (settings.layout !== 'none') {
      const positions = calculatePositions(settings.sources, resolution, settings.layout);
      applyPositions(settings.sources, positions);
    } else {
      for (let i = 0; i < settings.sources.length; i++) {
        const s = settings.sources[i];
        if (s.x === undefined || s.y === undefined || s.w === undefined || s.h === undefined) {
          const defaults = calculatePositions(settings.sources, resolution, 'equal');
          if (defaults[i]) {
            s.x = defaults[i].x; s.y = defaults[i].y;
            s.w = defaults[i].w; s.h = defaults[i].h;
          }
        }
      }
    }
    renderSourceList();
    refreshPreview();
    updateCellNav();
    updateDetails();
  }

  function refreshPreview() {
    sourcesContainer.innerHTML = '';
    for (let i = 0; i < settings.sources.length; i++) {
      (function(idx) {
        const srcEl = buildSourceElement(settings.sources[idx], resolution, function() {
          focusCell(idx);
        });
        if (idx === focusedIdx) srcEl.setAttribute('data-focussed', '');

        const s = settings.sources[idx];
        if (s.w / resolution[0] < 0.02 || s.h / resolution[1] < 0.02) {
          const side = (s.x + s.w / 2) < resolution[0] / 2 ? 'left' : 'right';
          srcEl.setAttribute('data-pointout', side);
          srcEl.title = s.stream || 'Source ' + (idx + 1);
        }

        sourcesContainer.appendChild(srcEl);
      })(i);
    }
    MistIcons.init(sourcesContainer);
  }

  previewEl.addEventListener('contextmenu', function(e) {
    const sourceTarget = e.target.closest('.source');
    if (!sourceTarget) return;
    if (settings.layout !== 'none') return;
    e.preventDefault();
    const allSources = previewEl.querySelectorAll('.sources .source');
    const idx = Array.from(allSources).indexOf(sourceTarget);
    if (idx < 0) return;

    const menu = el('div', {class: 'context_menu'});
    setStyles(menu, {
      position: 'fixed', left: e.clientX + 'px', top: e.clientY + 'px', zIndex: 10000,
      background: 'var(--backgroundColor)', border: '1px solid var(--inputBackgroundColor)',
      padding: '0.25em', borderRadius: '0.25em', boxShadow: '0.1em 0.1em 0.5em var(--shadowColor)'
    });

    if (idx < settings.sources.length - 1) {
      const fwdBtn = el('button', null, 'Move forward');
      fwdBtn.addEventListener('click', function() {
        const tmp = settings.sources[idx];
        settings.sources[idx] = settings.sources[idx + 1];
        settings.sources[idx + 1] = tmp;
        if (focusedIdx === idx) focusedIdx = idx + 1;
        else if (focusedIdx === idx + 1) focusedIdx = idx;
        menu.remove();
        refreshPreview();
        updateCellNav();
        notifyChange();
      });
      menu.appendChild(fwdBtn);
    }
    if (idx > 0) {
      const bwdBtn = el('button', null, 'Move backward');
      bwdBtn.addEventListener('click', function() {
        const tmp = settings.sources[idx];
        settings.sources[idx] = settings.sources[idx - 1];
        settings.sources[idx - 1] = tmp;
        if (focusedIdx === idx) focusedIdx = idx - 1;
        else if (focusedIdx === idx - 1) focusedIdx = idx;
        menu.remove();
        refreshPreview();
        updateCellNav();
        notifyChange();
      });
      menu.appendChild(bwdBtn);
    }

    document.body.appendChild(menu);
    document.addEventListener('mousedown', function handler(ev) {
      if (!menu.contains(ev.target)) {
        menu.remove();
        document.removeEventListener('mousedown', handler);
      }
    });
  });

  designer.appendChild(srcSection);
  designer.appendChild(resSection);
  designer.appendChild(layoutSection);
  designer.appendChild(cellNav);
  designer.appendChild(previewEl);
  designer.appendChild(detailsContainer);

  if (settings.layout === 'none') {
    for (let k = 0; k < settings.sources.length; k++) {
      const s = settings.sources[k];
      if (s.x === undefined) {
        const defaults = calculatePositions(settings.sources, resolution, 'equal');
        applyPositions(settings.sources, defaults);
        break;
      }
    }
  } else {
    const positions = calculatePositions(settings.sources, resolution, settings.layout);
    applyPositions(settings.sources, positions);
  }

  renderSourceList();
  refreshPreview();
  updateCellNav();

  const dragHandler = setupDragResize(previewEl, settings, function() {
    refreshPreview();
    updateCellNav();
    updateDetails();
    notifyChange();
  });

  return {
    $el: designer,
    destroy: function() { dragHandler.destroy(); },
    refresh: function() { recalcAndRefresh(); }
  };
}

function openDesigner(settings, cap, onApply) {
  const designer = buildDesignerInline(settings, cap, { maxPreviewHeight: '75vh' });

  const buttons = el('div');
  setStyles(buttons, { textAlign: 'center', marginTop: '1em' });
  const applyBtn = el('button', {class: 'save'}, 'Apply');
  applyBtn.addEventListener('click', function() {
    if (settings.layout !== 'none') {
      for (let i = 0; i < settings.sources.length; i++) {
        delete settings.sources[i].x;
        delete settings.sources[i].y;
        delete settings.sources[i].w;
        delete settings.sources[i].h;
      }
    } else {
      for (let j = 0; j < settings.sources.length; j++) {
        settings.sources[j].x = Math.round(settings.sources[j].x || 0);
        settings.sources[j].y = Math.round(settings.sources[j].y || 0);
        settings.sources[j].w = Math.round(settings.sources[j].w || 100);
        settings.sources[j].h = Math.round(settings.sources[j].h || 100);
      }
    }
    onApply(settings);
    popup.close();
  });
  buttons.appendChild(applyBtn);
  designer.$el.appendChild(buttons);

  const modal = uiHelpers.openFormModal({
    size: 'lg',
    body: designer.$el
  });
  const popup = modal.popup;

  const origClose = popup.close.bind(popup);
  popup.close = function() {
    designer.destroy();
    origClose();
  };

  modal.close = function() {
    popup.close();
  };

  return modal;
}

export { buildDesignerInline };

export function composerEditor(item, cap, $area, editCallbacks) {
  let editCopy = deepExtend({}, item);

  if (!editCopy.sources) editCopy.sources = [];
  if (!editCopy.resolution) editCopy.resolution = '1920x1080';
  if (!editCopy.layout) {
    const hasPositions = editCopy.sources.length > 0 && editCopy.sources[0].x !== undefined;
    editCopy.layout = hasPositions ? 'none' : 'equal';
  }

  const elements = [{
    label: 'Display name',
    type: 'str',
    pointer: { main: editCopy, index: 'x-LSP-name' },
    help: 'Optional friendly name for this process.'
  }];

  const summary = el('div');
  setStyles(summary, { margin: '0.5em 0', padding: '0.5em', background: 'var(--secondaryBackgroundColor)', borderRadius: '0.25em' });
  function updateSummary() {
    summary.innerHTML = '';
    const n = editCopy.sources.length;
    summary.appendChild(el('span', null, n + ' source' + (n !== 1 ? 's' : '') + ' \u2022 '));
    summary.appendChild(el('span', null, editCopy.resolution + ' \u2022 '));
    const layoutNames = { equal: 'Standard grid', focussed: 'Focussed', none: 'Freestyle' };
    summary.appendChild(el('span', null, layoutNames[editCopy.layout] || editCopy.layout));
  }
  updateSummary();

  const designerBtnWrap = el('div', {class: 'bigbuttons'});
  const designerBtn = el('button', null, 'Open designer');
  designerBtn.setAttribute('data-icon', 'layout-grid');
  designerBtn.addEventListener('click', function() {
    openDesigner(editCopy, cap, function(updated) {
      editCopy = deepExtend(editCopy, updated);
      updateSummary();
    });
  });
  designerBtnWrap.appendChild(designerBtn);

  const baseUI = toNode(formEngine.buildUI(elements));
  if (baseUI) {
    $area.appendChild(baseUI);
  }
  $area.appendChild(designerBtnWrap);
  $area.appendChild(summary);

  const filteredCap = deepExtend({}, cap);
  if (filteredCap.required) {
    delete filteredCap.required.sources;
    delete filteredCap.required.resolution;
    delete filteredCap.required.layout;
  }
  if (filteredCap.optional) {
    delete filteredCap.optional.sources;
    delete filteredCap.optional.resolution;
    delete filteredCap.optional.layout;
    delete filteredCap.optional.text;
  }

  const build = formEngine.convertBuildOptions(filteredCap, editCopy);
  const classified = classifyFields(build);
  if (classified.basic.length || classified.advanced.length) {
    $area.appendChild(disclosureForm({
      basic: classified.basic,
      advanced: classified.advanced,
      startExpanded: AppShell.getMode() === 'advanced'
    }));
  }

  const btnContainer = el('div', {class: 'inline-editor-item-buttons'});
  const cancelBtn = el('button', {class: 'cancel'}, 'Cancel');
  cancelBtn.addEventListener('click', function() {
    editCallbacks.cancel();
  });
  const saveBtn = el('button', {class: 'save'}, 'Save process');
  saveBtn.addEventListener('click', function() {
    editCallbacks.save(editCopy);
  });
  btnContainer.appendChild(cancelBtn);
  btnContainer.appendChild(saveBtn);
  $area.appendChild(btnContainer);
}

