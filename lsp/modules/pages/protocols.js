import { APP_NAME } from '@brand';
import { registerTab } from '../core/tab_registry.js';
import { classifyFields, disclosureForm } from '../components/disclosure_form.js';
import { register } from '../core/mode_dispatch.js';
import { apiClient } from '../core/api_client.js';
import { formEngine } from '../core/form_engine.js';
import { navto } from '../core/navigation.js';
import { el } from '../core/dom_helpers.js';
import { uiCore } from '../core/ui_core.js';

// --- Per-protocol documentation hints (shown in settings modal) ---

const PROTOCOL_HINTS = {
  'HTTP': 'Foundation for all browser-based protocols (HLS, DASH, WebRTC, etc.). Default port 8080 avoids conflicts with other web servers. Set <em>Public address</em> if behind a reverse proxy so '+APP_NAME+' can build correct playback URLs. The <em>Interface</em> setting controls which network address to listen on.',
  'HTTPS': 'Encrypted HTTP with TLS - provides the same functionality as HTTP but over a secure connection. Requires a certificate and private key file. Default port 4433 (change to 443 if no other service uses it). If you terminate TLS at a reverse proxy, you can use plain HTTP instead.',
  'HLS': 'Apple\'s HTTP Live Streaming, the most widely supported adaptive format. Serves TS-based segments at <code>/hls/STREAM/index.m3u8</code>. Use <em>Chunk path</em> to serve segments from a CDN. The <em>Live playlist limit</em> controls how many segments appear in the playlist (0 = unlimited). Disabling chunked transfer increases compatibility but reduces performance.',
  'CMAF': 'Modern container serving DASH (<code>.mpd</code>), HLS v7 (<code>.m3u8</code>), and Smooth Streaming (<code>/Manifest</code>) from one source using fMP4 segments. Preferred over legacy TS-based HLS for new deployments - better codec support (HEVC, AV1) and lower latency with chunked transfer.',
  'WebRTC': 'Sub-second latency streaming directly in the browser. Supports WHEP for playback and WHIP for ingest. Configure <em>STUN/TURN</em> servers for NAT traversal when viewers are behind firewalls. Needs its own certificate/key pair (separate from HTTPS). The <em>public UDP address</em> is critical when the server\'s external IP differs from its bind address.',
  'RTMP': 'Adobe\'s protocol, still widely used for ingest from OBS, vMix, and other encoders. Push URL format: <code>rtmp://HOST:PORT/PASSWORD/STREAM</code>. Use <em>Acceptable</em> to restrict to ingest-only or playback-only. Default port 1935. Supports RTMPS (TLS) when certificate/key are configured.',
  'RTSP': 'Standard protocol for IP cameras and professional equipment. Supports both RTP over UDP and TCP transport. Push URL: <code>rtsp://HOST:PORT/STREAM?pass=PASSWORD</code>. Default port 5554. Often used alongside ONVIF discovery for camera integration.',
  'TSSRT': 'Reliable low-latency transport designed for live contribution over unpredictable networks. Supports caller, listener, and rendezvous modes. Set a <em>passphrase</em> (10&ndash;79 chars) for AES encryption. The <em>Stream ID</em> field maps to the stream name in '+APP_NAME+'. Default port 8889.',
  'TS': 'Raw MPEG-TS over TCP, UDP, or RTP. Each instance serves one stream on a dedicated port (default 8888). For multicast delivery, use UDP targets. Supports HEVC, H264, AAC, MP3, AC3, and other codecs.',
  'DTSC': APP_NAME+'\'s native protocol for efficient server-to-server streaming. Supports all codecs and track types with minimal overhead. Used for load balancing, clustering, and inter-server communication.',
  'TSRIST': 'Reliable Internet Stream Transport - an open standard for reliable low-latency streaming over lossy networks. Similar purpose to SRT but with an IETF-backed specification.'
};

// --- Group subtitles ---

const GROUP_SUBTITLES = {
  'Server':     'Foundation listeners - most other protocols depend on these',
  'HTTP-based': 'Served over HTTP/HTTPS - enable the Server protocols first',
  'Streaming':  'Standalone listeners on dedicated ports'
};
import { uiHelpers } from '../core/ui_helpers.js';
import { registerDynamicProvider, extractKeywords } from '../core/section_registry.js';

registerDynamicProvider(function() {
  if (!mist.data.capabilities || !mist.data.capabilities.connectors) return [];
  var connectors = mist.data.capabilities.connectors;
  var entries = [];
  for (var id in connectors) {
    if (connectors[id] && ('PUSHONLY' in connectors[id])) continue;
    var cap = connectors[id];
    var friendly = (cap && cap.friendly) || id;
    var hint = PROTOCOL_HINTS[id] || '';
    var kw = [id.toLowerCase(), 'protocol'];
    if (cap.required) kw = kw.concat(extractKeywords(cap.required));
    if (cap.optional) kw = kw.concat(extractKeywords(cap.optional));
    entries.push({
      id: 'protocol/' + id.toLowerCase(),
      page: 'Protocols',
      title: friendly,
      subtitle: hint ? '' : (cap.desc || 'Protocol'),
      keywords: kw,
      requires: ['capabilities'],
      render: (function(connId, c, h) {
        return function($container) {
          if (h) {
            var info = el('div');
            info.innerHTML = h;
            $container.appendChild(info);
          } else if (c.desc) {
            $container.appendChild(el('p', null, c.desc));
          }
          var instances = findInstances(connId);
          if (instances.length) {
            $container.appendChild(el('p', {style: 'font-weight: 600; margin-top: 8px;'}, instances.length + ' instance' + (instances.length === 1 ? '' : 's') + ' configured'));
          } else {
            $container.appendChild(el('p', {style: 'color: var(--text-muted, #999); margin-top: 8px;'}, 'Not currently enabled'));
          }
        };
      })(id, cap, hint),
      navTo: { tab: 'Protocols', other: '' }
    });
  }
  return entries;
});

const setPageTitle = uiHelpers.setPageTitle;
const appendPageActions = uiHelpers.appendPageActions;
const pageIntro = uiHelpers.pageIntro;

// --- Utility functions ---

function isPushOnly(connId) {
  const cap = mist.data.capabilities.connectors[connId];
  return cap && ('PUSHONLY' in cap);
}

function getFriendly(connId) {
  const cap = mist.data.capabilities.connectors[connId];
  return (cap && cap.friendly) || connId;
}

function findInstances(connId) {
  const result = [];
  const protos = mist.data.config.protocols || [];
  for (let i = 0; i < protos.length; i++) {
    if (protos[i].connector === connId) {
      result.push({ index: i, data: protos[i] });
    }
  }
  return result;
}

function normalizeOnlineState(online) {
  if (online === 1 || online === true || online === 'Enabled' || online === 'Online') return 1;
  if (online === 2 || online === 'Starting' || online === 'Pending') return 2;
  if (online === 0 || online === false || online === 'Disabled' || online === 'Offline') return 0;
  return null;
}

function getStatusColor(instances) {
  if (!instances.length) return '';
  let hasOnline = false;
  let hasPending = false;
  let hasOffline = false;

  for (let i = 0; i < instances.length; i++) {
    if (instances[i].data.error) return 'red';
    const state = normalizeOnlineState(instances[i].data.online);
    if (state === 1) hasOnline = true;
    if (state === 2) hasPending = true;
    if (state === 0) hasOffline = true;
  }

  if (hasOffline && !hasOnline && !hasPending) return 'red';
  if (hasOffline || hasPending) return 'orange';
  if (hasOnline) return 'green';
  return 'green';
}

function classifyConnector(connId) {
  if (/^HTTPS$/i.test(connId) || /^HTTPS\.exe$/i.test(connId)) return 'https';
  if (/^HTTP$/i.test(connId) || /^HTTP\.exe$/i.test(connId)) return 'http';

  const cap = mist.data.capabilities.connectors[connId];
  if (cap && cap.deps && cap.deps !== '') {
    const deps = typeof cap.deps === 'string' ? cap.deps.split(', ') : cap.deps;
    for (let i = 0; i < deps.length; i++) {
      if (/^https?/i.test(deps[i])) return 'http-based';
    }
  }

  const name = connId.toLowerCase();
  if (/hls|dash|cmaf|webrtc|wss|mss|mp4|jpg|webm|flv|ogg|aac|mp3|wav|h264|hds/.test(name)) return 'http-based';

  return 'streaming';
}

function hasRequiredFields(connId) {
  const cap = mist.data.capabilities.connectors[connId];
  return cap && cap.required && Object.keys(cap.required).length > 0;
}

function hasAnyFields(connId) {
  const cap = mist.data.capabilities.connectors[connId];
  if (!cap) return false;
  if (cap.required && Object.keys(cap.required).length > 0) return true;
  if (cap.optional && Object.keys(cap.optional).length > 0) return true;
  return false;
}

function getUrlPattern(connId) {
  const cap = mist.data.capabilities.connectors[connId];
  if (!cap) return null;
  const rel = cap.url_rel;
  if (!rel) return null;
  if (Array.isArray(rel)) return rel[0] || null;
  return rel;
}

function getDeps(connId) {
  const cap = mist.data.capabilities.connectors[connId];
  if (!cap || !cap.deps || cap.deps === '') return null;
  const deps = typeof cap.deps === 'string' ? cap.deps.split(', ') : cap.deps;
  return deps.length ? deps.join(', ') : null;
}

// --- Default enable logic ---

function getDefaultEnableCandidates() {
  const connectors = mist.data.capabilities.connectors;
  const protos = mist.data.config.protocols || [];
  const configured = {};
  for (let i = 0; i < protos.length; i++) configured[protos[i].connector] = true;

  const toEnable = [];
  const skipped = [];
  for (const connId in connectors) {
    if (configured[connId]) continue;
    const cap = connectors[connId];
    if (('PUSHONLY' in cap) || ('NODEFAULT' in cap)) { skipped.push(connId); continue; }
    if (cap.required && Object.keys(cap.required).length > 0) { skipped.push(connId); continue; }
    toEnable.push(connId);
  }

  return { toEnable: toEnable, skipped: skipped };
}

function enableDefaults(callback) {
  const candidates = getDefaultEnableCandidates();
  const toEnable = candidates.toEnable;
  const skipped = candidates.skipped;

  let msg = 'Enable disabled protocols with default settings?\n\n';
  if (toEnable.length) {
    msg += toEnable.map(function(c) { return getFriendly(c); }).join(', ');
  } else {
    msg += 'None available.';
  }
  if (skipped.length) {
    msg += '\n\nRequire manual setup:\n' + skipped.map(function(c) { return getFriendly(c); }).join(', ');
  }
  if (!toEnable.length || !confirm(msg)) return;

  if (!mist.data.config.protocols) mist.data.config.protocols = [];
  for (let k = 0; k < toEnable.length; k++) {
    mist.data.config.protocols.push({ connector: toEnable[k] });
  }
  apiClient.send(function() {
    if (callback) callback();
  }, { config: mist.data.config });
}

// --- Field classification for modals ---
// convertBuildOptions() marks optional fields with classes: ['advanced-only'].
// The CSS rule [data-mode="guided"] .advanced-only { display: none !important }
// would hide them entirely. Instead, we strip that class and use the disclosure
// form to manage basic/advanced splitting ourselves.

function classifyAndClean(build, skipDesc) {
  const filtered = [];
  for (let i = 0; i < build.length; i++) {
    if (!build[i]) continue;
    if (skipDesc && build[i].type === 'help' && i === 0) continue;
    const el = build[i];
    if (el.classes) {
      el.classes = el.classes.filter(function(c) { return c !== 'advanced-only'; });
      if (!el.classes.length) delete el.classes;
    }
    filtered.push(el);
  }
  return classifyFields(filtered);
}

// --- Settings modal ---

function prefillDefaults(saveas, cap) {
  if (cap.required) {
    for (var k in cap.required) {
      if ('default' in cap.required[k] && !(k in saveas)) saveas[k] = cap.required[k]['default'];
    }
  }
  if (cap.optional) {
    for (var k in cap.optional) {
      if ('default' in cap.optional[k] && !(k in saveas)) saveas[k] = cap.optional[k]['default'];
    }
  }
}

function showAddInstanceModal(connId, onDone, mode) {
  const cap = mist.data.capabilities.connectors[connId];
  if (!cap) return;

  const modal = uiHelpers.openFormModal({
    size: 'md',
    title: getFriendly(connId) + ' - New instance'
  });

  const content = el('div', {class: 'protocol-modal-content'});
  const hint = PROTOCOL_HINTS[connId] || (cap.desc ? cap.desc : null);
  if (hint) {
    content.appendChild(formEngine.buildUI([{
      type: 'help',
      classes: (connId in PROTOCOL_HINTS) ? ['page-intro'] : [],
      help: hint
    }]));
  }

  const startExpanded = (mode === 'advanced');
  const saveas = { connector: connId };
  prefillDefaults(saveas, cap);
  const build = formEngine.convertBuildOptions(cap, saveas);
  const classified = classifyAndClean(build, !!hint);

  content.appendChild(disclosureForm({
    basic: classified.basic,
    advanced: classified.advanced,
    startExpanded: startExpanded
  }));

  const feedback = el('div', {style: 'margin-top: 0.75em; display: none'});
  content.appendChild(feedback);

  modal.setBody(content);
  modal.setFooter(formEngine.buildUI([{
    type: 'buttons',
    buttons: [
      { type: 'cancel', label: 'Cancel', 'function': function() { modal.close(); } },
      { type: 'save', label: 'Add instance', 'function': function() {
        const countBefore = findInstances(connId).length;
        feedback.style.display = 'none';
        apiClient.send(function() {
          const countAfter = findInstances(connId).length;
          if (countAfter > countBefore) {
            modal.close();
            onDone();
          } else {
            feedback.className = 'err_balloon red';
            feedback.textContent = 'Instance was not added - the server rejected it as a duplicate. Change at least one setting to differentiate this instance.';
            feedback.style.display = '';
          }
        }, { addprotocol: saveas });
      }}
    ]
  }]));
}

function showProtocolModal(connId, onDone, mode) {
  const cap = mist.data.capabilities.connectors[connId];
  if (!cap) return;

  const instances = findInstances(connId);
  const isEnabled = instances.length > 0;
  const modal = uiHelpers.openFormModal({
    size: 'md',
    title: getFriendly(connId)
  });

  const content = el('div', {class: 'protocol-modal-content'});
  let footerButtons = null;

  const hint = PROTOCOL_HINTS[connId] || (cap.desc ? cap.desc : null);
  if (hint) {
    const isRich = (connId in PROTOCOL_HINTS);
    content.appendChild(formEngine.buildUI([{
      type: 'help',
      classes: isRich ? ['page-intro'] : [],
      help: hint
    }]));
  }

  const startExpanded = (mode === 'advanced');

  if (!isEnabled) {
    const saveas = { connector: connId };
    const build = formEngine.convertBuildOptions(cap, saveas);
    const classified = classifyAndClean(build, !!hint);

    content.appendChild(disclosureForm({
      basic: classified.basic,
      advanced: classified.advanced,
      startExpanded: startExpanded
    }));

    footerButtons = [
      { type: 'cancel', label: 'Cancel', 'function': function() { modal.close(); } },
      { type: 'save', label: 'Enable', 'function': function() {
        apiClient.send(function() { modal.close(); onDone(); }, { addprotocol: saveas });
      }}
    ];
  } else {
    const useFooterActions = (instances.length === 1);
    for (let i = 0; i < instances.length; i++) {
      const instanceButtons = buildInstanceSection(content, connId, cap, instances[i], i, instances.length, modal, onDone, startExpanded, hint, useFooterActions);
      if (useFooterActions && instanceButtons) {
        footerButtons = instanceButtons;
      }
    }

    if (hasAnyFields(connId)) {
      content.appendChild(
        el('button', {
          'data-icon': 'plus',
          style: 'margin-top: 0.75em',
          onclick: function() {
            modal.close();
            showAddInstanceModal(connId, onDone, mode);
          }
        }, 'Add another instance')
      );
    }
  }

  modal.setBody(content);
  if (footerButtons && footerButtons.length) {
    modal.setFooter(formEngine.buildUI([{
      type: 'buttons',
      buttons: footerButtons
    }]));
  }
}

function buildInstanceSection(container, connId, cap, inst, index, total, modal, onDone, startExpanded, hasHint, actionsToFooter) {
  const editCopy = Object.assign({}, inst.data);
  const orig = Object.assign({}, inst.data);

  if (total > 1) {
    const h4 = el('h4', {}, 'Instance ' + (index + 1));
    Object.assign(h4.style, {
      marginTop: index > 0 ? '1.5em' : '0',
      paddingTop: index > 0 ? '1em' : '0',
      borderTop: index > 0 ? '1px solid var(--border-color)' : 'none'
    });
    container.appendChild(h4);
  }

  const build = formEngine.convertBuildOptions(cap, editCopy);
  const classified = classifyAndClean(build, !!hasHint);

  container.appendChild(disclosureForm({
    basic: classified.basic,
    advanced: classified.advanced,
    startExpanded: startExpanded
  }));

  const buttons = [
    { type: 'cancel', label: 'Cancel', 'function': function() { modal.close(); } },
    { type: 'save', label: 'Save', 'function': function() {
      apiClient.send(function() { modal.close(); onDone(); }, { updateprotocol: [orig, editCopy] });
    }}
  ];

  if (total > 1 || index === 0) {
    buttons.push({
      label: 'Remove',
      'function': function() {
        if (!confirm('Remove this instance of ' + getFriendly(connId) + '?')) return;
        apiClient.send(function() { modal.close(); onDone(); }, { deleteprotocol: orig });
      }
    });
  }

  if (actionsToFooter) {
    return buttons;
  }
  container.appendChild(formEngine.buildUI([{ type: 'buttons', buttons: buttons }]));
  return null;
}

// --- Card builder (replaces pill builder) ---

function buildCard(connId, onRefresh, mode, ctxMenu) {
  const cap = mist.data.capabilities.connectors[connId];
  const instances = findInstances(connId);
  const isEnabled = instances.length > 0;
  let hasError = false;
  for (let e = 0; e < instances.length; e++) {
    if (instances[e].data.error) { hasError = true; break; }
  }
  const color = getStatusColor(instances);

  const card = el('div', {class: 'proto-card'});
  if (isEnabled) card.classList.add('enabled');
  if (hasError) card.classList.add('has-error');

  // Header row: dot + name + count + toggle
  const header = el('div', {class: 'proto-card-header'});

  const dot = el('span', {class: 'proto-card-status'});
  if (color) dot.classList.add(color);
  header.appendChild(dot);

  const name = el('span', {class: 'proto-card-name'}, connId);
  header.appendChild(name);

  let count = null;
  if (isEnabled && instances.length > 1) {
    count = el('span', {class: 'proto-card-count'}, '\u00d7' + instances.length);
    header.appendChild(count);
  }

  const checkbox = el('input', {type: 'checkbox'});
  checkbox.checked = isEnabled;
  const toggleLabel = el('label', {class: 'proto-pill-toggle'});
  toggleLabel.appendChild(checkbox);
  toggleLabel.appendChild(el('span', {class: 'proto-pill-slider'}));
  header.appendChild(toggleLabel);

  card.appendChild(header);

  // Description row
  const friendly = getFriendly(connId);
  if (friendly && friendly !== connId) {
    card.appendChild(el('div', {class: 'proto-card-desc'}, friendly));
  }

  // Meta row (advanced mode only)
  if (mode === 'advanced') {
    const urlPattern = getUrlPattern(connId);
    const deps = getDeps(connId);
    const port = (isEnabled && instances[0].data.port) ? instances[0].data.port : null;

    if (urlPattern || deps || port) {
      const meta = el('div', {class: 'proto-card-meta'});
      if (urlPattern) {
        meta.appendChild(el('span', {class: 'proto-card-url'}, urlPattern));
      }
      if (deps) {
        meta.appendChild(el('span', {class: 'proto-card-dep'}, 'Requires ' + deps));
      }
      if (port) {
        meta.appendChild(el('span', {class: 'proto-card-port'}, 'Port ' + port));
      }
      card.appendChild(meta);
    }
  }

  // Click card to open modal (not just header)
  card.addEventListener('click', function(e) {
    if (e.target.closest('.proto-pill-toggle')) return;
    showProtocolModal(connId, onRefresh, mode);
  });
  card.addEventListener('contextmenu', function(e) {
    if (e.target.closest('.proto-pill-toggle')) return;
    e.preventDefault();
    e.stopPropagation();
    if (!ctxMenu) return;
    const friendly = getFriendly(connId);
    const inst = findInstances(connId);
    const en = inst.length > 0;
    const actions = [
      ['Configure', function() { ctxMenu.hide(); showProtocolModal(connId, onRefresh, mode); }, 'settings', 'Open protocol settings'],
      [en ? 'Disable' : 'Enable', function() {
        if (en) {
          let pending = inst.length;
          for (let j = 0; j < inst.length; j++) {
            apiClient.send(function() { pending--; if (!pending) onRefresh(); }, { deleteprotocol: inst[j].data });
          }
        } else {
          if (hasRequiredFields(connId)) {
            showProtocolModal(connId, onRefresh, mode);
          } else {
            apiClient.send(function() { onRefresh(); }, { addprotocol: { connector: connId } });
          }
        }
        ctxMenu.hide();
      }, en ? 'toggle-left' : 'toggle-right', en ? 'Disable this protocol' : 'Enable this protocol']
    ];
    if (en && hasAnyFields(connId)) {
      actions.push(['Add instance', function() {
        ctxMenu.hide();
        showAddInstanceModal(connId, onRefresh, mode);
      }, 'plus', 'Add another instance of this protocol']);
    }
    const header = el('div', {class: 'context-menu-header'}, friendly || connId);
    ctxMenu.show([[header], actions], e);
  });
  card.style.cursor = 'pointer';

  // Toggle behavior
  checkbox.addEventListener('change', function(e) {
    e.stopPropagation();
    const checked = this.checked;
    if (checked) {
      if (hasRequiredFields(connId)) {
        this.checked = false;
        showProtocolModal(connId, onRefresh, mode);
        return;
      }
      apiClient.send(function() { onRefresh(); }, { addprotocol: { connector: connId } });
    } else {
      const toDelete = findInstances(connId);
      if (!toDelete.length) return;
      let pending = toDelete.length;
      for (let j = 0; j < toDelete.length; j++) {
        apiClient.send(function() {
          pending--;
          if (pending === 0) onRefresh();
        }, { deleteprotocol: toDelete[j].data });
      }
    }
  });

  return {
    el: card,
    connId: connId,
    checkbox: checkbox,
    updateStatus: function() {
      const inst = findInstances(connId);
      const en = inst.length > 0;
      const c = getStatusColor(inst);
      let err = false;
      for (let k = 0; k < inst.length; k++) {
        if (inst[k].data.error) { err = true; break; }
      }

      card.classList.toggle('enabled', en);
      card.classList.toggle('has-error', err);
      checkbox.checked = en;
      dot.classList.remove('green', 'orange', 'red');
      if (c) dot.classList.add(c);

      // Update count badge
      const existingCount = card.querySelector('.proto-card-count');
      if (en && inst.length > 1) {
        if (existingCount) {
          existingCount.textContent = '\u00d7' + inst.length;
        } else {
          const newCount = el('span', {class: 'proto-card-count'}, '\u00d7' + inst.length);
          name.after(newCount);
        }
      } else {
        if (existingCount) existingCount.remove();
      }
    }
  };
}

// --- Group rendering helper ---

function appendGroup(page, title, connIds, onRefresh, mode, ctxMenu) {
  if (!connIds.length) return [];

  const group = el('section', {class: 'proto-group-slab'});
  const head = el('div', {class: 'proto-group-head'});
  head.appendChild(el('div', {class: 'proto-group-heading'}, title));
  if (GROUP_SUBTITLES[title]) {
    head.appendChild(el('div', {class: 'proto-group-subtitle'}, GROUP_SUBTITLES[title]));
  }
  group.appendChild(head);

  const body = el('div', {class: 'proto-group-body'});
  const grid = el('div', {class: 'proto-card-grid'});
  const ctrls = [];
  for (let i = 0; i < connIds.length; i++) {
    const ctrl = buildCard(connIds[i], onRefresh, mode, ctxMenu);
    ctrls.push(ctrl);
    grid.appendChild(ctrl.el);
  }
  body.appendChild(grid);
  group.appendChild(body);
  page.appendChild(group);
  return ctrls;
}

// --- Main page handler ---

function protocolsPage(tab, other, prev, $main, $pageHeader, mode) {
  $main.classList.add('page-body--flex-col', 'proto-page-body', 'proto-page', 'proto-page-shell', 'slab-shell', 'slab-shell--seamed');
  if (typeof mist.data.capabilities === 'undefined') {
    apiClient.send(function() { navto(tab); }, { capabilities: true });
    $main.append('Loading..');
    return;
  }

  setPageTitle($pageHeader, 'Protocols');

  let allCtrls = [];
  const page = $main;

  // Header buttons
  const enableDefaultsBtn = el('button', {
    onclick: function() {
      enableDefaults(refresh);
    }
  }, 'Enable default protocols');
  const disableAllBtn = el('button', {
    onclick: function() {
      if (confirm('Disable all configured protocols?')) {
        mist.data.config.protocols = [];
        apiClient.send(function() { refresh(); }, { config: mist.data.config });
      }
    }
  }, 'Disable all protocols');

  appendPageActions($pageHeader, [enableDefaultsBtn, disableAllBtn]);

  // Rich tooltips on header buttons
  enableDefaultsBtn.addEventListener('mouseenter', function(e) {
    const candidates = getDefaultEnableCandidates();
    let tip;
    if (!candidates.toEnable.length) {
      tip = 'All default protocols are already enabled.';
      if (candidates.skipped.length) {
        tip += ' Some protocols require manual setup.';
      }
    } else {
      tip = 'Enable all disabled protocols that support default settings.';
    }
    UI.tooltip.show(e, tip);
  });
  enableDefaultsBtn.addEventListener('mouseleave', function() {
    UI.tooltip.hide();
  });

  disableAllBtn.addEventListener('mouseenter', function(e) {
    const protos = mist.data.config.protocols || [];
    if (!protos.length) {
      UI.tooltip.show(e, 'No protocols are currently enabled.');
    }
  });
  disableAllBtn.addEventListener('mouseleave', function() {
    UI.tooltip.hide();
  });

  function updateHeaderButtons() {
    const candidates = getDefaultEnableCandidates();
    enableDefaultsBtn.disabled = !candidates.toEnable.length;
    disableAllBtn.disabled = !(mist.data.config.protocols || []).length;
  }

  function refresh() {
    apiClient.send(function() { buildPage(); });
  }

  const ctxMenu = new uiCore.context_menu();

  function buildPage() {
    page.innerHTML = '';
    page.appendChild(ctxMenu.ele);
    allCtrls = [];
    updateHeaderButtons();

    // Page intro
    page.appendChild(pageIntro('Protocols define how clients connect to '+APP_NAME+' - for playback, ingest, or both. Click any protocol to edit it, right-click for quick actions, and use the toggle to enable or disable. HTTP/HTTPS provide the foundation for browser-based delivery, while standalone protocols like RTMP, SRT, and RTSP listen on their own ports.'));

    const connectors = mist.data.capabilities.connectors;
    const groups = { http: null, https: null, 'http-based': [], streaming: [] };

    for (const connId in connectors) {
      if (isPushOnly(connId)) continue;
      const cat = classifyConnector(connId);
      if (cat === 'http') groups.http = connId;
      else if (cat === 'https') groups.https = connId;
      else if (cat === 'http-based') groups['http-based'].push(connId);
      else groups.streaming.push(connId);
    }

    const sortCards = function(a, b) {
      const aOn = findInstances(a).length > 0 ? 0 : 1;
      const bOn = findInstances(b).length > 0 ? 0 : 1;
      if (aOn !== bOn) return aOn - bOn;
      return getFriendly(a).localeCompare(getFriendly(b));
    };

    // Server group
    const serverProtos = [];
    if (groups.http) serverProtos.push(groups.http);
    if (groups.https) serverProtos.push(groups.https);
    allCtrls = allCtrls.concat(appendGroup(page, 'Server', serverProtos, refresh, mode, ctxMenu));

    // HTTP-based group
    groups['http-based'].sort(sortCards);
    allCtrls = allCtrls.concat(appendGroup(page, 'HTTP-based', groups['http-based'], refresh, mode, ctxMenu));

    // Streaming group
    groups.streaming.sort(sortCards);
    allCtrls = allCtrls.concat(appendGroup(page, 'Streaming', groups.streaming, refresh, mode, ctxMenu));
  }

  buildPage();
  UI.interval.set(function() {
    apiClient.send(function() {
      for (let i = 0; i < allCtrls.length; i++) {
        allCtrls[i].updateStatus();
      }
    });
  }, 10e3);
}

// --- Register with ModeDispatch ---

register('Protocols', {
  guided: function(tab, other, prev, $main, $pageHeader) {
    protocolsPage(tab, other, prev, $main, $pageHeader, 'guided');
  },
  advanced: function(tab, other, prev, $main, $pageHeader) {
    protocolsPage(tab, other, prev, $main, $pageHeader, 'advanced');
  }
});

registerTab('Edit Protocol', function(tab, other) {
  navto('Protocols');
});
