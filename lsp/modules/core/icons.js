import { createElement as lucideCreateElement } from 'lucide';
import {
  Plus, Trash2, Check, Sparkles, X, Save, ArrowUp, ArrowDown,
  Undo2, KeyRound, Hand, ShieldAlert, Radiation, Copy, ClipboardPaste,
  Settings, Activity, Eye, Code, Folder, Moon, AlarmClock, Play, Sun,
  SlidersHorizontal, List, Users, Upload, Camera, Cast, Video, Monitor,
  Zap, File, Globe, CircleHelp, Info, ExternalLink, LogOut, Power, Cpu,
  MemoryStick, Gauge, Mail, Music, LayoutGrid, HardDrive, CloudUpload,
  Radio, Tv, Send, Repeat, Calendar, Variable, Target, Clock, Pencil,
  Square, Shield, Layers, Route, BarChart2, Server, DoorOpen, PlusCircle,
  RefreshCw, Loader, CheckCircle, PauseCircle, PenLine, CornerDownRight,
  LifeBuoy, UserX, UploadCloud, AlertTriangle, XCircle
} from 'lucide';

const lucideIcons = {
  Plus, Trash2, Check, Sparkles, X, Save, ArrowUp, ArrowDown,
  Undo2, KeyRound, Hand, ShieldAlert, Radiation, Copy, ClipboardPaste,
  Settings, Activity, Eye, Code, Folder, Moon, AlarmClock, Play, Sun,
  SlidersHorizontal, List, Users, Upload, Camera, Cast, Video, Monitor,
  Zap, File, Globe, CircleHelp, Info, ExternalLink, LogOut, Power, Cpu,
  MemoryStick, Gauge, Mail, Music, LayoutGrid, HardDrive, CloudUpload,
  Radio, Tv, Send, Repeat, Calendar, Variable, Target, Clock, Pencil,
  Square, Shield, Layers, Route, BarChart2, Server, DoorOpen, PlusCircle,
  RefreshCw, Loader, CheckCircle, PauseCircle, PenLine, CornerDownRight,
  LifeBuoy, UserX, UploadCloud, AlertTriangle, XCircle
};

const ICON_MAP = {
  'plus':         'Plus',
  'trash':        'Trash2',
  'check':        'Check',
  'sparkles':     'Sparkles',
  'cross':        'X',
  'disk':         'Save',
  'up':           'ArrowUp',
  'down':         'ArrowDown',
  'return':       'Undo2',
  'key':          'KeyRound',
  'stop':         'Hand',
  'stop_remove':  'Hand',
  'invalidate':   'ShieldAlert',
  'nuke':         'Radiation',
  'copy':         'Copy',
  'paste':        'ClipboardPaste',
  'code':         'Code',
  'eye':          'Eye',
  'Edit':         'Settings',
  'Status':       'Activity',
  'Preview':      'Eye',
  'Embed':        'Code',
  'Streams':      'Undo2',
  'folder':       'Folder',
  'sleep':        'Moon',
  'wake':         'AlarmClock',
  'play':         'Play',
  'sun':          'Sun',
  'moon':         'Moon',
  'sliders':      'SlidersHorizontal',
  'list':         'List',
  'activity':     'Activity',
  'users':        'Users',
  'upload':       'Upload',
  'camera':       'Camera',
  'cast':         'Cast',
  'video':        'Video',
  'monitor':      'Monitor',
  'zap':          'Zap',
  'file':         'File',
  'globe':        'Globe',
  'circle-help':  'CircleHelp',
  'info':         'Info',
  'external-link': 'ExternalLink',
  'log-out':      'LogOut',
  'power':        'Power',
  'cpu':          'Cpu',
  'memory-stick': 'MemoryStick',
  'gauge':        'Gauge',
  'mail':         'Mail',
  'music':        'Music',
  'grid':         'LayoutGrid',
  'hard-drive':   'HardDrive',
  'cloud-upload': 'CloudUpload',
  'radio':        'Radio',
  'tv':           'Tv',
  'send':         'Send',
  'repeat':       'Repeat',
  'calendar':     'Calendar',
  'variable':     'Variable',
  'target':       'Target',
  'clock':        'Clock',
  'pencil':       'Pencil',
  'square':       'Square',
  'shield':       'Shield',
  'layers':       'Layers',
  'route':        'Route',
  'bar-chart-2':  'BarChart2',
  'server':       'Server',
  'door-open':    'DoorOpen',
  'plus-circle':  'PlusCircle',
  'trash-2':      'Trash2',
  'refresh-cw':   'RefreshCw',
  'loader':       'Loader',
  'check-circle': 'CheckCircle',
  'pause-circle': 'PauseCircle',
  'pen-line':     'PenLine',
  'corner-down-right': 'CornerDownRight',
  'life-buoy':    'LifeBuoy',
  'user-x':       'UserX',
  'upload-cloud': 'UploadCloud',
  'alert-triangle':'AlertTriangle',
  'x-circle':     'XCircle'
};

function renderIcon(el) {
  const name = el.getAttribute('data-icon');
  if (!name) return;

  const lucideName = ICON_MAP[name];
  if (!lucideName || !lucideIcons[lucideName]) return;

  const existing = el.querySelector('.lucide-icon');
  if (existing) existing.remove();

  const iconData = lucideIcons[lucideName];
  const svg = lucideCreateElement(iconData);
  svg.classList.add('lucide-icon');
  el.insertBefore(svg, el.firstChild);
}

function renderAllIcons(root) {
  const els = (root || document).querySelectorAll('[data-icon]');
  for (let i = 0; i < els.length; i++) {
    renderIcon(els[i]);
  }
}

const observer = new MutationObserver(function(mutations) {
  for (let i = 0; i < mutations.length; i++) {
    const m = mutations[i];
    if (m.type === 'childList') {
      for (let j = 0; j < m.addedNodes.length; j++) {
        const node = m.addedNodes[j];
        if (node.nodeType !== 1) continue;
        if (node.hasAttribute && node.hasAttribute('data-icon')) {
          renderIcon(node);
        }
        if (node.querySelectorAll) {
          const descendants = node.querySelectorAll('[data-icon]');
          for (let k = 0; k < descendants.length; k++) {
            renderIcon(descendants[k]);
          }
        }
      }
    }
    if (m.type === 'attributes' && m.attributeName === 'data-icon') {
      renderIcon(m.target);
    }
  }
});

function init() {
  renderAllIcons();
  observer.observe(document.body, {
    childList: true,
    subtree: true,
    attributes: true,
    attributeFilter: ['data-icon']
  });
}

export const MistIcons = {
  init: init,
  renderIcon: renderIcon,
  renderAllIcons: renderAllIcons,
  ICON_MAP: ICON_MAP
};
