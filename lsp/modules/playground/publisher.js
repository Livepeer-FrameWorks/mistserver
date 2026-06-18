/**
 * StreamCrafter WHIP publishing panel.
 * Uses <fw-streamcrafter> custom element from @livepeer-frameworks/streamcrafter-wc.
 * WHIP URL driven by global config - no separate stream name input.
 */

import { el } from '../core/dom_helpers.js';
import { getConfig, onChange } from './config.js';
import '@livepeer-frameworks/streamcrafter-wc/define';

let crafterEl = null;
let mountPoint = null;
let rendererSelect = null;

const LS_RENDERER_KEY = 'pg.streamcrafterRenderer';
const RENDERERS = ['auto', 'webgpu', 'webgl', 'canvas2d'];

const state = {
  renderer: loadRenderer(),
};

export function createPublisherPanel() {
  const publisherEl = document.getElementById('pg-publisher');
  const controls = el('div', { class: 'pg-pub-controls' }, [
    el('label', { class: 'pg-pub-label', for: 'pg-pub-renderer' }, 'Engine'),
  ]);
  rendererSelect = el('select', {
    id: 'pg-pub-renderer',
    class: 'pg-pub-select',
    onchange: onRendererChange,
  }, RENDERERS.map(function (renderer) {
    return el('option', { value: renderer }, rendererLabel(renderer));
  }));
  rendererSelect.value = state.renderer;
  controls.appendChild(rendererSelect);
  publisherEl.appendChild(controls);

  mountPoint = el('div', { class: 'pg-pub-preview' });
  publisherEl.appendChild(mountPoint);

  remountCrafter();
  onChange(updateWhipUrl);
}

function getWhipUrl() {
  const cfg = getConfig();
  return cfg.baseUrl + '/webrtc/' + cfg.streamName;
}

function updateWhipUrl() {
  if (crafterEl) {
    crafterEl.setAttribute('whip-url', getWhipUrl());
  }
}

function loadRenderer() {
  const stored = localStorage.getItem(LS_RENDERER_KEY);
  if (stored && RENDERERS.indexOf(stored) >= 0) {
    return stored;
  }
  return 'auto';
}

function onRendererChange(e) {
  const next = e && e.target ? e.target.value : null;
  if (!next || RENDERERS.indexOf(next) < 0 || next === state.renderer) {
    return;
  }
  state.renderer = next;
  localStorage.setItem(LS_RENDERER_KEY, next);
  remountCrafter();
}

function remountCrafter() {
  if (!mountPoint) return;
  if (crafterEl && crafterEl.parentNode === mountPoint) {
    mountPoint.removeChild(crafterEl);
  }
  crafterEl = document.createElement('fw-streamcrafter');
  crafterEl.setAttribute('controls', '');
  crafterEl.setAttribute('whip-url', getWhipUrl());
  crafterEl.initialProfile = 'broadcast';
  crafterEl.devMode = true;
  crafterEl.debug = true;
  crafterEl.enableCompositor = true;
  crafterEl.compositorConfig = { renderer: state.renderer };
  crafterEl.onStateChange = function (state, context) {
    console.debug('[StreamCrafter] State:', state, context);
  };
  crafterEl.onError = function (error) {
    console.error('[StreamCrafter] Error:', error);
  };
  mountPoint.appendChild(crafterEl);
}

function rendererLabel(renderer) {
  if (renderer === 'webgpu') return 'WebGPU';
  if (renderer === 'webgl') return 'WebGL';
  if (renderer === 'canvas2d') return 'Canvas2D';
  return 'Auto';
}
