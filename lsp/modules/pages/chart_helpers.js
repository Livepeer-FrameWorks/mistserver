import {
  Chart,
  LineController, LineElement, PointElement,
  BarController, BarElement,
  DoughnutController, ArcElement,
  LinearScale, TimeScale, CategoryScale,
  Legend, Tooltip, Filler
} from 'chart.js';
import 'chartjs-adapter-date-fns';

Chart.register(
  LineController, LineElement, PointElement,
  BarController, BarElement,
  DoughnutController, ArcElement,
  LinearScale, TimeScale, CategoryScale,
  Legend, Tooltip, Filler
);
import * as format from '../core/formatters.js';
import { el } from '../core/dom_helpers.js';

const ChartHelpers = {};

// ---- Theme colors from CSS variables ----

ChartHelpers.getThemeColors = function() {
  const style = getComputedStyle(document.body);
  function v(name) { return style.getPropertyValue(name).trim(); }
  return {
    text:          v('--textColor'),
    textFaded:     v('--textColorFaded'),
    secondaryText: v('--secondaryTextColor'),
    bg:            v('--backgroundColor'),
    secondaryBg:   v('--secondaryBackgroundColor'),
    accent:        v('--accentColor'),
    accentDim:     v('--accentColorDim'),
    border:        v('--border-color'),
    red:           v('--red'),
    green:         v('--green'),
    orange:        v('--orange')
  };
};

ChartHelpers.applyDefaults = function() {
  const c = ChartHelpers.getThemeColors();
  const C = Chart;
  if (!C) { return; }
  C.defaults.color = c.secondaryText;
  C.defaults.borderColor = c.border;
  C.defaults.plugins.legend.labels.color = c.text;
  C.defaults.plugins.tooltip.backgroundColor = c.secondaryBg;
  C.defaults.plugins.tooltip.titleColor = c.text;
  C.defaults.plugins.tooltip.bodyColor = c.text;
  C.defaults.plugins.tooltip.borderColor = c.border;
  C.defaults.plugins.tooltip.borderWidth = 1;
  C.defaults.animation = false;
  C.defaults.responsive = true;
  C.defaults.maintainAspectRatio = false;
};

// ---- Palette for datasets ----

ChartHelpers.palette = [
  '#7aa2f7', '#9ece6a', '#f7768e', '#ff9e64',
  '#bb9af7', '#73daca', '#e0af68', '#7dcfff',
  '#c0caf5', '#a9b1d6'
];

ChartHelpers.getColor = function(index) {
  return ChartHelpers.palette[index % ChartHelpers.palette.length];
};

// ---- Data transformation ----

ChartHelpers.mistToXY = function(mistArray) {
  if (!mistArray || !mistArray.length) { return []; }
  const out = [];
  for (let i = 0; i < mistArray.length; i++) {
    out.push({x: mistArray[i][0], y: mistArray[i][1]});
  }
  return out;
};

// ---- Plain-text formatters for chart ticks/tooltips ----

ChartHelpers.fmtPercent = function(val) {
  return format.number(val) + '%';
};

ChartHelpers.fmtNumber = function(val) {
  return format.number(val);
};

ChartHelpers.fmtBytesPerSec = function(val) {
  if (val === 0) { return '0 bit/s'; }
  const bits = val * 8;
  const suffixes = ['bit','kbit','Mbit','Gbit','Tbit','Pbit'];
  let exp = Math.floor(Math.log(Math.abs(bits)) / Math.log(1000));
  if (exp < 0) { exp = 0; }
  if (exp >= suffixes.length) { exp = suffixes.length - 1; }
  const scaled = bits / Math.pow(1000, exp);
  return format.number(scaled) + ' ' + suffixes[exp] + '/s';
};

ChartHelpers.fmtBytes = function(val) {
  if (val === 0) { return '0 bytes'; }
  const suffixes = ['byte','KiB','MiB','GiB','TiB','PiB'];
  let exp = Math.floor(Math.log(Math.abs(val)) / Math.log(1024));
  if (exp < 0) { exp = 0; }
  if (exp >= suffixes.length) { exp = suffixes.length - 1; }
  const scaled = val / Math.pow(1024, exp);
  return format.number(scaled) + ' ' + suffixes[exp];
};

ChartHelpers.fmtTime = function(val) {
  return format.time(val / 1e3);
};

// ---- Chart factory helpers ----

ChartHelpers.createTimeScale = function(opts) {
  const c = ChartHelpers.getThemeColors();
  return {
    type: 'time',
    time: {
      tooltipFormat: 'HH:mm:ss',
      displayFormats: { second: 'HH:mm:ss', minute: 'HH:mm', hour: 'HH:mm' }
    },
    ticks: { color: c.secondaryText, maxTicksLimit: 8, font: { size: 11 } },
    grid: { color: c.border, drawBorder: false },
    title: opts && opts.title ? { display: true, text: opts.title, color: c.secondaryText } : { display: false }
  };
};

ChartHelpers.createLinearScale = function(opts) {
  opts = opts || {};
  const c = ChartHelpers.getThemeColors();
  const scale = {
    type: 'linear',
    position: opts.position || 'left',
    beginAtZero: opts.beginAtZero !== false,
    ticks: {
      color: c.secondaryText,
      font: { size: 11 },
      maxTicksLimit: 6
    },
    grid: {
      color: opts.drawGrid !== false ? c.border : 'transparent',
      drawBorder: false
    },
    title: opts.title ? { display: true, text: opts.title, color: c.secondaryText, font: { size: 11 } } : { display: false }
  };
  if (opts.formatter) {
    scale.ticks.callback = opts.formatter;
  }
  if (opts.max !== undefined) { scale.max = opts.max; }
  if (opts.min !== undefined) { scale.min = opts.min; }
  return scale;
};

ChartHelpers.lineDataset = function(label, data, opts) {
  opts = opts || {};
  return {
    label: label,
    data: data,
    borderColor: opts.color || ChartHelpers.palette[0],
    backgroundColor: opts.fill ? (opts.bgColor || opts.color || ChartHelpers.palette[0]) : 'transparent',
    borderWidth: opts.borderWidth || 2,
    pointRadius: 0,
    pointHitRadius: 6,
    tension: 0.3,
    fill: !!opts.fill,
    yAxisID: opts.yAxisID || 'y',
    hidden: !!opts.hidden
  };
};

// ---- Chart lifecycle management ----

let registeredCharts = [];

ChartHelpers.register = function(chart) {
  registeredCharts.push(chart);
};

ChartHelpers.destroyAll = function() {
  for (let i = 0; i < registeredCharts.length; i++) {
    try { registeredCharts[i].destroy(); } catch (e) {}
  }
  registeredCharts = [];
};

ChartHelpers.updateAllColors = function() {
  ChartHelpers.applyDefaults();
  const c = ChartHelpers.getThemeColors();
  for (let i = 0; i < registeredCharts.length; i++) {
    const chart = registeredCharts[i];
    try {
      // Update scale colors
      for (const key in chart.options.scales) {
        const scale = chart.options.scales[key];
        if (scale.ticks) { scale.ticks.color = c.secondaryText; }
        if (scale.grid) { scale.grid.color = c.border; }
        if (scale.title) { scale.title.color = c.secondaryText; }
      }
      chart.options.plugins.legend.labels.color = c.text;
      chart.update('none');
    } catch (e) {}
  }
};

// Listen for theme toggle
document.addEventListener('click', function(e) {
  if (e.target.closest('#theme-toggle')) {
    setTimeout(ChartHelpers.updateAllColors, 50);
  }
});

// ---- Canvas element factory ----

ChartHelpers.createCanvas = function() {
  return document.createElement('canvas');
};

// ---- Create a chart wrapped in a card ----

ChartHelpers.chartCard = function(title, heightPx) {
  const card = el('div', {class: 'chart-card'});
  if (title) {
    card.appendChild(el('h4', {class: 'chart-card-title'}, title));
  }
  const wrap = el('div', {class: 'chart-card-body'});
  if (heightPx) { wrap.style.height = heightPx + 'px'; }
  const canvas = ChartHelpers.createCanvas();
  wrap.appendChild(canvas);
  card.appendChild(wrap);
  return { $card: card, canvas: canvas };
};

export { ChartHelpers };
