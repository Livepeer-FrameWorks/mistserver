import fs from 'node:fs';
import path from 'node:path';
import vm from 'node:vm';

const ROOT = path.resolve(import.meta.dirname, '../../..');

function createStorage(initial) {
  let store = Object.assign({}, initial || {});
  return {
    getItem(key) {
      return Object.prototype.hasOwnProperty.call(store, key) ? String(store[key]) : null;
    },
    setItem(key, value) { store[key] = String(value); },
    removeItem(key) { delete store[key]; },
    clear() { store = {}; },
    _dump() { return Object.assign({}, store); }
  };
}

function deepExtend(target) {
  if (!target || (typeof target !== 'object')) target = {};
  for (let i = 1; i < arguments.length; i++) {
    const src = arguments[i];
    if (!src || (typeof src !== 'object')) continue;
    for (const k in src) {
      if (!Object.prototype.hasOwnProperty.call(src, k)) continue;
      const v = src[k];
      if (v && (typeof v === 'object') && !Array.isArray(v)) {
        const base = (target[k] && (typeof target[k] === 'object') && !Array.isArray(target[k])) ? target[k] : {};
        target[k] = deepExtend(base, v);
      } else {
        target[k] = v;
      }
    }
  }
  return target;
}

function createAnchor() {
  let u = new URL('http://example.test/');
  const a = {};
  function setFromHref(v) {
    try { u = new URL(v, u.href); } catch (e) {}
  }
  Object.defineProperty(a, 'href', {
    get() { return u.href; },
    set(v) { setFromHref(v); }
  });
  Object.defineProperty(a, 'protocol', {
    get() { return u.protocol; },
    set(v) { u.protocol = v; }
  });
  Object.defineProperty(a, 'hostname', {
    get() { return u.hostname; },
    set(v) { u.hostname = v; }
  });
  Object.defineProperty(a, 'port', {
    get() { return u.port; },
    set(v) { u.port = v; }
  });
  Object.defineProperty(a, 'pathname', {
    get() { return u.pathname; },
    set(v) { u.pathname = v; }
  });
  Object.defineProperty(a, 'search', {
    get() { return u.search; },
    set(v) { u.search = v; }
  });
  Object.defineProperty(a, 'searchParams', {
    get() { return u.searchParams; }
  });
  return a;
}

function makeNode(tag, attrs, children) {
  const node = {
    tag,
    attrs: attrs || null,
    children: [],
    appendChild(child) { this.children.push(child); return child; }
  };
  if (Array.isArray(children)) {
    for (let i = 0; i < children.length; i++) node.children.push(children[i]);
  } else if (children !== undefined && children !== null) {
    node.children.push(children);
  }
  return node;
}

function createChain() {
  const obj = {
    textValue: '',
    classes: {},
    children: [],
    text(v) { this.textValue = String(v); this.textContent = String(v); return this; },
    append(v) { this.children.push(v); return this; },
    appendChild(v) { this.children.push(v); return v; },
    empty() { this.children = []; this.textValue = ''; this.textContent = ''; return this; },
    addClass(v) { this.classes[v] = true; return this; },
    removeClass(v) { delete this.classes[v]; return this; },
    classList: {
      _classes: {},
      add(v) { this._classes[v] = true; obj.classes[v] = true; },
      remove(v) { delete this._classes[v]; delete obj.classes[v]; },
      contains(v) { return !!this._classes[v]; }
    }
  };
  Object.defineProperty(obj, 'textContent', {
    get() { return obj.textValue; },
    set(v) { obj.textValue = String(v); },
    enumerable: true,
    configurable: true
  });
  Object.defineProperty(obj, 'innerHTML', {
    get() { return obj.textValue; },
    set(v) { obj.textValue = String(v); obj.children = []; },
    enumerable: true,
    configurable: true
  });
  return obj;
}

function createBaseContext() {
  const bodyAttrs = {};
  const sessionStorage = createStorage();
  const localStorage = createStorage();
  const document = {
    body: {
      setAttribute(k, v) { bodyAttrs[k] = String(v); },
      getAttribute(k) { return bodyAttrs[k] || null; }
    },
    createElement(tag) {
      if (tag === 'a') return createAnchor();
      return makeNode(tag);
    },
    getElementById() { return null; },
    querySelectorAll() { return []; },
    addEventListener() {}
  };

  const ctx = {
    console,
    URL,
    JSON,
    Object,
    Array,
    Date,
    encodeURIComponent,
    decodeURIComponent,
    setTimeout,
    clearTimeout,
    AbortController: globalThis.AbortController,
    fetch() { return Promise.reject(new Error('fetch not stubbed')); },
    deepExtend,
    log() {},
    MD5(v) { return 'md5:' + v; },
    el: makeNode,
    cattablesort() {},
    renderQRCode() {},
    constants: { countrylist: {} },
    document,
    location: {
      hash: '',
      href: 'http://example.test/#',
      origin: 'http://example.test',
      protocol: 'http:'
    },
    sessionStorage,
    localStorage,
    UI: {
      modules: {},
      modeAwareTabs: {},
      tabHandlers: {},
      elements: {
        connection: {
          status: createChain(),
          user_and_host: createChain(),
          msg: createChain()
        }
      },
      format: { time(v) { return 't' + v; } },
      navtoCalls: [],
      navto(tab) { this.navtoCalls.push(tab); },
      interval: { list: {}, set() { return false; } },
      websockets: { create() { throw new Error('websocket not stubbed'); } }
    },
    mist: {
      user: { name: '', password: '', host: 'http://example.test/api' },
      data: { streams: {}, capabilities: { inputs: {} } },
      stored: { get() { return {}; }, set() {} },
      send() {}
    },
    parseURL(url, set) {
      const u = new URL(url, 'http://example.test/');
      if (set) {
        if (set.protocol) u.protocol = set.protocol;
        if (set.port !== undefined) u.port = String(set.port).replace(/^:/, '');
        if (set.pathname !== undefined) u.pathname = set.pathname;
        if (set.search !== undefined) u.search = set.search;
      }
      return {
        full: u.href,
        protocol: u.protocol + '//',
        host: u.hostname,
        port: u.port ? ':' + u.port : '',
        pathname: u.pathname || null,
        search: u.search ? u.search.replace(/^\?/, '') : null,
        searchParams: u.searchParams
      };
    }
  };

  ctx.format = ctx.UI.format;
  ctx.navto = function() {};
  ctx.apiClient = { send() {}, parseURL(url, set) { return ctx.parseURL(url, set); } };
  ctx.mistHelpers = { inputMatch() { return false; }, stored: { get() { return {}; }, set() {}, del() {} }, convertPushArr2Obj() { return {}; } };
  ctx.streamHints = { findStreamKeys() { return []; }, findFolderSubstreams() {}, findInputBySource() {}, updateLiveStreamHint() {} };
  ctx.findInput = function() { return null; };
  ctx.findOutput = function() { return null; };
  ctx.stream = { findMist(cb) { if (typeof cb === 'function') cb(); } };
  ctx.tabView = { showTab() {} };
  ctx.sockets = {};
  ctx.uiHelpers = { randomKey() { return ''; } };
  ctx.formEngine = { buildUI() {}, convertBuildOptions() {} };
  ctx.dynamicUI = { dynamic() {} };
  ctx.uiCore = {};
  ctx.copy = function() {};
  ctx.upload = function() {};
  ctx.MistIcons = {};
  ctx.editAutomation = function() {};
  ctx.editStream = function() {};
  ctx.pushesTab = function() {};

  ctx.window = {
    URL,
    location: ctx.location,
    document,
    sessionStorage,
    localStorage,
    mist: ctx.mist,
    UI: ctx.UI
  };
  ctx.global = ctx;
  return ctx;
}

const TEST_BRAND = {
  APP_NAME: 'TestServer',
  APP_TITLE: 'TestServer MI',
  LOGIN_TITLE: 'TestServer',
  API_HOST_HELP: 'Test API host help text'
};

function loadModule(relPath, overrides) {
  const ctx = createBaseContext();
  Object.assign(ctx, TEST_BRAND);
  if (overrides) {
    for (const k in overrides) {
      ctx[k] = overrides[k];
    }
  }
  if (!ctx.window) ctx.window = {};
  if (!ctx.window.URL) ctx.window.URL = URL;
  if (!ctx.window.location) ctx.window.location = ctx.location;
  if (!ctx.window.document) ctx.window.document = ctx.document;
  if (!ctx.window.sessionStorage) ctx.window.sessionStorage = ctx.sessionStorage;
  if (!ctx.window.localStorage) ctx.window.localStorage = ctx.localStorage;
  if (!ctx.window.mist) ctx.window.mist = ctx.mist;
  if (!ctx.window.UI) ctx.window.UI = ctx.UI;

  const abs = path.join(ROOT, relPath);
  let src = fs.readFileSync(abs, 'utf8');
  const exportAssignments = [];

  src = src.replace(/^\s*import\b[^;]*;?\s*$/gm, '');
  src = src.replace(/^(\s*)export\s*\{([^}]*)\};?\s*$/gm, (_, indent, spec) => {
    const parts = String(spec || '').split(',');
    for (const item of parts) {
      const trimmed = item.trim();
      if (!trimmed) continue;
      const aliasSplit = trimmed.split(/\s+as\s+/);
      const localName = (aliasSplit[0] || '').trim();
      const exportName = (aliasSplit[1] || localName).trim();
      if (!localName || !exportName) continue;
      exportAssignments.push('this.' + exportName + ' = ' + localName + ';');
    }
    return '';
  });
  src = src.replace(/^(\s*)export\s+(function|var|const|let|class)\s+([A-Za-z_$][\w$]*)/gm, (_, indent, kind, name) => {
    exportAssignments.push('this.' + name + ' = ' + name + ';');
    return indent + kind + ' ' + name;
  });

  if (exportAssignments.length) {
    src += '\n' + exportAssignments.join('\n') + '\n';
  }
  vm.runInNewContext(src, ctx, { filename: relPath });
  return ctx;
}

export { ROOT, loadModule, createStorage, deepExtend, createChain, TEST_BRAND };
