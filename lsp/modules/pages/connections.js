import { registerTab } from '../core/tab_registry.js';
import { apiClient } from '../core/api_client.js';
import { navto } from '../core/navigation.js';
import { el } from '../core/dom_helpers.js';
import { formEngine } from '../core/form_engine.js';
import { uiCore } from '../core/ui_core.js';
import { dynamicUI } from '../core/dynamic.js';
import { copy } from '../core/clipboard.js';
import * as format from '../core/formatters.js';

const FIELDS = ['host', 'stream', 'protocol', 'sessid', 'tags', 'conntime',
                'down', 'downbps', 'position', 'pktcount', 'pktlost', 'pktretransmit'];
const COUNT_WARN = 100;
const POLL_MS = 5e3;

function loadFilter() {
  let f = {};
  try { f = JSON.parse(localStorage['connections_filter']) || {}; } catch (e) { f = {}; }
  return Object.assign({ sessids: [], streams: [], protocols: [], hosts: [] }, f);
}
function saveFilter(f) {
  try { localStorage['connections_filter'] = JSON.stringify(f); } catch (e) { /* ignore */ }
}

// The API compares protocols literally; expand connector names to cover all server-side labels.
function expandProtocols(protocols) {
  return protocols.reduce(function (acc, val) {
    switch (String(val).toUpperCase()) {
      case 'INPUT':
      case 'OUTPUT':
        acc.push(String(val).toUpperCase());
        break;
      default:
        acc.push('INPUT:' + val, 'OUTPUT:' + val, val);
        var ws = val + '/WS';
        acc.push('INPUT:' + ws, 'OUTPUT:' + ws, ws);
    }
    return acc;
  }, []);
}

function connType(protocol) {
  var head = String(protocol || '').split(':')[0];
  if (head === 'INPUT') { return 'Input'; }
  if (head === 'OUTPUT') { return 'Output'; }
  return 'Viewer';
}

function cleanProtocol(protocol) {
  return String(protocol || '').replace('INPUT:', '').replace('OUTPUT:', '');
}

function clickToCopy(text) {
  var span = el('span', { class: 'clickable overflow_ellipsis', title: text }, text);
  span.addEventListener('click', function () {
    copy(text);
    var orig = span.textContent;
    span.textContent = 'Copied!';
    setTimeout(function () { span.textContent = orig; }, 1e3);
  });
  return span;
}

function tagsCell(d) {
  if (!Array.isArray(d.tags) || !d.tags.length) { return ''; }
  return el('span', { class: 'tags' }, d.tags.map(function (t) {
    return el('span', { class: 'tag overflow_ellipsis', title: t }, t);
  }));
}

const COLUMNS = [
  { label: 'Host', node: true, sort: function (d) { return d.host === '::' ? '' : d.host; }, build: function (d) { return d.host === '::' ? '' : clickToCopy(d.host); } },
  { label: 'Stream', node: true, sort: function (d) { return d.stream; }, build: function (d) { return el('span', { class: 'clickable', onclick: function () { navto('Status', d.stream); } }, d.stream); } },
  { label: 'Type', sort: function (d) { return connType(d.protocol); }, build: function (d) { return connType(d.protocol); } },
  { label: 'Protocol', sort: function (d) { return cleanProtocol(d.protocol); }, build: function (d) { return cleanProtocol(d.protocol); } },
  { label: 'Session Id', node: true, sort: function (d) { return d.sessid; }, build: function (d) { return clickToCopy(d.sessid); } },
  { label: 'Tags', node: true, sort: function (d) { return (d.tags || []).join(','); }, build: tagsCell },
  { label: 'Connected', numeric: true, sort: function (d) { return Number(d.conntime) || 0; }, build: function (d) { return format.duration(d.conntime); } },
  { label: 'Data downloaded', numeric: true, node: true, sort: function (d) { return Number(d.down) || 0; }, build: function (d) { return format.bytes(d.down); } },
  { label: 'Current bitrate', numeric: true, node: true, sort: function (d) { return Number(d.downbps) || 0; }, build: function (d) { return format.bits((Number(d.downbps) || 0) * 8, true); } },
  { label: 'Media time', numeric: true, sort: function (d) { return Number(d.position) || 0; }, build: function (d) { return d.position ? format.duration(d.position) : ''; } },
  { label: 'Packets received', numeric: true, sort: function (d) { return Number(d.pktcount) || 0; }, build: function (d) { return d.pktcount ? String(format.number(d.pktcount, { round: false })) : ''; } },
  { label: 'Packets lost', numeric: true, sort: function (d) { return Number(d.pktlost) || 0; }, build: function (d) { return d.pktcount ? String(format.number(d.pktlost, { round: false })) : ''; } },
  { label: 'Packets retransmitted', numeric: true, sort: function (d) { return Number(d.pktretransmit) || 0; }, build: function (d) { return d.pktcount ? String(format.number(d.pktretransmit, { round: false })) : ''; } }
];

function attachRowMenu(tr, contextMenu) {
  tr.addEventListener('contextmenu', function (e) {
    e.preventDefault();
    var d = tr._data;
    if (!d) { return; }
    contextMenu.show([
      [el('div', { class: 'header' }, [
        el('span', null, ['Session', d.protocol, '(' + d.stream + ')'].join(' ')),
        el('span', { class: 'description overflow_ellipsis', title: d.sessid }, d.sessid),
        el('span', { class: 'description' }, d.host)
      ])],
      [
        ['Copy session id', function () { copy(d.sessid); this._setText('Copied!'); setTimeout(function () { contextMenu.hide(); }, 300); return false; }, 'copy', 'Copy this session id to the clipboard.'],
        d.host === '::' ? null : ['Copy host', function () { copy(d.host); this._setText('Copied!'); setTimeout(function () { contextMenu.hide(); }, 300); return false; }, 'copy', 'Copy this host to the clipboard.'],
        ['View stream', function () { navto('Status', d.stream); }, '', 'Go to this stream\'s status page.']
      ],
      [
        ['Stop session', function () {
          var me = this;
          if (!confirm('Are you sure you want to stop this session?\nA stopped session will disconnect any currently open connections, and, if the USER_NEW trigger is in use, prevent new connections from opening for at least ten seconds.')) { return false; }
          apiClient.send(function () { me._setText('Stopped!'); setTimeout(function () { contextMenu.hide(); }, 300); }, { stop_sessid: [d.sessid] });
          return false;
        }, '', 'Disconnect this session\'s open connections.'],
        ['Invalidate session', function () {
          var me = this;
          if (!confirm('Are you sure you want to invalidate this session?\nThis will re-trigger the USER_NEW trigger (if in use), allowing you to close the existing connections after they have been previously allowed.')) { return false; }
          apiClient.send(function () { me._setText('Invalidated!'); setTimeout(function () { contextMenu.hide(); }, 300); }, { invalidate_sessid: [d.sessid] });
          return false;
        }, '', 'Re-trigger USER_NEW for this session.']
      ]
    ], e);
  });
}

function buildClientsTable(contextMenu) {
  return dynamicUI.dynamic({
    create: function () {
      var headerRow = el('tr');
      var table = el('table', { class: 'connections-table' }, [el('thead', null, headerRow), el('tbody')]);
      COLUMNS.forEach(function (col) {
        headerRow.appendChild(el('th', { class: col.numeric ? 'header numeric' : 'header', 'data-index': col.label }, col.label));
      });
      uiCore.sortableItems(table, function (sortby) {
        return this._sortvals ? this._sortvals[sortby] : '';
      }, {
        controls: headerRow,
        sortby: 'Connected',
        sortdir: -1,
        sortsave: 'sort_connections',
        container: table.children[1]
      });
      return table;
    },
    getEntries: function (d) { return d; },
    add: {
      create: function (id) {
        var tr = el('tr', { 'data-sessid': id });
        tr._cells = {};
        tr._keys = {};
        tr._sortvals = {};
        COLUMNS.forEach(function (col) {
          var td = el('td', { 'data-index': col.label, class: col.numeric ? 'numeric' : null });
          tr._cells[col.label] = td;
          tr.appendChild(td);
        });
        attachRowMenu(tr, contextMenu);
        return tr;
      },
      update: function (d) {
        var tr = this;
        tr._data = d;
        COLUMNS.forEach(function (col) {
          tr._sortvals[col.label] = col.sort(d);
          var cell = tr._cells[col.label];
          // Node columns key on their sort value (display derives from it);
          // text columns key on the rendered string so display changes update.
          var key = col.node ? String(col.sort(d)) : String(col.build(d));
          if (tr._keys[col.label] === key) { return; }
          tr._keys[col.label] = key;
          var built = col.build(d);
          if (built instanceof Node) { cell.replaceChildren(built); }
          else { cell.textContent = (built == null ? '' : String(built)); }
        });
      }
    },
    update: function () {
      if (this.sort) { this.sort(); }
    }
  });
}

function renderConnections(tab, other, prev, $main) {
  $main.classList.add('page-body--flex-col', 'connections-page-body', 'connections-shell', 'slab-shell', 'slab-shell--seamed');

  var filter = loadFilter();
  var connectors = (mist.data && mist.data.capabilities && mist.data.capabilities.connectors) || {};
  var protoHelp = 'Match by protocol. Use "INPUT"/"OUTPUT" for any input/output, or a '
    + 'connector name (e.g. ' + (Object.keys(connectors).slice(0, 3).join(', ') || 'HTTP, RTMP') + ').';

  $main.appendChild(formEngine.buildUI([{
    type: 'help',
    classes: ['page-intro'],
    help: 'Filter by session, stream, protocol, or host. Click a column header to sort, drag its edge to resize, and right-click a row to inspect, copy, or stop a session.'
  }]));

  var $form = el('div', { class: 'connections-filter' });
  var $result = el('div', { class: 'connections-result' });
  $main.appendChild($form);
  $main.appendChild($result);

  var contextMenu = new uiCore.context_menu();
  $main.appendChild(contextMenu.ele);

  var $message = el('p', { class: 'description' });
  var $scroll = el('div', { class: 'table-scroll' });
  var table = buildClientsTable(contextMenu);
  $scroll.appendChild(table);
  $result.appendChild($message);
  $result.appendChild($scroll);

  function showMessage(text) {
    $message.textContent = text;
    $message.hidden = false;
    $scroll.hidden = true;
  }
  function showTable() {
    $message.hidden = true;
    $scroll.hidden = false;
  }

  // Keep a single poller for the page lifetime so Apply only updates the request shape.
  var state = { subscription: null, active: false };

  function renderTable() {
    var cd = mist.data && mist.data.clients && mist.data.clients[0];
    var rows = (cd && Array.isArray(cd.data)) ? cd.data : [];
    var fields = (cd && cd.fields) || [];

    if (!rows.length) {
      table.update({});
      showMessage('No connections match the current filters.');
      return;
    }

    var byId = {};
    rows.forEach(function (row) {
      var d = {};
      for (var k = 0; k < fields.length; k++) { d[fields[k]] = row[k]; }
      byId[d.sessid] = d;
    });
    showTable();
    table.update(byId);
  }

  function poll() {
    if (!state.active || !state.subscription) { return; }
    var req = Object.assign({ fields: FIELDS, time: -3 }, state.subscription);
    apiClient.send(renderTable, { clients: [req] }, { hide: true });
  }

  function applyFilter(isInitial) {
    saveFilter(filter);
    var subscription = {};
    if (filter.sessids.length) { subscription.sessids = filter.sessids; }
    if (filter.streams.length) { subscription.streams = filter.streams; }
    if (filter.hosts.length) { subscription.hosts = filter.hosts; }
    var protos = expandProtocols(filter.protocols);
    if (protos.length) { subscription.protocols = protos; }

    showMessage('Loading...');

    // Avoid fetching large client lists before the user narrows a broad filter.
    var countReq = { fields: ['clients', 'inputs', 'outputs'], start: -3, end: -3 };
    if (subscription.sessids) { countReq.sessids = subscription.sessids; }
    if (subscription.streams) { countReq.streams = subscription.streams; }
    if (subscription.protocols) { countReq.protocols = subscription.protocols; }
    if (subscription.hosts) { countReq.hosts = subscription.hosts; }

    apiClient.send(function (d) {
      var amount = filter.sessids.length;
      if (!amount) {
        var t = d && d.totals;
        var last = t && Array.isArray(t.data) && t.data.length ? t.data[t.data.length - 1] : null;
        if (Array.isArray(last)) { amount = last.reduce(function (a, v) { return a + (Number(v) || 0); }, 0); }
      }
      if (amount > COUNT_WARN) {
        showMessage('Your current filters would yield ' + format.number(amount)
          + ' results. Please narrow them down; we recommend bringing the amount below ' + COUNT_WARN + '.');
        if (isInitial) { state.active = false; return; }
        if (!confirm('Your current filters would yield ' + amount + ' results. Are you sure you want to query all of them?')) { state.active = false; return; }
      }
      state.subscription = subscription;
      state.active = true;
      poll();
    }, { totals: countReq }, { hide: true });
  }

  function field(label, index, help) {
    return { type: 'inputlist', label: label, help: help, pointer: { main: filter, index: index } };
  }
  $form.appendChild(formEngine.buildUI([
    el('h3', null, 'Filter connections'),
    field('Session id', 'sessids', 'Only show these session ids.'),
    field('Stream', 'streams', 'Only show connections of these streams.'),
    field('Protocol', 'protocols', protoHelp),
    field('Host', 'hosts', 'Only show connections with these hosts.'),
    { type: 'buttons', buttons: [{ type: 'save', label: 'Apply', 'function': function () { applyFilter(false); } }] }
  ]));

  UI.interval.set(poll, POLL_MS);
  applyFilter(true);
}

registerTab('Connections', renderConnections);
