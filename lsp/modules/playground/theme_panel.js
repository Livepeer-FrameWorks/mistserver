/**
 * Skin & Theme panel for the MistPlayer playground.
 * Lets users switch skins, pick theme presets, tweak tokens live,
 * and see the code to reproduce their configuration.
 */

import { el } from '../core/dom_helpers.js';
import { getConfig } from './config.js';
import { getHttpHost } from './api.js';
import { MistThemes, resolveTheme } from '@player';

var themeList = [];
for (var k in MistThemes) {
  if (MistThemes.hasOwnProperty(k)) {
    var modes = [];
    if (MistThemes[k].dark) modes.push('dark');
    if (MistThemes[k].light) modes.push('light');
    themeList.push({ id: k, label: k.replace(/-/g, ' ').replace(/\b\w/g, function(c) { return c.toUpperCase(); }), modes: modes });
  }
}

const SKINS = [
  { id: 'default', label: 'Default' },
  { id: 'dev',     label: 'Dev' },
  { id: 'minimal', label: 'Minimal' },
  { id: 'branded', label: 'Branded' },
];

const PRESETS = [
  { label: 'Green',  accent: '#0f0' },
  { label: 'Blue',   accent: '#0066cc' },
  { label: 'Red',    accent: '#e33' },
  { label: 'Orange', accent: '#ff8800' },
  { label: 'Purple', accent: '#a855f7' },
  { label: 'Cyan',   accent: '#06b6d4' },
];

const state = {
  skin: 'default',
  theme: 'default',
  themeMode: 'dark',
  accent: null,
  tokens: {},
};

let panelEl = null;
let codeEl = null;
let remountFn = null;
let playerContainerEl = null;

export function createThemePanel(onRemount) {
  remountFn = onRemount;
  panelEl = document.getElementById('pg-theme-panel');
  codeEl = document.getElementById('pg-code-panel');
  playerContainerEl = document.getElementById('pg-player-container');
  if (!panelEl) return;

  panelEl.appendChild(el('div', { class: 'pg-theme-row' }, [
    el('span', { class: 'pg-theme-label' }, 'Skin'),
    buildSkinSelector(),
  ]));

  panelEl.appendChild(el('div', { class: 'pg-theme-row' }, [
    el('span', { class: 'pg-theme-label' }, 'Theme'),
    buildThemeSelector(),
  ]));

  panelEl.appendChild(el('div', { class: 'pg-theme-row' }, [
    el('span', { class: 'pg-theme-label' }, 'Accent'),
    buildAccentPicker(),
  ]));

  panelEl.appendChild(el('div', { class: 'pg-theme-row pg-theme-row--custom' }, [
    el('span', { class: 'pg-theme-label' }, 'Custom token'),
    buildCustomTokenInput(),
  ]));

  updateCodeSnippet();
}

function buildSkinSelector() {
  var group = el('div', { class: 'pg-theme-options' });
  for (var i = 0; i < SKINS.length; i++) {
    (function (skin) {
      var btn = el('button', {
        class: 'pg-theme-chip' + (state.skin === skin.id ? ' active' : ''),
        'data-skin': skin.id,
        onclick: function () {
          state.skin = skin.id;
          updateChips(group, 'data-skin', skin.id);
          requestRemount();
          updateCodeSnippet();
        },
      }, skin.label);
      group.appendChild(btn);
    })(SKINS[i]);
  }
  return group;
}

function buildThemeSelector() {
  var group = el('div', { class: 'pg-theme-options' });
  for (var i = 0; i < themeList.length; i++) {
    (function (theme) {
      var btn = el('button', {
        class: 'pg-theme-chip' + (state.theme === theme.id ? ' active' : ''),
        'data-theme': theme.id,
        onclick: function () {
          state.theme = theme.id;
          state.themeMode = theme.modes[0] || 'dark';
          updateChips(group, 'data-theme', theme.id);
          applyThemeTokens();
          updateCodeSnippet();
        },
      }, theme.label);
      group.appendChild(btn);
    })(themeList[i]);
  }
  return group;
}

function buildAccentPicker() {
  var group = el('div', { class: 'pg-theme-options' });
  for (var i = 0; i < PRESETS.length; i++) {
    (function (preset) {
      var swatch = el('button', {
        class: 'pg-theme-swatch',
        style: { background: preset.accent },
        title: preset.label + ' (' + preset.accent + ')',
        onclick: function () {
          state.accent = preset.accent;
          state.tokens['--mist-color-accent'] = preset.accent;
          applyAllTokens();
          highlightSwatch(group, this);
          updateCodeSnippet();
        },
      });
      group.appendChild(swatch);
    })(PRESETS[i]);
  }

  var customColor = el('input', {
    type: 'color',
    class: 'pg-theme-color-input',
    value: '#00ff00',
    title: 'Custom accent color',
    oninput: function () {
      state.accent = this.value;
      state.tokens['--mist-color-accent'] = this.value;
      applyAllTokens();
      highlightSwatch(group, null);
      updateCodeSnippet();
    },
  });
  group.appendChild(customColor);

  return group;
}

function buildCustomTokenInput() {
  var wrap = el('div', { class: 'pg-theme-custom-input' });
  var nameInput = el('input', {
    type: 'text',
    placeholder: '--mist-font-size',
    class: 'pg-theme-input',
  });
  var valueInput = el('input', {
    type: 'text',
    placeholder: '16px',
    class: 'pg-theme-input',
  });
  var applyBtn = el('button', {
    class: 'pg-btn pg-btn-primary',
    onclick: function () {
      var name = nameInput.value.trim();
      var val = valueInput.value.trim();
      if (!name || !val) return;
      if (name.indexOf('--') !== 0) name = '--mist-' + name;
      state.tokens[name] = val;
      applyAllTokens();
      updateCodeSnippet();
      nameInput.value = '';
      valueInput.value = '';
    },
  }, 'Apply');

  wrap.appendChild(nameInput);
  wrap.appendChild(valueInput);
  wrap.appendChild(applyBtn);
  return wrap;
}

// --- Actions ---

function applyThemeTokens() {
  var container = playerContainerEl
    ? playerContainerEl.querySelector('.mistvideo')
    : null;
  if (!container) return;

  var themeTokens = resolveTheme(state.theme, state.themeMode);
  if (themeTokens) {
    for (var name in themeTokens) {
      var prop = name.indexOf('--') === 0 ? name : '--mist-' + name;
      container.style.setProperty(prop, themeTokens[name]);
    }
  }

  // Re-apply user overrides on top
  for (var tok in state.tokens) {
    container.style.setProperty(tok, state.tokens[tok]);
  }
}

function applyAllTokens() {
  applyThemeTokens();
}

function requestRemount() {
  if (remountFn) remountFn();
}

export function getThemeState() {
  return {
    skin: state.skin,
    theme: state.theme,
    themeMode: state.themeMode,
    accent: state.accent,
    tokens: Object.assign({}, state.tokens),
  };
}

export function getSkinName() {
  return state.skin;
}

export function getThemeName() {
  return state.theme;
}

export function getThemeMode() {
  return state.themeMode;
}

export function onPlayerMounted() {
  applyThemeTokens();
}

// --- UI helpers ---

function updateChips(group, attr, activeId) {
  var btns = group.querySelectorAll('.pg-theme-chip');
  for (var i = 0; i < btns.length; i++) {
    btns[i].classList.toggle('active', btns[i].getAttribute(attr) === activeId);
  }
}

function highlightSwatch(group, activeBtn) {
  var btns = group.querySelectorAll('.pg-theme-swatch');
  for (var i = 0; i < btns.length; i++) {
    btns[i].classList.toggle('active', btns[i] === activeBtn);
  }
}

// --- Code snippet ---

function updateCodeSnippet() {
  if (!codeEl) return;
  codeEl.innerHTML = '';

  var cfg = getConfig();
  var host = getHttpHost();
  var lines = [];

  lines.push('<!-- IIFE (script tag) -->');
  lines.push('<script src="' + host + '/player.js"><\/script>');
  lines.push('<div id="player"><\/div>');
  lines.push('<script>');

  var opts = [];
  opts.push('  target: document.getElementById("player")');
  opts.push('  host: "' + host + '"');
  if (state.skin !== 'default') {
    opts.push('  skin: "' + state.skin + '"');
  }
  if (state.theme !== 'default') {
    opts.push('  theme: "' + state.theme + '"');
  }

  lines.push('mistPlay("' + (cfg.streamName || 'live') + '", {');
  lines.push(opts.join(',\n'));
  lines.push('});');

  var tokenKeys = Object.keys(state.tokens);
  if (tokenKeys.length) {
    lines.push('');
    lines.push('// Runtime token overrides');
    lines.push('var player = createPlayer({');
    lines.push('  target: document.getElementById("player"),');
    lines.push('  stream: "' + (cfg.streamName || 'live') + '",');
    lines.push('  host: "' + host + '"');
    lines.push('});');

    var tokenObj = [];
    for (var i = 0; i < tokenKeys.length; i++) {
      var tk = tokenKeys[i];
      var shortKey = tk.replace(/^--mist-/, '');
      tokenObj.push('  "' + shortKey + '": "' + state.tokens[tk] + '"');
    }
    lines.push('player.setTheme({');
    lines.push(tokenObj.join(',\n'));
    lines.push('});');
  }

  lines.push('<\/script>');

  lines.push('');
  lines.push('<!-- Or pure CSS theming -->');
  lines.push('<style>');
  lines.push('.mistvideo {');
  if (state.accent) {
    lines.push('  --mist-color-accent: ' + state.accent + ';');
  }
  for (var j = 0; j < tokenKeys.length; j++) {
    var tkey = tokenKeys[j];
    if (tkey === '--mist-color-accent' && state.accent) continue;
    lines.push('  ' + tkey + ': ' + state.tokens[tkey] + ';');
  }
  if (!state.accent && tokenKeys.length === 0) {
    lines.push('  /* override any --mist-* token here */');
  }
  lines.push('}');
  lines.push('<\/style>');

  var pre = el('pre', { class: 'pg-code-block' });
  var code = el('code', null, lines.join('\n'));
  pre.appendChild(code);
  codeEl.appendChild(pre);

  var copyBtn = el('button', {
    class: 'pg-btn pg-btn-primary pg-code-copy',
    onclick: function () {
      navigator.clipboard.writeText(lines.join('\n')).then(function () {
        copyBtn.textContent = 'Copied!';
        setTimeout(function () { copyBtn.textContent = 'Copy'; }, 1200);
      }).catch(function () {});
    },
  }, 'Copy');
  codeEl.appendChild(copyBtn);
}
