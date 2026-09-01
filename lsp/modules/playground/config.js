/**
 * Global config state for the playground.
 * Drives ingest URIs, playback source polling, player endpoints, publisher WHIP URL.
 * Persists to localStorage; notifies subscribers on change.
 *
 * Base URL points to the MistServer management API (e.g. http://host:4242).
 * The HTTP protocol endpoint (port 8080) is auto-detected from the API response.
 */

import { el } from '../core/dom_helpers.js';
import { getHttpHost, setApiBase, detectHttpHost } from './api.js';

const LS_PREFIX = 'pg.';
const KEYS = ['baseUrl', 'streamName', 'viewerPath', 'thumbnailUrl'];

const state = {};
const listeners = [];

/**
 * Load config from localStorage. Called after detectHttpHost() resolves
 * so getHttpHost() returns the auto-detected HTTP endpoint.
 */
export function loadConfig() {
  state.baseUrl = localStorage.getItem(LS_PREFIX + 'baseUrl') || location.origin;
  state.streamName = localStorage.getItem(LS_PREFIX + 'streamName') || 'live';
  state.viewerPath = localStorage.getItem(LS_PREFIX + 'viewerPath') || '';
  state.thumbnailUrl = localStorage.getItem(LS_PREFIX + 'thumbnailUrl') || '';
  state.autoplayMuted = localStorage.getItem(LS_PREFIX + 'autoplayMuted') !== 'false';
}

function save() {
  for (const key of KEYS) {
    localStorage.setItem(LS_PREFIX + key, state[key]);
  }
  localStorage.setItem(LS_PREFIX + 'autoplayMuted', String(state.autoplayMuted));
}

function notify() {
  const cfg = getConfig();
  for (const fn of listeners) fn(cfg);
}

export function getConfig() {
  const base = state.baseUrl || location.origin;
  let hostname;
  try { hostname = new URL(base).hostname; } catch (e) { hostname = location.hostname; }
  return {
    baseUrl: base,
    streamName: state.streamName || 'live',
    viewerPath: state.viewerPath || '',
    thumbnailUrl: state.thumbnailUrl || '',
    autoplayMuted: state.autoplayMuted !== false,
    hostname: hostname,
  };
}

export function setConfig(partial) {
  let changed = false;
  let baseUrlChanged = false;
  for (const key of Object.keys(partial)) {
    if (key in state && state[key] !== partial[key]) {
      state[key] = partial[key];
      changed = true;
      if (key === 'baseUrl') baseUrlChanged = true;
    }
  }
  if (changed) {
    save();
    if (baseUrlChanged) {
      // Re-detect HTTP endpoint when the user changes the MistServer target
      setApiBase(state.baseUrl);
      detectHttpHost().then(function () { notify(); });
    } else {
      notify();
    }
  }
}

export function onChange(fn) {
  listeners.push(fn);
}

export function createConfigPanel() {
  const target = document.getElementById('pg-config');
  if (!target) return;

  const fields = [
    { key: 'baseUrl', label: 'Base URL', placeholder: location.origin },
    { key: 'streamName', label: 'Stream name', placeholder: 'live' },
    { key: 'viewerPath', label: 'Viewer path', placeholder: '(optional)' },
    { key: 'thumbnailUrl', label: 'Thumbnail URL', placeholder: '(optional)' },
  ];

  for (const f of fields) {
    const input = el('input', {
      type: 'text',
      value: state[f.key],
      placeholder: f.placeholder,
    });

    let debounceTimer = null;
    input.addEventListener('input', function () {
      state[f.key] = input.value;
      save();

      if (f.key === 'baseUrl') {
        clearTimeout(debounceTimer);
        debounceTimer = setTimeout(function () {
          setApiBase(state.baseUrl);
          detectHttpHost().then(function () { notify(); });
        }, 600);
      } else {
        notify();
      }
    });

    onChange(function () {
      if (document.activeElement !== input) {
        input.value = state[f.key];
      }
    });

    target.appendChild(
      el('div', { class: 'pg-config-field' }, [
        el('label', null, f.label),
        input,
      ])
    );
  }

  // Autoplay muted toggle
  const checkbox = el('input', {
    type: 'checkbox',
    id: 'pg-autoplay-muted',
    checked: state.autoplayMuted !== false ? '' : undefined,
  });
  checkbox.checked = state.autoplayMuted !== false;
  checkbox.addEventListener('change', function () {
    state.autoplayMuted = checkbox.checked;
    save();
    notify();
  });
  target.appendChild(
    el('div', { class: 'pg-config-toggle' }, [
      checkbox,
      el('label', { for: 'pg-autoplay-muted' }, 'Autoplay muted'),
    ])
  );
}
