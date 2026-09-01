import { APP_NAME } from '@brand';
import { el, deepExtend } from './dom_helpers.js';
import * as format from './formatters.js';
import { apiClient } from './api_client.js';
import { tabHandlers } from './tab_registry.js';

const MistVideoObject = {};
const otherhost = {
  host: false,
  https: false
};
const UI = {
  debug: false,
  elements: {},
  stored: {
    getOpts: function(){
      let stored = localStorage['stored'];
      if (stored) {
        stored = JSON.parse(stored);
      }
      deepExtend(this.vars,stored);
      return this.vars;
    },
    saveOpt: function(name,val){
      this.vars[name] = val;
      localStorage['stored'] = JSON.stringify(this.vars);
      return this.vars;
    },
    vars: {
      helpme: true
    }
  },
  interval: {
    list: {},
    clear: function(){
      for (const i in this.list) {
        clearInterval(this.list[i].id);
      }
      this.list = {};
    },
    set: function(callback,delay){
      if (this.opts) {
        log('[interval]','Set called on interval, but an interval is already active.');
      }

      const opts = {
        delay: delay,
        callback: callback,
        id: setInterval(callback,delay)
      };
      this.list[opts.id] = opts;
      return opts.id;
    }
  },
  websockets: {
    list: [],
    clear: function(){
      for (const i in this.list) {
        this.list[i].close();
      }
    },
    create: function(url,error_callback){
      let ws;
      try {
        ws = new WebSocket(url);
      } catch(e) {
        console.error("WebSocket creation failed for "+url+":",e.message);
        if (error_callback) error_callback(e);
        return null;
      }
      const me = this;
      this.list.push(ws);
      ws.addEventListener("close",function(){
        for (let i = me.list.length - 1; i >=0; i--) {
          if (me.list[i] == ws) { me.list.splice(i,1); }
        }
      });
      if (error_callback) ws.addEventListener("error",error_callback);
      return ws;
    }
  },
  tooltip: {
    show: function (anchor,contents){
      const tip = this.element;

      if (!document.body.contains(tip)) {
        document.body.appendChild(tip);
      }

      tip.innerHTML = '';
      if (contents instanceof Node) {
        tip.appendChild(contents);
      } else if (typeof contents !== 'undefined' && contents !== null) {
        tip.innerHTML = String(contents);
      }
      clearTimeout(this.hiding);
      delete this.hiding;

      tip.hidden = false;
      tip.classList.remove('show');

      const gap = 10;
      const scrollX = window.pageXOffset || document.documentElement.scrollLeft || 0;
      const scrollY = window.pageYOffset || document.documentElement.scrollTop || 0;
      const viewportW = document.documentElement.clientWidth || window.innerWidth || 0;
      const viewportH = window.innerHeight || document.documentElement.clientHeight || 0;
      const tipW = tip.offsetWidth;
      const tipH = tip.offsetHeight;

      let anchorEl = null;
      if (anchor && (typeof Event !== 'undefined') && (anchor instanceof Event)) {
        anchorEl = anchor.currentTarget || anchor.target || null;
      } else if (anchor && anchor.getBoundingClientRect) {
        anchorEl = anchor;
      } else if (anchor && anchor.target && anchor.target.getBoundingClientRect) {
        anchorEl = anchor.target;
      }
      if (anchorEl && (anchorEl.nodeType === 3)) {
        anchorEl = anchorEl.parentElement;
      }

      let left;
      let top;

      if (anchorEl && anchorEl.getBoundingClientRect) {
        const rect = anchorEl.getBoundingClientRect();
        left = rect.left + scrollX;
        top = rect.bottom + scrollY + 8;

        const fitsBelow = (rect.bottom + 8 + tipH) <= (viewportH - gap);
        if (!fitsBelow) {
          top = rect.top + scrollY - tipH - 8;
        }
      } else if (anchor && (typeof anchor.pageX === 'number') && (typeof anchor.pageY === 'number')) {
        left = anchor.pageX + 10;
        top = anchor.pageY + 25;
      } else {
        left = scrollX + gap;
        top = scrollY + gap;
      }

      const minLeft = scrollX + gap;
      let maxLeft = scrollX + viewportW - tipW - gap;
      if (maxLeft < minLeft) { maxLeft = minLeft; }
      left = Math.min(Math.max(minLeft, left), maxLeft);

      const minTop = scrollY + gap;
      let maxTop = scrollY + viewportH - tipH - gap;
      if (maxTop < minTop) { maxTop = minTop; }
      top = Math.min(Math.max(minTop, top), maxTop);

      tip.style.left = left + 'px';
      tip.style.top = top + 'px';
      tip.classList.add('show');
    },
    hide: function() {
      const tip = this.element;
      tip.classList.remove('show');
      this.hiding = setTimeout(function(){
        tip.hidden = true;
      },200);
    },
    element: el('div', {id: 'tooltip'})
  },
  menu: [
    {
      Overview: { icon: 'gauge' },
      General: {
        icon: 'sliders',
        hiddenmenu: {
          "Stream keys": {}
        }
      },
      Protocols: { icon: 'globe' },
      Streams: {
        icon: 'video',
        keepParam: true,
        hiddenmenu: {
          Edit: {},
          'Stream Config': {},
          Status: {},
          Preview: {},
          Embed: {}
        }
      },
      Push: { icon: 'upload' },
      Devices: {
        icon: 'monitor',
        keepParam: true,
        hiddenmenu: {
          'Camera Detail': {},
          'PTZ Control': {}
        }
      },
      Automations: { icon: 'zap' },
      Connections: { icon: 'users' },
      Logs: { icon: 'file' },
      Statistics: { icon: 'activity' }
    },
    {
      Documentation: {
        icon: 'circle-help',
        link: 'https://docs.mistserver.org/'
      },
      Changelog: {
        icon: 'list',
        link: 'https://releases.mistserver.org/changelog'
      },
      'Email for Help': { icon: 'mail' }
    }
  ],
  plot: null,
  tabHandlers: tabHandlers,
  pageSubtitles: {
    'Overview':       'Server status, active streams, and version information',
    'General':        'Server name, debug levels, and global configuration',
    'Protocols':      'Manage input and output protocols for stream delivery',
    'Streams':        'View, create, and manage your media streams',
    'Push':           'Send streams to external servers or record to files',
    'Devices':        'Discovered network devices and PTZ controls',
    'Automations':    'Event-driven rules for authentication, redirects, and more',
    'Connections':    'Live, filterable list of active viewer, input, and output connections',
    'Logs':           'Recent server log messages and diagnostic output',
    'Statistics':     'Server health, stream metrics, and viewer analytics',
    'Email for Help': 'Contact '+APP_NAME+' support with your configuration',
    'Stream keys':    'Bypass tokens for allowing incoming stream pushes'
  }
};

if (!('origin' in location)) {
  location.origin = location.protocol+'//';
}
let host;
if (location.origin == 'file://') {
  host = 'http://localhost:4242/api';
}
else {
  host = location.origin+location.pathname.replace(/\/+$/, "")+'/api';
}
const mist = {
  data: {},
  defaultHost: host,
  user: {
    name: '',
    password: '',
    host: host
  }
};

function log() {
  try {
    if (UI.debug) {
      const error = (new Error).stack;
      [].push.call(arguments,error);
    }
    [].unshift.call(arguments,'['+format.time((new Date).getTime()/1000)+']');
    console.log.apply(console,arguments);
  } catch(e) {}
}

function parseURL(url,set) {
  return apiClient.parseURL(url,set);
}

function triggerRewrite(trigger) {
  if ((typeof trigger == 'object') && (typeof trigger.length == 'undefined')) { return trigger; }
  return {
    handler: trigger[0],
    sync: trigger[1],
    streams: trigger[2],
    'default': trigger[3]
  };
}

window.UI = UI;
window.mist = mist;
window.MistVideoObject = MistVideoObject;
window.otherhost = otherhost;

export { UI, mist, log, parseURL, triggerRewrite, otherhost, MistVideoObject };
