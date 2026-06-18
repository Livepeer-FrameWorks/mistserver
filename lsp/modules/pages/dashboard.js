import { APP_NAME } from '@brand';
import { Chart } from 'chart.js';
import { registerTab } from '../core/tab_registry.js';
import { ChartHelpers } from './chart_helpers.js';
import { MistIcons } from '../core/icons.js';
import { uiHelpers } from '../core/ui_helpers.js';
import * as format from '../core/formatters.js';
import { apiClient } from '../core/api_client.js';
import { navto } from '../core/navigation.js';
import { mistHelpers } from '../core/mist_helpers.js';
import { el, deepExtend, getval } from '../core/dom_helpers.js';
import { formEngine } from '../core/form_engine.js';
import { cattablesort } from '../../plugins/cattablesort.js';
import { defineSection } from '../core/section_registry.js';

defineSection({
  id: 'statistics/server-overview',
  page: 'Statistics',
  title: 'Server Overview',
  subtitle: 'CPU, memory, bandwidth, and viewer charts',
  keywords: ['cpu', 'memory', 'load average', 'bandwidth', 'viewers', 'resources', 'swap', 'processor', 'KPI'],
  requires: ['capabilities'],
  navTo: { tab: 'Statistics', other: '' }
});

defineSection({
  id: 'statistics/per-stream',
  page: 'Statistics',
  title: 'Per-Stream Statistics',
  subtitle: 'Viewers, bandwidth, and packet loss per stream',
  keywords: ['stream stats', 'packet loss', 'retransmission', 'viewers per stream', 'stream detail'],
  requires: ['active_streams'],
  navTo: { tab: 'Statistics', other: '' }
});

defineSection({
  id: 'statistics/viewer-analytics',
  page: 'Statistics',
  title: 'Viewer Analytics',
  subtitle: 'Active viewers table and protocol breakdown',
  keywords: ['active viewers', 'client list', 'protocol breakdown', 'connections', 'host IP', 'bandwidth by protocol'],
  requires: ['totals'],
  navTo: { tab: 'Statistics', other: '' }
});

defineSection({
  id: 'statistics/custom-graphs',
  page: 'Statistics',
  title: 'Custom Graphs',
  subtitle: 'User-defined charts and data visualization',
  keywords: ['custom chart', 'graph', 'data origin', 'multi-axis', 'saved graphs'],
  requires: ['capabilities'],
  navTo: { tab: 'Statistics', other: '' }
});

// ---- History buffers (persist across tab switches within a session) ----
const history = {
  cpu: [],
  memory: [],
  maxAge: 30 * 60 * 1000
};

function bufferSnapshot() {
  if (!mist.data.capabilities) { return; }
  const now = (mist.data.config.time || Date.now() / 1000) * 1000;
  const cpuVal = (mist.data.capabilities.cpu_use || 0) / 10;
  const memVal = mist.data.capabilities.load ? (mist.data.capabilities.load.memory || 0) : 0;

  history.cpu.push({x: now, y: cpuVal});
  history.memory.push({x: now, y: memVal});

  const cutoff = now - history.maxAge;
  while (history.cpu.length && history.cpu[0].x < cutoff) { history.cpu.shift(); }
  while (history.memory.length && history.memory[0].x < cutoff) { history.memory.shift(); }
}

// ---- Shared helpers ----

function toNumber(v) { const n = Number(v); return isFinite(n) ? n : 0; }

function escapeHTML(value) {
  const tmp = document.createElement('div');
  tmp.textContent = (value === undefined || value === null) ? '' : String(value);
  return tmp.innerHTML;
}

function asArray(value) {
  if (Array.isArray(value)) { return value; }
  if (!value || typeof value !== 'object') { return []; }
  const out = [];
  for (const k in value) { out.push(value[k]); }
  return out;
}

function getStreamList() {
  const streams = {};
  for (const i in mist.data.streams) { streams[i] = true; }
  if (mist.data.active_streams) {
    for (let j = 0; j < mist.data.active_streams.length; j++) {
      streams[mist.data.active_streams[j]] = true;
    }
  }
  return Object.keys(streams).sort();
}

function getProtocolList() {
  const protos = [];
  if (mist.data.config && mist.data.config.protocols) {
    for (let i = 0; i < mist.data.config.protocols.length; i++) {
      const c = mist.data.config.protocols[i].connector;
      if (protos.indexOf(c) === -1) { protos.push(c); }
    }
  }
  return protos.sort();
}

const statCard = uiHelpers.statCard;
const infoRow = uiHelpers.infoRow;
const unavailableText = uiHelpers.unavailableText;
const pageIntro = uiHelpers.pageIntro;

function unitValue(value, unit) {
  const out = el('span');
  out.innerHTML = format.addUnit(value, unit);
  return out;
}

function appendBracketedUnit(target, value, unit) {
  target.appendChild(document.createTextNode(' ('));
  target.appendChild(unitValue(value, unit));
  target.appendChild(document.createTextNode(')'));
}

function createBar(total, used, cached) {
  if (!(total > 0)) { return ''; }
  const usedPerc = Math.max(0, Math.min(100, used / total * 100));
  const cachedPerc = cached ? Math.max(0, Math.min(100, cached / total * 100)) : 0;
  const bar = el('div', {class: 'bargraph'});
  const usedSpan = el('span', {class: 'used', title: 'Used'}, Math.round(usedPerc) + '%');
  usedSpan.style.width = usedPerc + '%';
  bar.appendChild(usedSpan);
  if (cached) {
    const cachedSpan = el('span', {class: 'cached', title: 'Cached'});
    cachedSpan.style.width = cachedPerc + '%';
    bar.appendChild(cachedSpan);
  }
  return bar;
}

// ---- Sub-navigation builder ----

function buildSubNav(views, activeKey, onChange) {
  const nav = el('div', {class: 'dashboard-subnav'});
  for (let i = 0; i < views.length; i++) {
    const view = views[i];
    const btn = el('button', {class: 'subnav-btn'}, view.label);
    btn._key = view.key;
    if (view.key === activeKey) { btn.classList.add('active'); }
    btn.addEventListener('click', function() {
      const btns = nav.querySelectorAll('.subnav-btn');
      for (let b = 0; b < btns.length; b++) { btns[b].classList.remove('active'); }
      this.classList.add('active');
      onChange(this._key);
    });
    nav.appendChild(btn);
  }
  return nav;
}

function createStatsView(className) {
  return el('div', {class: 'stats-view ' + className});
}

function createStatsBlock(className) {
  return el('section', {class: 'stats-block ' + className});
}

// ==============================================================
// SERVER OVERVIEW
// ==============================================================

function renderOverview(container) {
  ChartHelpers.destroyAll();
  container.innerHTML = '';
  const view = createStatsView('stats-view-overview');

  const cpuVal = el('span', {class: 'stat-value'});
  const memVal = el('span', {class: 'stat-value'});
  const viewerVal = el('span', {class: 'stat-value'});
  const bwVal = el('span', {class: 'stat-value'});

  const stats = el('div', {class: 'overview-stats'});
  stats.appendChild(statCard('cpu', cpuVal, 'CPU usage'));
  stats.appendChild(statCard('memory-stick', memVal, 'Memory usage'));
  stats.appendChild(statCard('users', viewerVal, 'Viewers'));
  stats.appendChild(statCard('activity', bwVal, 'Bandwidth'));
  const statsBlock = createStatsBlock('stats-block-kpis');
  statsBlock.appendChild(stats);

  // Charts
  const cpuMem = ChartHelpers.chartCard('CPU & Memory', 260);
  const connBw = ChartHelpers.chartCard('Connections & Bandwidth', 260);
  cpuMem.$card.classList.add('stats-chart-card');
  connBw.$card.classList.add('stats-chart-card');
  const chartRow = el('div', {class: 'chart-grid chart-grid-2'});
  chartRow.appendChild(cpuMem.$card);
  chartRow.appendChild(connBw.$card);
  const chartsBlock = createStatsBlock('stats-block-charts');
  chartsBlock.appendChild(chartRow);

  // System details
  const resourceBody = el('div');
  const resources = el('section', {class: 'overview-section'});
  resources.appendChild(el('h3', null, 'Resources'));
  resources.appendChild(resourceBody);

  const loadBody = el('div');
  const loadSection = el('section', {class: 'overview-section'});
  loadSection.appendChild(el('h3', null, 'Load'));
  loadSection.appendChild(loadBody);

  const cpuBody = el('div');
  const cpuSection = el('section', {class: 'overview-section'});
  cpuSection.appendChild(el('h3', null, 'Processors'));
  cpuSection.appendChild(cpuBody);

  const innerGrid = el('div', {class: 'overview-grid'});
  innerGrid.appendChild(loadSection);
  innerGrid.appendChild(cpuSection);
  const detailGrid = el('div', {class: 'overview-grid'});
  detailGrid.appendChild(resources);
  detailGrid.appendChild(innerGrid);

  const detailsBlock = createStatsBlock('stats-block-details');
  detailsBlock.appendChild(detailGrid);

  view.appendChild(statsBlock);
  view.appendChild(chartsBlock);
  view.appendChild(detailsBlock);
  container.appendChild(view);

  // Init lucide icons
  MistIcons.init(view);

  ChartHelpers.applyDefaults();
  const C = Chart;
  const c = ChartHelpers.getThemeColors();

  // CPU/Memory chart
  const cpuMemChart = new C(cpuMem.canvas, {
    type: 'line',
    data: {
      datasets: [
        ChartHelpers.lineDataset('CPU', history.cpu.slice(), {color: c.orange, yAxisID: 'y'}),
        ChartHelpers.lineDataset('Memory', history.memory.slice(), {color: c.accent, yAxisID: 'y'})
      ]
    },
    options: {
      interaction: { mode: 'index', intersect: false },
      scales: {
        x: ChartHelpers.createTimeScale(),
        y: ChartHelpers.createLinearScale({title: '%', formatter: ChartHelpers.fmtPercent, max: 100})
      },
      plugins: { legend: { position: 'top', labels: { boxWidth: 12, padding: 8, color: c.text, font: { size: 11 } } } }
    }
  });
  ChartHelpers.register(cpuMemChart);

  // Connections/Bandwidth chart
  const connBwChart = new C(connBw.canvas, {
    type: 'line',
    data: {
      datasets: [
        ChartHelpers.lineDataset('Viewers', [], {color: c.accent, yAxisID: 'y'}),
        ChartHelpers.lineDataset('Bandwidth (down)', [], {color: c.green, yAxisID: 'y1'}),
        ChartHelpers.lineDataset('Bandwidth (up)', [], {color: c.orange, yAxisID: 'y1'})
      ]
    },
    options: {
      interaction: { mode: 'index', intersect: false },
      scales: {
        x: ChartHelpers.createTimeScale(),
        y:  ChartHelpers.createLinearScale({title: 'Viewers', formatter: ChartHelpers.fmtNumber, position: 'left'}),
        y1: ChartHelpers.createLinearScale({title: 'Bandwidth', formatter: ChartHelpers.fmtBytesPerSec, position: 'right', drawGrid: false})
      },
      plugins: { legend: { position: 'top', labels: { boxWidth: 12, padding: 8, color: c.text, font: { size: 11 } } } }
    }
  });
  ChartHelpers.register(connBwChart);

  function updateSystemDetails() {
    const caps = mist.data.capabilities || {};
    const mem = Object.assign({total: 0, used: 0, free: 0, cached: 0, swapfree: 0, swaptotal: 0, shmfree: 0, shmtotal: 0}, caps.mem || {});
    const load = Object.assign({one: 0, five: 0, fifteen: 0, memory: 0, shm: 0}, caps.load || {});
    const cpus = asArray(caps.cpu);

    for (const k in mem) { mem[k] = toNumber(mem[k]); }
    for (const k2 in load) { load[k2] = toNumber(load[k2]); }

    const cpuUse = toNumber(caps.cpu_use);
    const hasCpu = cpus.length > 0 || cpuUse > 0;
    const hasMem = mem.total > 0;
    const hasLoad = cpus.length > 0 || load.one > 0;

    cpuVal.textContent = hasCpu ? (format.number(cpuUse / 10) + '%') : 'N/A';
    memVal.innerHTML = '';
    if (hasMem) {
      memVal.appendChild(unitValue(load.memory, '%'));
    } else {
      memVal.textContent = 'N/A';
    }

    // Viewers and bandwidth from totals
    let viewers = 0;
    let downBps = 0;
    let upBps = 0;
    if (mist.data.totals && mist.data.totals.all_streams && mist.data.totals.all_streams.all_protocols) {
      const t = mist.data.totals.all_streams.all_protocols;
      if (t.clients && t.clients.length) { viewers = t.clients[t.clients.length - 1][1]; }
      if (t.downbps && t.downbps.length) { downBps = t.downbps[t.downbps.length - 1][1]; }
      if (t.upbps && t.upbps.length) { upBps = t.upbps[t.upbps.length - 1][1]; }
    }
    viewerVal.textContent = format.number(viewers);
    bwVal.textContent = '';
    bwVal.appendChild(format.bits((downBps + upBps) * 8, true));

    // Resources
    resourceBody.innerHTML = '';
    if (hasMem) {
      const phys = el('div', {class: 'resource-group'});
      phys.appendChild(el('h4', null, 'Physical memory'));
      phys.appendChild(createBar(mem.total, mem.used, mem.cached));
      const usedSpan = el('span');
      usedSpan.appendChild(format.bytes(mem.used * 1024 * 1024));
      appendBracketedUnit(usedSpan, load.memory, '%');
      phys.appendChild(infoRow('Used', usedSpan));
      phys.appendChild(infoRow('Available', format.bytes(mem.free * 1024 * 1024)));
      phys.appendChild(infoRow('Cached', format.bytes(mem.cached * 1024 * 1024)));
      phys.appendChild(infoRow('Total', format.bytes(mem.total * 1024 * 1024)));
      resourceBody.appendChild(phys);
    } else {
      const noMem = el('div', {class: 'resource-group'});
      noMem.appendChild(el('h4', null, 'Physical memory'));
      noMem.appendChild(unavailableText('Not available on this system'));
      resourceBody.appendChild(noMem);
    }
    if (mem.shmtotal > 0) {
      const shmUsed = mem.shmtotal - mem.shmfree;
      const shm = el('div', {class: 'resource-group'});
      shm.appendChild(el('h4', null, 'Shared memory'));
      shm.appendChild(createBar(mem.shmtotal, shmUsed));
      const shmUsedSpan = el('span');
      shmUsedSpan.appendChild(format.bytes(shmUsed * 1024 * 1024));
      appendBracketedUnit(shmUsedSpan, load.shm, '%');
      shm.appendChild(infoRow('Used', shmUsedSpan));
      shm.appendChild(infoRow('Available', format.bytes(mem.shmfree * 1024 * 1024)));
      shm.appendChild(infoRow('Total', format.bytes(mem.shmtotal * 1024 * 1024)));
      resourceBody.appendChild(shm);
    }
    if (mem.swaptotal > 0) {
      const swapUsed = mem.swaptotal - mem.swapfree;
      const swap = el('div', {class: 'resource-group'});
      swap.appendChild(el('h4', null, 'Swap'));
      swap.appendChild(createBar(mem.swaptotal, swapUsed));
      swap.appendChild(infoRow('Used', format.bytes(swapUsed * 1024 * 1024)));
      swap.appendChild(infoRow('Available', format.bytes(mem.swapfree * 1024 * 1024)));
      swap.appendChild(infoRow('Total', format.bytes(mem.swaptotal * 1024 * 1024)));
      resourceBody.appendChild(swap);
    }

    // Load
    loadBody.innerHTML = '';
    loadBody.appendChild(infoRow('CPU use', hasCpu ? unitValue(format.number(cpuUse / 10), '%') : unavailableText()));
    loadBody.appendChild(infoRow('1 minute', hasLoad ? format.number(load.one / 100) : unavailableText()));
    loadBody.appendChild(infoRow('5 minutes', hasLoad ? format.number(load.five / 100) : unavailableText()));
    loadBody.appendChild(infoRow('15 minutes', hasLoad ? format.number(load.fifteen / 100) : unavailableText()));

    // CPUs
    cpuBody.innerHTML = '';
    if (cpus.length) {
      for (let ci = 0; ci < cpus.length; ci++) {
        const cpu = cpus[ci] || {};
        const grp = el('div', {class: 'resource-group'});
        if (cpus.length > 1) { grp.appendChild(el('h4', null, 'CPU #' + (ci + 1))); }
        grp.appendChild(infoRow('Model', cpu.model || unavailableText()));
        grp.appendChild(infoRow('Speed', ('mhz' in cpu) ? unitValue(format.number(cpu.mhz), 'MHz') : unavailableText()));
        grp.appendChild(infoRow('Cores', ('cores' in cpu) ? cpu.cores : unavailableText()));
        grp.appendChild(infoRow('Threads', ('threads' in cpu) ? cpu.threads : unavailableText()));
        cpuBody.appendChild(grp);
      }
    } else {
      cpuBody.appendChild(unavailableText('Processor details are not available on this system'));
    }
  }

  function updateCharts() {
    bufferSnapshot();

    // CPU/Memory chart
    cpuMemChart.data.datasets[0].data = history.cpu.slice();
    cpuMemChart.data.datasets[1].data = history.memory.slice();
    cpuMemChart.update('none');

    // Connections/Bandwidth
    if (mist.data.totals && mist.data.totals.all_streams && mist.data.totals.all_streams.all_protocols) {
      const t = mist.data.totals.all_streams.all_protocols;
      connBwChart.data.datasets[0].data = ChartHelpers.mistToXY(t.clients);
      connBwChart.data.datasets[1].data = ChartHelpers.mistToXY(t.downbps);
      connBwChart.data.datasets[2].data = ChartHelpers.mistToXY(t.upbps);
      connBwChart.update('none');
    }

    updateSystemDetails();
  }

  function poll() {
    apiClient.send(function() { updateCharts(); }, {
      capabilities: true,
      totals: [{fields: ['clients', 'upbps', 'downbps'], end: -15}],
      active_streams: true
    });
  }

  poll();
  UI.interval.set(poll, 10e3);
}

// ==============================================================
// PER-STREAM DETAIL
// ==============================================================

function renderStreamDetail(container) {
  ChartHelpers.destroyAll();
  container.innerHTML = '';
  const view = createStatsView('stats-view-stream');

  let selectedStream = '';
  const controls = el('div', {class: 'dashboard-controls'});
  const streamPicker = uiHelpers.createSmartSelect({
    label: 'Stream',
    emptyLabel: 'Select a stream...',
    searchPlaceholder: 'Type to filter streams...',
    quickLabel: 'Top streams by viewers',
    maxQuickPicks: 5,
    onChange: function(value) {
      selectedStream = value;
      updateStreamInfo();
      if (selectedStream) {
        poll();
      } else {
        clearCharts();
      }
    }
  });
  controls.appendChild(streamPicker.element);

  // Stream info panel
  const info = el('div', {class: 'dashboard-stream-info stats-stream-summary'});

  // Chart placeholders
  const viewersCard = ChartHelpers.chartCard('Viewers', 220);
  const bwCard = ChartHelpers.chartCard('Bandwidth', 220);
  const pktCard = ChartHelpers.chartCard('Packet Loss & Retransmission', 220);
  viewersCard.$card.classList.add('stats-chart-card');
  bwCard.$card.classList.add('stats-chart-card');
  pktCard.$card.classList.add('stats-chart-card');

  const chartRow1 = el('div', {class: 'chart-grid chart-grid-2'});
  chartRow1.appendChild(viewersCard.$card);
  chartRow1.appendChild(bwCard.$card);
  const chartRow2 = el('div', {class: 'chart-grid chart-grid-1'});
  chartRow2.appendChild(pktCard.$card);

  const controlsBlock = createStatsBlock('stats-block-controls');
  controlsBlock.appendChild(controls);
  const summaryBlock = createStatsBlock('stats-block-summary');
  summaryBlock.appendChild(info);
  const chartsMainBlock = createStatsBlock('stats-block-charts');
  chartsMainBlock.appendChild(chartRow1);
  const chartsAuxBlock = createStatsBlock('stats-block-charts-aux');
  chartsAuxBlock.appendChild(chartRow2);

  view.appendChild(controlsBlock);
  view.appendChild(summaryBlock);
  view.appendChild(chartsMainBlock);
  view.appendChild(chartsAuxBlock);
  container.appendChild(view);

  ChartHelpers.applyDefaults();
  const C = Chart;
  const c = ChartHelpers.getThemeColors();

  const viewersChart = new C(viewersCard.canvas, {
    type: 'line',
    data: { datasets: [ChartHelpers.lineDataset('Viewers', [], {color: c.accent})] },
    options: {
      interaction: { mode: 'index', intersect: false },
      scales: {
        x: ChartHelpers.createTimeScale(),
        y: ChartHelpers.createLinearScale({title: 'Viewers', formatter: ChartHelpers.fmtNumber})
      },
      plugins: { legend: { display: false } }
    }
  });
  ChartHelpers.register(viewersChart);

  const bwChart = new C(bwCard.canvas, {
    type: 'line',
    data: {
      datasets: [
        ChartHelpers.lineDataset('Download', [], {color: c.green}),
        ChartHelpers.lineDataset('Upload', [], {color: c.orange})
      ]
    },
    options: {
      interaction: { mode: 'index', intersect: false },
      scales: {
        x: ChartHelpers.createTimeScale(),
        y: ChartHelpers.createLinearScale({title: 'Bandwidth', formatter: ChartHelpers.fmtBytesPerSec})
      },
      plugins: { legend: { position: 'top', labels: { boxWidth: 12, padding: 8, color: c.text, font: { size: 11 } } } }
    }
  });
  ChartHelpers.register(bwChart);

  const pktChart = new C(pktCard.canvas, {
    type: 'line',
    data: {
      datasets: [
        ChartHelpers.lineDataset('Lost', [], {color: c.red}),
        ChartHelpers.lineDataset('Retransmitted', [], {color: c.orange})
      ]
    },
    options: {
      interaction: { mode: 'index', intersect: false },
      scales: {
        x: ChartHelpers.createTimeScale(),
        y: ChartHelpers.createLinearScale({title: '%', formatter: ChartHelpers.fmtPercent, max: 100})
      },
      plugins: { legend: { position: 'top', labels: { boxWidth: 12, padding: 8, color: c.text, font: { size: 11 } } } }
    }
  });
  ChartHelpers.register(pktChart);

  function getViewerCount(streamName) {
    if (!mist.data.totals || !mist.data.totals[streamName] || !mist.data.totals[streamName].all_protocols) {
      return 0;
    }
    const clients = mist.data.totals[streamName].all_protocols.clients;
    if (!clients || !clients.length) { return 0; }
    return toNumber(clients[clients.length - 1][1]);
  }

  function refreshStreamPicker() {
    const streams = getStreamList();
    const options = [];
    for (let i = 0; i < streams.length; i++) {
      const name = streams[i];
      options.push({
        value: name,
        label: name,
        search: name + ' ' + getViewerCount(name)
      });
    }
    streamPicker.setOptions(options);

    if (selectedStream && streams.indexOf(selectedStream) < 0) {
      selectedStream = '';
      streamPicker.setValue('', {silent: true});
      clearCharts();
    } else {
      streamPicker.setValue(selectedStream, {silent: true});
    }

    const ranked = [];
    for (let j = 0; j < streams.length; j++) {
      ranked.push({value: streams[j], viewers: getViewerCount(streams[j])});
    }
    ranked.sort(function(a, b) {
      if (a.viewers !== b.viewers) { return b.viewers - a.viewers; }
      return a.value.localeCompare(b.value);
    });

    let top = [];
    for (let k = 0; k < ranked.length; k++) {
      if (ranked[k].viewers > 0) { top.push(ranked[k]); }
    }
    if (!top.length) {
      top = ranked.slice(0, 5);
    } else if (top.length > 5) {
      top = top.slice(0, 5);
    }

    const quickPicks = [];
    for (let q = 0; q < top.length; q++) {
      const entry = top[q];
      let label = entry.value;
      if (entry.viewers > 0) {
        label += ' \u00b7 ' + format.number(entry.viewers);
      }
      quickPicks.push({
        value: entry.value,
        label: label,
        title: entry.viewers === 1 ? '1 viewer' : format.number(entry.viewers) + ' viewers'
      });
    }
    streamPicker.setQuickPicks(quickPicks);
  }

  function updateStreamInfo() {
    info.innerHTML = '';
    if (!selectedStream) { return; }
    if (mist.data.active_streams && mist.data.active_streams.indexOf(selectedStream) > -1) {
      info.appendChild(el('span', {class: 'cam-pill enabled'}, 'Online'));
    } else {
      info.appendChild(el('span', {class: 'cam-pill disabled'}, 'Offline'));
    }
  }

  function updateCharts() {
    if (!selectedStream) { return; }
    const streamKey = selectedStream;
    const protoKey = 'all_protocols';
    if (mist.data.totals && mist.data.totals[streamKey] && mist.data.totals[streamKey][protoKey]) {
      const t = mist.data.totals[streamKey][protoKey];
      viewersChart.data.datasets[0].data = ChartHelpers.mistToXY(t.clients);
      viewersChart.update('none');
      bwChart.data.datasets[0].data = ChartHelpers.mistToXY(t.downbps);
      bwChart.data.datasets[1].data = ChartHelpers.mistToXY(t.upbps);
      bwChart.update('none');
      pktChart.data.datasets[0].data = ChartHelpers.mistToXY(t.perc_lost);
      pktChart.data.datasets[1].data = ChartHelpers.mistToXY(t.perc_retrans);
      pktChart.update('none');
    }
    updateStreamInfo();
  }

  function clearCharts() {
    viewersChart.data.datasets[0].data = [];
    viewersChart.update('none');
    bwChart.data.datasets[0].data = [];
    bwChart.data.datasets[1].data = [];
    bwChart.update('none');
    pktChart.data.datasets[0].data = [];
    pktChart.data.datasets[1].data = [];
    pktChart.update('none');
  }

  function poll() {
    const totalRequests = [
      {fields: ['clients'], end: -1}
    ];
    if (selectedStream) {
      totalRequests.push({
        fields: ['clients', 'upbps', 'downbps', 'perc_lost', 'perc_retrans'],
        streams: [selectedStream],
        end: -15
      });
    }
    apiClient.send(function() {
      refreshStreamPicker();
      if (selectedStream) {
        updateCharts();
      } else {
        updateStreamInfo();
      }
    }, {
      totals: totalRequests,
      active_streams: true
    });
  }

  refreshStreamPicker();
  clearCharts();
  updateStreamInfo();
  poll();
  UI.interval.set(poll, 10e3);
}

// ==============================================================
// VIEWER ANALYTICS
// ==============================================================

function renderViewerAnalytics(container) {
  ChartHelpers.destroyAll();
  container.innerHTML = '';
  const view = createStatsView('stats-view-viewers');

  // Connection count chart
  const connCard = ChartHelpers.chartCard('Connections Over Time', 220);
  // Protocol breakdown
  const protoCard = ChartHelpers.chartCard('Protocol Breakdown', 260);
  // Bandwidth by protocol
  const bwProtoCard = ChartHelpers.chartCard('Bandwidth by Protocol', 220);
  connCard.$card.classList.add('stats-chart-card');
  protoCard.$card.classList.add('stats-chart-card');
  bwProtoCard.$card.classList.add('stats-chart-card');

  const chartRow1 = el('div', {class: 'chart-grid chart-grid-2'});
  chartRow1.appendChild(connCard.$card);
  chartRow1.appendChild(protoCard.$card);
  const chartRow2 = el('div', {class: 'chart-grid chart-grid-1'});
  chartRow2.appendChild(bwProtoCard.$card);

  // Active viewers table
  const tableSection = el('section', {class: 'overview-section stats-viewer-table'});
  tableSection.appendChild(el('h3', null, 'Active Viewers'));
  const table = el('table', {class: 'cattablesort'});
  const tableWrap = el('div', {class: 'dashboard-table-wrap'});
  tableWrap.appendChild(table);
  tableSection.appendChild(tableWrap);

  const chartsMainBlock = createStatsBlock('stats-block-charts');
  chartsMainBlock.appendChild(chartRow1);
  const chartsAuxBlock = createStatsBlock('stats-block-charts-aux');
  chartsAuxBlock.appendChild(chartRow2);
  const tableBlock = createStatsBlock('stats-block-table');
  tableBlock.appendChild(tableSection);

  view.appendChild(chartsMainBlock);
  view.appendChild(chartsAuxBlock);
  view.appendChild(tableBlock);
  container.appendChild(view);

  ChartHelpers.applyDefaults();
  const C = Chart;
  const c = ChartHelpers.getThemeColors();

  // Connections over time
  const connChart = new C(connCard.canvas, {
    type: 'line',
    data: { datasets: [ChartHelpers.lineDataset('Connections', [], {color: c.accent, fill: true, bgColor: c.accentDim})] },
    options: {
      interaction: { mode: 'index', intersect: false },
      scales: {
        x: ChartHelpers.createTimeScale(),
        y: ChartHelpers.createLinearScale({title: 'Viewers', formatter: ChartHelpers.fmtNumber})
      },
      plugins: { legend: { display: false } }
    }
  });
  ChartHelpers.register(connChart);

  // Protocol doughnut
  const protoChart = new C(protoCard.canvas, {
    type: 'doughnut',
    data: { labels: [], datasets: [{ data: [], backgroundColor: ChartHelpers.palette }] },
    options: {
      cutout: '55%',
      plugins: {
        legend: { position: 'right', labels: { boxWidth: 12, padding: 8, color: c.text, font: { size: 11 } } }
      }
    }
  });
  ChartHelpers.register(protoChart);

  // Bandwidth by protocol (stacked bar)
  const bwProtoChart = new C(bwProtoCard.canvas, {
    type: 'bar',
    data: { labels: [], datasets: [] },
    options: {
      scales: {
        x: { ticks: { color: c.secondaryText, font: { size: 11 } }, grid: { display: false } },
        y: ChartHelpers.createLinearScale({title: 'Bandwidth', formatter: ChartHelpers.fmtBytesPerSec})
      },
      plugins: { legend: { display: false } }
    }
  });
  ChartHelpers.register(bwProtoChart);

  function updateCharts() {
    // Connections over time
    if (mist.data.totals && mist.data.totals.all_streams && mist.data.totals.all_streams.all_protocols) {
      const t = mist.data.totals.all_streams.all_protocols;
      connChart.data.datasets[0].data = ChartHelpers.mistToXY(t.clients);
      connChart.update('none');
    }

    // Protocol breakdown from clients data
    // clients response is an array (matching the request array) -- use first entry
    let clientData = null;
    if (mist.data.clients && mist.data.clients.length) {
      clientData = mist.data.clients[0];
    }
    if (clientData && clientData.fields) {
      const protoFields = clientData.fields;
      const protoIdx = protoFields.indexOf('protocol');
      const downIdx = protoFields.indexOf('downbps');
      const upIdx = protoFields.indexOf('upbps');

      if (protoIdx > -1) {
        const protoCounts = {};
        const protoBw = {};
        const rows = clientData.data || [];
        for (let i = 0; i < rows.length; i++) {
          const proto = rows[i][protoIdx] || 'Unknown';
          protoCounts[proto] = (protoCounts[proto] || 0) + 1;
          if (downIdx > -1 && upIdx > -1) {
            protoBw[proto] = (protoBw[proto] || 0) + toNumber(rows[i][downIdx]) + toNumber(rows[i][upIdx]);
          }
        }
        const labels = Object.keys(protoCounts).sort();
        const counts = [];
        const bw = [];
        for (let j = 0; j < labels.length; j++) {
          counts.push(protoCounts[labels[j]]);
          bw.push(protoBw[labels[j]] || 0);
        }
        protoChart.data.labels = labels;
        protoChart.data.datasets[0].data = counts;
        protoChart.update('none');

        // Bandwidth by protocol bar chart
        bwProtoChart.data.labels = labels;
        bwProtoChart.data.datasets = [{
          data: bw,
          backgroundColor: labels.map(function(_, idx) { return ChartHelpers.getColor(idx); })
        }];
        bwProtoChart.update('none');
      }
    }

    // Active viewers table
    updateViewerTable();
  }

  function updateViewerTable() {
    let cd = null;
    if (mist.data.clients && mist.data.clients.length) {
      cd = mist.data.clients[0];
    }
    if (!cd || !cd.fields) {
      table.innerHTML = '';
      const noDataRow = el('tr');
      noDataRow.appendChild(el('td', null, 'No viewer data available.'));
      table.appendChild(noDataRow);
      return;
    }
    const fields = cd.fields;
    const rows = cd.data || [];

    const wantedFields = ['host', 'stream', 'protocol', 'conntime', 'downbps', 'upbps', 'pktlost', 'pktretransmit'];
    const fieldLabels = {
      host: 'Host', stream: 'Stream', protocol: 'Protocol',
      conntime: 'Connected', downbps: 'Down', upbps: 'Up',
      pktlost: 'Lost', pktretransmit: 'Retrans'
    };
    const fieldIndices = [];
    const headerLabels = [];
    for (let i = 0; i < wantedFields.length; i++) {
      const idx = fields.indexOf(wantedFields[i]);
      if (idx > -1) {
        fieldIndices.push({index: idx, field: wantedFields[i]});
        headerLabels.push(fieldLabels[wantedFields[i]] || wantedFields[i]);
      }
    }

    const thead = el('thead');
    const headRow = el('tr');
    for (let h = 0; h < headerLabels.length; h++) {
      headRow.appendChild(el('th', null, headerLabels[h]));
    }
    thead.appendChild(headRow);

    const tbody = el('tbody');
    const limit = Math.min(rows.length, 100);
    for (let r = 0; r < limit; r++) {
      const row = el('tr');
      for (let f = 0; f < fieldIndices.length; f++) {
        const val = rows[r][fieldIndices[f].index];
        const field = fieldIndices[f].field;
        const td = el('td');

        if (field === 'downbps' || field === 'upbps') {
          td.appendChild(format.bits(toNumber(val) * 8, true));
        } else if (field === 'conntime') {
          td.textContent = format.number(toNumber(val)) + 's';
        } else if (field === 'pktlost' || field === 'pktretransmit') {
          td.textContent = format.number(toNumber(val));
        } else {
          td.textContent = val;
        }
        row.appendChild(td);
      }
      tbody.appendChild(row);
    }

    table.innerHTML = '';
    table.appendChild(thead);
    table.appendChild(tbody);
    if (typeof cattablesort === 'function') { cattablesort(table); }
  }

  function poll() {
    apiClient.send(function() { updateCharts(); }, {
      totals: [{fields: ['clients'], end: -15}],
      clients: [{time: -3, fields: ['host', 'stream', 'protocol', 'conntime', 'downbps', 'upbps', 'pktlost', 'pktretransmit']}],
      active_streams: true
    });
  }

  poll();
  UI.interval.set(poll, 10e3);
}

// ==============================================================
// CUSTOM GRAPHS
// ==============================================================

function renderCustom(container) {
  ChartHelpers.destroyAll();
  container.innerHTML = '';
  const view = createStatsView('stats-view-custom');

  let graphs = {};
  const savedGraphs = mistHelpers.stored.get().graphs;
  if (savedGraphs) { graphs = deepExtend({}, savedGraphs); }

  const streams = getStreamList();
  const protocols = getProtocolList();

  const saveas = { graph: 'new', datatype: 'clients', origin: ['total'], xaxis: 'time' };

  // Build creation form
  const graphSelectOpts = [['new', 'New graph']];
  for (const gid in graphs) { graphSelectOpts.push([gid, gid]); }

  const form = el('div', {class: 'dashboard-custom-form stats-custom-form'});
  form.appendChild(el('h3', null, 'Add data set'));

  const graphSelect = el('select');
  for (let s = 0; s < graphSelectOpts.length; s++) {
    const gOpt = el('option', null, graphSelectOpts[s][1]);
    gOpt.value = graphSelectOpts[s][0];
    graphSelect.appendChild(gOpt);
  }

  const graphId = el('input', {type: 'text', placeholder: 'Graph name'});
  graphId.value = 'Graph ' + (Object.keys(graphs).length + 1);

  const datatypeSelect = el('select');
  const datatypes = [
    ['clients', 'Connections'],
    ['upbps', 'Bandwidth (up)'],
    ['downbps', 'Bandwidth (down)'],
    ['cpuload', 'CPU use'],
    ['memload', 'Memory load'],
    ['perc_lost', 'Lost packets'],
    ['perc_retrans', 'Retransmitted packets']
  ];
  for (let d = 0; d < datatypes.length; d++) {
    const dtOpt = el('option', null, datatypes[d][1]);
    dtOpt.value = datatypes[d][0];
    datatypeSelect.appendChild(dtOpt);
  }

  const originRadios = el('div', {class: 'dashboard-origin-radios'});

  const totalRadio = el('input', {type: 'radio', name: 'origin'});
  totalRadio.value = 'total';
  totalRadio.checked = true;
  const originTotal = el('label');
  originTotal.appendChild(totalRadio);
  originTotal.appendChild(document.createTextNode(' All'));

  const originStreamSelect = el('select');
  for (let si = 0; si < streams.length; si++) {
    const stOpt = el('option', null, streams[si]);
    stOpt.value = streams[si];
    originStreamSelect.appendChild(stOpt);
  }
  const streamRadio = el('input', {type: 'radio', name: 'origin'});
  streamRadio.value = 'stream';
  const originStream = el('label');
  originStream.appendChild(streamRadio);
  originStream.appendChild(document.createTextNode(' Stream: '));
  originStream.appendChild(originStreamSelect);

  const originProtoSelect = el('select');
  for (let pi = 0; pi < protocols.length; pi++) {
    const pOpt = el('option', null, protocols[pi]);
    pOpt.value = protocols[pi];
    originProtoSelect.appendChild(pOpt);
  }
  const protoRadio = el('input', {type: 'radio', name: 'origin'});
  protoRadio.value = 'protocol';
  const originProto = el('label');
  originProto.appendChild(protoRadio);
  originProto.appendChild(document.createTextNode(' Protocol: '));
  originProto.appendChild(originProtoSelect);

  originRadios.appendChild(originTotal);
  originRadios.appendChild(originStream);
  originRadios.appendChild(originProto);

  const addBtn = el('button', null, 'Add data set');

  const addToLabel = el('label', null, 'Add to: ');
  addToLabel.appendChild(graphSelect);
  form.appendChild(addToLabel);

  const graphIdLabel = el('label', {class: 'graph-id-label'}, 'Graph name: ');
  graphIdLabel.appendChild(graphId);
  form.appendChild(graphIdLabel);

  const dtLabel = el('label', null, 'Data type: ');
  dtLabel.appendChild(datatypeSelect);
  form.appendChild(dtLabel);

  const originDiv = el('div');
  originDiv.appendChild(el('span', null, 'Data origin:'));
  originDiv.appendChild(originRadios);
  form.appendChild(originDiv);
  form.appendChild(addBtn);

  graphSelect.addEventListener('change', function() {
    if (this.value === 'new') {
      graphIdLabel.hidden = false;
    } else {
      graphIdLabel.hidden = true;
    }
  });

  datatypeSelect.addEventListener('change', function() {
    const val = this.value;
    const radios = originRadios.querySelectorAll('input');
    if (val === 'cpuload' || val === 'memload') {
      totalRadio.checked = true;
      for (let ri = 0; ri < radios.length; ri++) {
        if (radios[ri].value !== 'total') { radios[ri].disabled = true; }
      }
    } else {
      for (let ri2 = 0; ri2 < radios.length; ri2++) {
        radios[ri2].disabled = false;
      }
    }
  });

  const graphContainer = el('div', {class: 'dashboard-custom-graphs stats-custom-graphs'});
  const formBlock = createStatsBlock('stats-block-custom-form');
  formBlock.appendChild(form);
  const graphBlock = createStatsBlock('stats-block-custom-graphs');
  graphBlock.appendChild(graphContainer);
  view.appendChild(formBlock);
  view.appendChild(graphBlock);
  container.appendChild(view);

  // Track chart instances by graph id
  const chartInstances = {};

  function getOrigin() {
    const checked = originRadios.querySelector('input:checked');
    const type = checked ? checked.value : 'total';
    if (type === 'stream') { return ['stream', originStreamSelect.value]; }
    if (type === 'protocol') { return ['protocol', originProtoSelect.value]; }
    return ['total'];
  }

  function getDatasetLabel(datatype, origin) {
    let label = '';
    for (let i = 0; i < datatypes.length; i++) {
      if (datatypes[i][0] === datatype) { label = datatypes[i][1]; break; }
    }
    if (origin[0] === 'total' && datatype !== 'cpuload' && datatype !== 'memload') {
      label += ' (total)';
    } else if (origin[0] !== 'total') {
      label += ' (' + origin[1] + ')';
    }
    return label;
  }

  function saveGraphs() {
    const toSave = {};
    for (const gid in graphs) {
      toSave[gid] = {
        id: gid,
        xaxis: 'time',
        datasets: graphs[gid].datasets.map(function(ds) {
          return { datatype: ds.datatype, origin: ds.origin };
        })
      };
    }
    mistHelpers.stored.set('graphs', toSave);
  }

  function getDataForDataset(ds) {
    if (ds.datatype === 'cpuload') {
      return history.cpu.slice();
    }
    if (ds.datatype === 'memload') {
      return history.memory.slice();
    }
    const streamKey = ds.origin[0] === 'stream' ? ds.origin[1] : 'all_streams';
    const protoKey = ds.origin[0] === 'protocol' ? ds.origin[1] : 'all_protocols';
    if (mist.data.totals && mist.data.totals[streamKey] && mist.data.totals[streamKey][protoKey]) {
      return ChartHelpers.mistToXY(mist.data.totals[streamKey][protoKey][ds.datatype]);
    }
    return [];
  }

  function getYAxisType(datatype) {
    if (datatype === 'cpuload' || datatype === 'memload' || datatype === 'perc_lost' || datatype === 'perc_retrans') {
      return 'percentage';
    }
    if (datatype === 'upbps' || datatype === 'downbps') { return 'bytespersec'; }
    return 'amount';
  }

  function getFormatter(type) {
    if (type === 'percentage') { return ChartHelpers.fmtPercent; }
    if (type === 'bytespersec') { return ChartHelpers.fmtBytesPerSec; }
    return ChartHelpers.fmtNumber;
  }

  function renderGraph(gid) {
    const graph = graphs[gid];
    if (!graph || !graph.datasets || !graph.datasets.length) { return; }

    // Remove existing element
    const existing = graphContainer.querySelector('[data-graph-id="' + gid + '"]');
    if (existing) { existing.remove(); }
    if (chartInstances[gid]) {
      try { chartInstances[gid].destroy(); } catch (e) {}
      delete chartInstances[gid];
    }

    const card = ChartHelpers.chartCard(gid, 260);

    const removeBtn = el('button', {class: 'chart-card-remove', title: 'Remove graph'}, '\u00d7');
    removeBtn.addEventListener('click', function() {
      if (confirm('Remove graph "' + gid + '"?')) {
        const ex = graphContainer.querySelector('[data-graph-id="' + gid + '"]');
        if (ex) { ex.remove(); }
        if (chartInstances[gid]) { try { chartInstances[gid].destroy(); } catch (e) {} }
        delete chartInstances[gid];
        delete graphs[gid];
        saveGraphs();
        const optToRemove = graphSelect.querySelector('option[value="' + gid + '"]');
        if (optToRemove) { optToRemove.remove(); }
      }
    });
    card.$card.setAttribute('data-graph-id', gid);
    card.$card.querySelector('.chart-card-title').appendChild(removeBtn);

    // Determine y-axis types needed
    const yTypes = {};
    for (let i = 0; i < graph.datasets.length; i++) {
      const yt = getYAxisType(graph.datasets[i].datatype);
      yTypes[yt] = true;
    }

    const scales = { x: ChartHelpers.createTimeScale() };
    const ytKeys = Object.keys(yTypes);
    for (let yi = 0; yi < ytKeys.length; yi++) {
      const axisId = yi === 0 ? 'y' : 'y' + yi;
      scales[axisId] = ChartHelpers.createLinearScale({
        title: ytKeys[yi] === 'percentage' ? '%' : (ytKeys[yi] === 'bytespersec' ? 'Bandwidth' : 'Amount'),
        formatter: getFormatter(ytKeys[yi]),
        position: yi === 0 ? 'left' : 'right',
        drawGrid: yi === 0,
        max: ytKeys[yi] === 'percentage' ? 100 : undefined
      });
    }

    const datasets = [];
    for (let di = 0; di < graph.datasets.length; di++) {
      const ds = graph.datasets[di];
      const dsYType = getYAxisType(ds.datatype);
      const dsAxisIdx = ytKeys.indexOf(dsYType);
      const dsAxisId = dsAxisIdx === 0 ? 'y' : 'y' + dsAxisIdx;
      datasets.push(ChartHelpers.lineDataset(
        getDatasetLabel(ds.datatype, ds.origin),
        getDataForDataset(ds),
        { color: ChartHelpers.getColor(di), yAxisID: dsAxisId }
      ));
    }

    const C = Chart;
    const cc = ChartHelpers.getThemeColors();
    const chart = new C(card.canvas, {
      type: 'line',
      data: { datasets: datasets },
      options: {
        interaction: { mode: 'index', intersect: false },
        scales: scales,
        plugins: { legend: { position: 'top', labels: { boxWidth: 12, padding: 8, color: cc.text, font: { size: 11 } } } }
      }
    });
    chartInstances[gid] = chart;
    ChartHelpers.register(chart);
    graphContainer.appendChild(card.$card);
  }

  function updateAllGraphs() {
    bufferSnapshot();
    for (const gid in graphs) {
      const chart = chartInstances[gid];
      if (!chart) { continue; }
      const graph = graphs[gid];
      for (let di = 0; di < graph.datasets.length; di++) {
        chart.data.datasets[di].data = getDataForDataset(graph.datasets[di]);
      }
      chart.update('none');
    }
  }

  addBtn.addEventListener('click', function() {
    let targetGraph = graphSelect.value;
    const datatype = datatypeSelect.value;
    const origin = getOrigin();

    if (targetGraph === 'new') {
      const newId = graphId.value.trim() || ('Graph ' + (Object.keys(graphs).length + 1));
      if (newId in graphs) {
        alert('A graph with this name already exists.');
        return;
      }
      graphs[newId] = { datasets: [] };
      const newOpt = el('option', null, newId);
      newOpt.value = newId;
      graphSelect.appendChild(newOpt);
      graphSelect.value = newId;
      graphSelect.dispatchEvent(new Event('change', {bubbles: true}));
      targetGraph = newId;
      graphId.value = 'Graph ' + (Object.keys(graphs).length + 1);
    }

    graphs[targetGraph].datasets.push({ datatype: datatype, origin: origin });
    saveGraphs();
    renderGraph(targetGraph);
  });

  // Render existing saved graphs
  for (const gid in graphs) {
    if (graphs[gid].datasets && graphs[gid].datasets.length) {
      renderGraph(gid);
    }
  }

  function poll() {
    // Build request based on what custom graphs need
    const req = { capabilities: true };
    const needsTotals = [];
    for (const gid in graphs) {
      for (let di = 0; di < graphs[gid].datasets.length; di++) {
        const ds = graphs[gid].datasets[di];
        if (ds.datatype === 'cpuload' || ds.datatype === 'memload') { continue; }
        const entry = {fields: [ds.datatype], end: -15};
        if (ds.origin[0] === 'stream') { entry.streams = [ds.origin[1]]; }
        if (ds.origin[0] === 'protocol') { entry.protocols = [ds.origin[1]]; }
        needsTotals.push(entry);
      }
    }
    if (needsTotals.length) { req.totals = needsTotals; }
    apiClient.send(function() { updateAllGraphs(); }, req);
  }

  if (Object.keys(graphs).length) { poll(); }
  UI.interval.set(poll, 10e3);
}

// ==============================================================
// MAIN TAB HANDLER
// ==============================================================

registerTab('Statistics', function(tab, other, prev, $main, $pageHeader) {
  $main.classList.add('page-body--flex-col', 'statistics-page-body', 'statistics-shell', 'slab-shell');
  $main.appendChild(document.createTextNode('Loading..'));

  if ($pageHeader) {
    const promData = {prometheus: mist.data.config.prometheus || ''};
    const host = mist.user.host || mist.defaultHost || '[server]/api';
    const promForm = formEngine.buildUI([{
      type: 'selectinput',
      label: 'Prometheus',
      selectinput: [
        ['', 'Disabled'],
        [{type: 'str', label: 'Subpath', placeholder: 'metrics'}, 'Enabled']
      ],
      pointer: {main: promData, index: 'prometheus'},
      help: 'Make stats available in Prometheus format via ' + host + '/SUBPATH.',
      'function': function() {
        apiClient.send(function(d) {
          if (d && d.config) mist.data.config = d.config;
        }, {config: {prometheus: promData.prometheus}});
      }
    }]);
    promForm.classList.add('prometheus-inline');
    uiHelpers.appendPageActions($pageHeader, promForm);
  }

  apiClient.send(function() {
    $main.innerHTML = '';

    // Count CPU cores for history
    if (mist.data.capabilities && mist.data.capabilities.cpu) {
      let cores = 0;
      for (const i in mist.data.capabilities.cpu) { cores += mist.data.capabilities.cpu[i].cores; }
    }
    bufferSnapshot();

    const views = [
      {key: 'overview', label: 'Server Overview'},
      {key: 'stream',   label: 'Per-Stream'},
      {key: 'viewers',  label: 'Viewer Analytics'}
    ];

    // Show custom tab in advanced mode or if saved graphs exist
    const hasSavedGraphs = mistHelpers.stored.get().graphs && Object.keys(mistHelpers.stored.get().graphs).length > 0;
    const isAdvanced = document.body.getAttribute('data-mode') === 'advanced';
    if (isAdvanced || hasSavedGraphs) {
      views.push({key: 'custom', label: 'Custom'});
    }

    let currentView = other || 'overview';
    const apiHost = mist.user.host || mist.defaultHost || '[server]/api';
    const intro = pageIntro(
      'Monitor server health, per-stream performance, and viewer analytics from this dashboard. '+
      'Statistics shown cover the last 10 minutes of activity by default. '+
      'Prometheus export is configured under <b>General</b> &rarr; <b>Prometheus stats output</b>. '+
      'API base for pull-based statistics: <code>'+escapeHTML(apiHost)+'</code>. '+
      'For endpoint details and collection examples, see the '+
      '<a href="https://docs.mistserver.org/" target="_blank" rel="noopener noreferrer">'+APP_NAME+' documentation</a>.'
    );
    const content = el('div', {class: 'dashboard-content statistics-content'});
    const nav = buildSubNav(views, currentView, function(key) {
      currentView = key;
      UI.interval.clear();
      switchView(key);
    });
    nav.classList.add('statistics-subnav');

    $main.appendChild(intro);
    $main.appendChild(nav);
    $main.appendChild(content);

    function switchView(key) {
      content.setAttribute('data-stat-view', key);
      switch (key) {
        case 'overview': renderOverview(content); break;
        case 'stream':   renderStreamDetail(content); break;
        case 'viewers':  renderViewerAnalytics(content); break;
        case 'custom':   renderCustom(content); break;
        default:         renderOverview(content); break;
      }
    }

    switchView(currentView);
  }, {active_streams: true, capabilities: true});
});

registerTab('Server Stats', function() {
  navto('Statistics');
});
