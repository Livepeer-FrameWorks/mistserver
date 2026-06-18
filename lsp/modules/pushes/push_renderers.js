import * as format from '../core/formatters.js';
import { pushScenarios } from './push_scenarios.js';
import { copy } from '../core/clipboard.js';
import { el } from '../core/dom_helpers.js';

const DEACTIVATION_MARKER = '\u{1F4A4}deactivated\u{1F4A4}_';

const STAT_LABELS = {
  pid: 'Pid: ',
  latency: 'Latency: ',
  active_ms: 'Active for: ',
  bytes: 'Data transferred: ',
  mediatime: 'Last sent timestamp:',
  media_tx: 'Media time transferred: ',
  mediaremaining: 'Media time until stream end: ',
  pkt_retrans_count: 'Packets retransmitted: ',
  pkt_loss_count: 'Packets lost: ',
  tracks: 'Tracks: '
};

const STAT_FORMATTERS = {
  pid: function(v) { return v; },
  latency: function(v) { return format.addUnit(format.number(v), 'ms'); },
  active_ms: function(v) { return format.duration(v * 1e-3); },
  bytes: format.bytes,
  mediatime: function(v) { return format.duration(v * 1e-3); },
  media_tx: function(v) { return format.duration(v * 1e-3); },
  mediatimestamp: function(v) { return format.duration(v * 1e-3); },
  pkt_retrans_count: function(v) { return format.number(v || 0); },
  pkt_loss_count: function(v, all) {
    return format.number(v || 0) + ' (' + format.addUnit(format.number(all.pkt_loss_perc || 0), '%') + ' over the last ' + format.addUnit(5, 's') + ')';
  },
  tracks: function(v) { return v.join(', '); }
};

function formatTargetCells(target) {
  const cont = el('div');
  const t = target.split('?');
  let params = [];
  if (t.length > 1) params = t.pop().split('&');
  const main = t.join('?');
  cont.appendChild(el('span', {title: target}, main));
  for (let i = 0; i < params.length; i++) {
    cont.appendChild(el('span', {class: 'param'}, (i === 0 ? '?' : '&') + params[i]));
  }
  return Array.from(cont.children);
}

function formatCondition(a, b, c) {
  const ps = pushScenarios;
  if (ps && ps.formatCondition) return ps.formatCondition(a, b, c);
  let str = '$' + a + ' ';
  switch (Number(b)) {
    case 0:  str += 'is true';  break;
    case 1:  str += 'is false'; break;
    case 2:  str += '== ' + c; break;
    case 3:  str += '!= ' + c; break;
    case 10: str += '> (numerical) ' + c; break;
    case 11: str += '>= (numerical) ' + c; break;
    case 12: str += '< (numerical) ' + c; break;
    case 13: str += '<= (numerical) ' + c; break;
    case 20: str += '> (lexical) ' + c; break;
    case 21: str += '>= (lexical) ' + c; break;
    case 22: str += '< (lexical) ' + c; break;
    case 23: str += '<= (lexical) ' + c; break;
    default: str += 'comparison operator unknown'; break;
  }
  return str;
}

function parseDeactivation(push) {
  let out = Object.assign({}, push);
  out.deactivated = out.stream.indexOf(DEACTIVATION_MARKER) === 0;
  if (out.deactivated) out.stream = out.stream.slice(DEACTIVATION_MARKER.length);
  return out;
}

function buildContextMenuHeader(push, targetChildren) {
  const $header = el('div', {class: 'header'});
  $header.appendChild(el('span', null, push.stream));
  $header.appendChild(el('span', {class: 'unit'}, ' \u00BB '));
  if (targetChildren) {
    const targetSpan = el('span', {class: 'target'});
    for (let i = 0; i < targetChildren.length; i++) {
      targetSpan.appendChild(targetChildren[i].cloneNode(true));
    }
    $header.appendChild(targetSpan);
  }
  if (push['x-LSP-notes']) {
    $header.appendChild(el('div', {class: 'description'}, push['x-LSP-notes']));
  }
  return $header;
}

function copyTarget(target, setText, hideMenu) {
  copy(target).then(function() {
    if (setText) setText('Copied!');
    if (hideMenu) setTimeout(hideMenu, 300);
  })['catch'](function() {
    if (setText) setText('Failed');
  });
  return false;
}

export const pushRenderers = {
  DEACTIVATION_MARKER: DEACTIVATION_MARKER,
  STAT_LABELS: STAT_LABELS,
  STAT_FORMATTERS: STAT_FORMATTERS,
  formatTargetCells: formatTargetCells,
  formatCondition: formatCondition,
  parseDeactivation: parseDeactivation,
  buildContextMenuHeader: buildContextMenuHeader,
  copyTarget: copyTarget
};
