/**
 * Stream list sidebar + ingest URIs.
 * Driven by global config - ingest URIs update when streamName changes.
 */

import { el } from '../core/dom_helpers.js';
import { send, getHttpHost } from './api.js';
import { getConfig, setConfig, onChange } from './config.js';
import { buildIngestUris } from './mist_sources.js';

let streamsEl = null;
let ingestEl = null;

export function initStreams() {
  streamsEl = document.getElementById('pg-streams');
  ingestEl = document.getElementById('pg-ingest-uris');
  renderIngestUris();
  onChange(renderIngestUris);
}

export function refreshStreams() {
  return send({ active_streams: true }).then(function (data) {
    renderStreamList(data.active_streams || {});
  }).catch(function () {});
}

function renderStreamList(activeStreams) {
  streamsEl.innerHTML = '';
  // MistServer returns active_streams as a flat array of stream names
  const names = Array.isArray(activeStreams) ? activeStreams : Object.keys(activeStreams);
  const currentName = getConfig().streamName;

  if (!names.length) {
    const empty = el('div', { class: 'pg-empty' }, 'No active streams');
    const hint = el('div', { class: 'pg-empty-hint' }, 'Start publishing to see streams here');
    empty.appendChild(hint);
    streamsEl.appendChild(empty);
    return;
  }

  for (const name of names) {
    const item = el('div', {
      class: 'pg-stream-item' + (name === currentName ? ' selected' : ''),
      onclick: function () { selectStream(name); },
    }, [
      el('span', { class: 'pg-stream-name' }, name),
      el('span', { class: 'pg-stream-badge' }, 'live'),
    ]);
    streamsEl.appendChild(item);
  }
}

function selectStream(name) {
  setConfig({ streamName: name });

  const items = streamsEl.querySelectorAll('.pg-stream-item');
  for (let i = 0; i < items.length; i++) {
    items[i].classList.toggle('selected', items[i].querySelector('.pg-stream-name').textContent === name);
  }
}

function renderIngestUris() {
  ingestEl.innerHTML = '';
  const cfg = getConfig();
  const uris = buildIngestUris(cfg.streamName, getHttpHost());

  const entries = [
    { label: 'RTMP', url: uris.rtmp },
    { label: 'SRT', url: uris.srt },
    { label: 'WHIP', url: uris.whip },
  ];

  for (const entry of entries) {
    const urlSpan = el('span', {
      class: 'pg-ingest-url',
      title: 'Click to copy',
    }, entry.url);

    urlSpan.addEventListener('click', function () {
      copyWithFeedback(urlSpan, entry.url);
    });

    ingestEl.appendChild(el('div', { class: 'pg-ingest-row' }, [
      el('span', { class: 'pg-ingest-label' }, entry.label),
      urlSpan,
    ]));
  }
}

function copyWithFeedback(element, text) {
  navigator.clipboard.writeText(text).then(function () {
    element.classList.add('pg-ingest-url--copied');
    const orig = element.textContent;
    element.textContent = 'Copied!';
    setTimeout(function () {
      element.classList.remove('pg-ingest-url--copied');
      element.textContent = orig;
    }, 1200);
  }).catch(function () {});
}
