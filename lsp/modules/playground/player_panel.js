/**
 * Dual-player toggle panel + Playback Sources sidebar.
 * Switches between MistServer embed player and FrameWorks Player.
 * Subscribes to global config for stream name changes.
 */

import { el } from '../core/dom_helpers.js';
import { fetchStreamSources } from './api.js';
import { getConfig, onChange } from './config.js';
import { mountMistPlayer, destroyMistPlayer } from './player_mist.js';
import { mountFWPlayer, destroyFWPlayer } from './player_fw.js';
import { buildContentEndpoints, getSourceTable } from './mist_sources.js';

const state = {
  activePlayer: 'mist',
  loaded: false,
  currentStream: null,
  currentSources: null,
  currentEndpoints: null,
};

let toolbar = null;
let container = null;
let actionsEl = null;
let playbackEl = null;
let tabs = {};
let loadBtn = null;
let unloadBtn = null;

export function createPlayerPanel() {
  toolbar = document.getElementById('pg-player-toolbar');
  container = document.getElementById('pg-player-container');
  actionsEl = document.getElementById('pg-player-actions');
  playbackEl = document.getElementById('pg-playback-sources');

  tabs.mist = el('button', {
    class: 'pg-player-tab active',
    onclick: function () { switchPlayer('mist'); },
  }, 'MistPlayer');

  tabs.frameworks = el('button', {
    class: 'pg-player-tab',
    onclick: function () { switchPlayer('frameworks'); },
  }, 'FrameWorks');

  toolbar.appendChild(tabs.mist);
  toolbar.appendChild(tabs.frameworks);

  // Load / Unload buttons
  loadBtn = el('button', {
    class: 'pg-btn pg-btn-primary',
    onclick: doLoad,
  }, 'Load');
  unloadBtn = el('button', {
    class: 'pg-btn pg-btn-danger',
    onclick: doUnload,
    disabled: true,
  }, 'Unload');
  actionsEl.appendChild(loadBtn);
  actionsEl.appendChild(unloadBtn);

  // Playback Sources panel (sidebar) - empty state + Poll button
  renderPlaybackEmpty();

  // React to config changes
  onChange(onConfigChange);
}

function onConfigChange(cfg) {
  if (cfg.streamName !== state.currentStream) {
    doUnload();
  }
}

// --- Load / Unload ---

function showLoadingIndicator() {
  container.innerHTML = '';
  var loading = el('div', { class: 'pg-player-loading' }, [
    el('div', { class: 'pg-player-loading-pulse' }),
    el('div', { class: 'pg-player-loading-logo' }),
    el('div', { class: 'pg-player-loading-text' }, 'Connecting...'),
  ]);
  container.appendChild(loading);
}

function clearLoadingIndicator() {
  var existing = container.querySelector('.pg-player-loading');
  if (existing) existing.remove();
}

function doLoad() {
  const cfg = getConfig();
  const name = cfg.streamName;
  if (!name) return;

  loadBtn.disabled = true;
  loadBtn.textContent = 'Loading...';
  showLoadingIndicator();

  fetchStreamSources(name).then(function (data) {
    const sources = data.source || data;
    state.currentStream = name;
    state.currentSources = sources;
    state.currentEndpoints = buildContentEndpoints(sources, name);
    state.loaded = true;

    clearLoadingIndicator();
    renderPlaybackSources(sources);
    mountCurrent();

    loadBtn.disabled = true;
    loadBtn.textContent = 'Load';
    unloadBtn.disabled = false;
  }).catch(function (e) {
    console.error('Failed to load stream sources for', name, e);
    clearLoadingIndicator();
    loadBtn.disabled = false;
    loadBtn.textContent = 'Load';
    renderPlaybackError(name);
  });
}

function doUnload() {
  destroyCurrent();
  state.currentStream = null;
  state.currentSources = null;
  state.currentEndpoints = null;
  state.loaded = false;

  loadBtn.disabled = false;
  loadBtn.textContent = 'Load';
  unloadBtn.disabled = true;

  renderPlaybackEmpty();
}

// --- Player toggle ---

export function switchPlayer(to) {
  if (to === state.activePlayer) return;
  if (state.loaded) destroyCurrent();
  state.activePlayer = to;

  tabs.mist.classList.toggle('active', to === 'mist');
  tabs.frameworks.classList.toggle('active', to === 'frameworks');

  if (state.loaded && state.currentStream) mountCurrent();
}

function destroyCurrent() {
  if (state.activePlayer === 'mist') {
    destroyMistPlayer(container);
  } else {
    destroyFWPlayer();
  }
  container.innerHTML = '';
}

function mountCurrent() {
  try {
    if (state.activePlayer === 'mist') {
      mountMistPlayer(container, state.currentStream);
    } else if (state.currentEndpoints) {
      mountFWPlayer(container, state.currentStream, state.currentEndpoints);
    }
  } catch (e) {
    console.error('Player mount failed:', e);
    container.innerHTML = '';
    container.appendChild(
      el('div', { class: 'pg-player-error' }, [
        el('span', null, e.message || 'Failed to mount player'),
        el('button', {
          class: 'pg-btn pg-btn-primary',
          onclick: function () {
            container.innerHTML = '';
            mountCurrent();
          },
        }, 'Retry'),
      ])
    );
  }
}

export function remountPlayer() {
  if (!state.loaded || !state.currentStream) return;
  destroyCurrent();
  mountCurrent();
}

// --- Playback Sources sidebar ---

function renderPlaybackEmpty() {
  playbackEl.innerHTML = '';
  const empty = el('div', { class: 'pg-empty' }, 'No sources loaded');
  const hint = el('div', { class: 'pg-empty-hint' }, 'Press Poll or Load to inspect');
  empty.appendChild(hint);
  playbackEl.appendChild(empty);

  const actions = el('div', { class: 'pg-playback-actions' });
  const pollBtn = el('button', {
    class: 'pg-btn pg-btn-primary',
    onclick: doPoll,
  }, 'Poll');
  actions.appendChild(pollBtn);
  playbackEl.appendChild(actions);
}

function renderPlaybackSources(sources) {
  playbackEl.innerHTML = '';
  const rows = getSourceTable(sources);

  if (!rows.length) {
    playbackEl.appendChild(el('div', { class: 'pg-empty' }, 'No sources available'));
  } else {
    for (const row of rows) {
      const urlSpan = el('span', { class: 'pg-playback-url' }, row.url);
      urlSpan.addEventListener('click', function () {
        copyWithFeedback(urlSpan, row.url);
      });
      var proto = row.protocol.toLowerCase().replace(/[^a-z]/g, '');
      playbackEl.appendChild(el('div', { class: 'pg-playback-row' }, [
        el('span', { class: 'pg-playback-label', 'data-proto': proto }, row.protocol),
        urlSpan,
      ]));
    }
  }

  const actions = el('div', { class: 'pg-playback-actions' });
  const pollBtn = el('button', {
    class: 'pg-btn pg-btn-primary',
    onclick: doPoll,
  }, 'Poll');
  actions.appendChild(pollBtn);
  playbackEl.appendChild(actions);
}

function renderPlaybackError(name) {
  playbackEl.innerHTML = '';
  const msg = el('div', { class: 'pg-empty' }, 'No sources for "' + name + '"');
  const hint = el('div', { class: 'pg-empty-hint' }, 'Stream may not be active');
  msg.appendChild(hint);
  playbackEl.appendChild(msg);

  const actions = el('div', { class: 'pg-playback-actions' });
  actions.appendChild(el('button', {
    class: 'pg-btn pg-btn-primary',
    onclick: doPoll,
  }, 'Poll'));
  playbackEl.appendChild(actions);
}

function doPoll() {
  const cfg = getConfig();
  const name = cfg.streamName;
  if (!name) return;

  fetchStreamSources(name).then(function (data) {
    const sources = data.source || data;
    state.currentSources = sources;
    renderPlaybackSources(sources);
  }).catch(function () {
    renderPlaybackError(name);
  });
}

function copyWithFeedback(element, text) {
  navigator.clipboard.writeText(text).then(function () {
    element.classList.add('pg-playback-url--copied');
    const orig = element.textContent;
    element.textContent = 'Copied!';
    setTimeout(function () {
      element.classList.remove('pg-playback-url--copied');
      element.textContent = orig;
    }, 1200);
  }).catch(function () {});
}
