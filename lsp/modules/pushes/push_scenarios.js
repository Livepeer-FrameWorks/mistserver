import { APP_NAME } from '@brand';
import { apiClient } from '../core/api_client.js';
import { mistHelpers } from '../core/mist_helpers.js';
import { findInput } from '../core/capabilities.js';

const SCENARIOS = {
  record_file: {
    group: 'record', title: 'Record to File',
    desc: 'Save a stream to disk as MP4, MKV, or TS.',
    icon: 'hard-drive',
    available: function() {
      return hasFileTargets();
    },
    targetLabel: 'File path',
    targetHelp: 'Path where the recording will be saved. Use template variables for dynamic file names.',
    placeholder: '/recordings/$stream_$datetime.ts',
    defaultTarget: '/recordings/$stream_$datetime.ts',
    matchTarget: function(target) {
      if (!target || target.charAt(0) !== '/') return false;
      if (hasWriterPrefix(target)) return false;
      if (target.indexOf('?m3u8=') >= 0 || target.indexOf('&m3u8=') >= 0) return false;
      return true;
    },
    hint: '<b>Common formats:</b><ul>'
      + '<li><b>.ts</b> - MPEG-TS. Resilient to crashes and power loss. Recommended for recording.</li>'
      + '<li><b>.mkv</b> - Flexible container, supports more codecs and multiple tracks. Also crash-resilient.</li>'
      + '<li><b>.mp4</b> - Most widely known. Best for archival, but needs the full recording in buffer to finalize properly.</li>'
      + '<li><b>.flv</b> - Flash container. Partially crash-resilient, limited codec support.</li>'
      + '</ul>'
      + '<b>Tip:</b> Record as <code>.ts</code> or <code>.mkv</code> for reliability. You can download recordings as <code>.mp4</code> from the Embed page.'
  },
  record_dvr: {
    group: 'record', title: 'Live DVR (HLS Recording)',
    desc: 'Record as TS segments with an HLS playlist for seekable live playback with a rolling buffer window.',
    icon: 'repeat',
    available: function() {
      return hasFileTargets();
    },
    targetLabel: 'Segment path',
    targetHelp: 'Path for segment files. Use template variables for dynamic naming. The <code>m3u8</code> parameter sets the playlist path.',
    placeholder: '/recordings/$basename/$yday/$hour/$minute_$segmentCounter.ts',
    defaultTarget: '/recordings/$basename/$yday/$hour/$minute_$segmentCounter.ts?m3u8=../../$basename.m3u8&split=24&maxEntries=3600&append=1&noendlist=1',
    matchTarget: function(target) {
      return target && (target.indexOf('?m3u8=') >= 0 || target.indexOf('&m3u8=') >= 0);
    },
    hint: APP_NAME+' writes <b>.ts segments</b> and maintains an <b>HLS playlist</b> (.m3u8). Viewers can seek through the entire buffer window while the stream is still live. Configure the fields below to control segment naming, playlist location, and rolling buffer behavior.'
  },
  record_cloud: {
    group: 'record', title: 'Record to Cloud',
    desc: 'Record to Amazon S3 or compatible cloud storage.',
    icon: 'cloud-upload',
    available: function() {
      return getWriterProtocols().length > 0;
    },
    targetLabel: 'Cloud path',
    targetHelp: 'Enter the storage path. Use template variables for dynamic paths.',
    placeholder: 'bucket/path/$stream_$datetime.mp4',
    defaultTarget: '',
    getLockedPrefixes: function() { return getWriterProtocols(); },
    matchTarget: function(target) {
      if (!target) return false;
      const wp = getWriterProtocols();
      for (let i = 0; i < wp.length; i++) {
        if (target.indexOf(wp[i]) === 0) return true;
      }
      return false;
    },
    hint: 'The cloud URL format depends on your storage provider. Example for S3: <code>s3://bucket-name/path/$stream_$datetime.mp4</code>. Make sure the external writer binary is installed and accessible. See the <a href="https://docs.mistserver.org/" target="_blank" rel="noopener">'+APP_NAME+' documentation</a> on external writers for setup and supported providers.'
  },
  push_rtmp: {
    group: 'server', title: 'RTMP / RTMPS',
    desc: 'Push to an RTMP or RTMPS server.',
    icon: 'cast',
    available: function() {
      return hasProtocol('rtmp://') || hasProtocol('rtmps://');
    },
    targetLabel: 'Server address',
    targetHelp: 'The RTMP server address and stream path.',
    placeholder: 'hostname/app/streamkey',
    defaultTarget: '',
    lockedPrefixes: ['rtmp://', 'rtmps://'],
    defaultLockedPrefix: 'rtmp://',
    matchTarget: function(target) {
      return target && (target.indexOf('rtmp://') === 0 || target.indexOf('rtmps://') === 0);
    }
  },
  push_srt: {
    group: 'server', title: 'SRT',
    desc: 'Low-latency push over Secure Reliable Transport.',
    icon: 'zap',
    available: function() { return hasProtocol('srt://'); },
    targetLabel: 'SRT address',
    targetHelp: 'The SRT endpoint address to push to.',
    placeholder: 'hostname:port',
    defaultTarget: '',
    lockedPrefixes: ['srt://'],
    defaultLockedPrefix: 'srt://',
    matchTarget: function(target) {
      return target && target.indexOf('srt://') === 0;
    }
  },
  push_ts: {
    group: 'server', title: 'MPEG-TS (UDP/TCP/RTP)',
    desc: 'Push an MPEG-TS stream over UDP, TCP, or RTP.',
    icon: 'radio',
    available: function() {
      return hasProtocol('tsudp://') || hasProtocol('tstcp://') || hasProtocol('tsrtp://');
    },
    targetLabel: 'TS destination',
    targetHelp: 'The MPEG-TS destination address.',
    placeholder: 'hostname:port',
    defaultTarget: '',
    lockedPrefixes: ['tsudp://', 'tstcp://', 'tsrtp://'],
    defaultLockedPrefix: 'tsudp://',
    matchTarget: function(target) {
      return target && (target.indexOf('tsudp://') === 0 || target.indexOf('tstcp://') === 0 || target.indexOf('tsrtp://') === 0);
    }
  },
  push_other: {
    group: 'server', title: 'Other Protocol',
    desc: 'RIST, RTSP, WHIP, DTSC, NDI, CMAF, or any other supported protocol.',
    icon: 'globe',
    available: function() { return true; },
    targetLabel: 'Target URL',
    targetHelp: 'Enter the full push target URL.',
    placeholder: '',
    defaultTarget: '',
    matchTarget: function() { return false; }
  },
  cdn_youtube: {
    group: 'platform', title: 'YouTube Live',
    desc: 'Push to YouTube Live via RTMP.',
    icon: 'tv',
    available: function() {
      return hasProtocol('rtmp://');
    },
    targetPrefix: 'rtmp://a.rtmp.youtube.com/live2/',
    targetLabel: 'Stream key',
    targetHelp: 'Paste the stream key from YouTube Studio.',
    placeholder: 'xxxx-xxxx-xxxx-xxxx-xxxx',
    isPlatform: true,
    hint: 'In <a href="https://studio.youtube.com" target="_blank">YouTube Studio</a>, go to <b>Go Live</b> and copy the <b>Stream key</b> from the stream settings panel.'
  },
  cdn_twitch: {
    group: 'platform', title: 'Twitch',
    desc: 'Push to Twitch via RTMP.',
    icon: 'tv',
    available: function() {
      return hasProtocol('rtmp://');
    },
    targetPrefix: 'rtmp://live.twitch.tv/app/',
    targetLabel: 'Stream key',
    targetHelp: 'Paste the stream key from your Twitch dashboard.',
    placeholder: 'live_xxxxxxxxxx_xxxxxxxxxx',
    isPlatform: true,
    hint: 'In the <a href="https://dashboard.twitch.tv/settings/stream" target="_blank">Twitch Dashboard</a>, go to <b>Settings \u2192 Stream</b> and copy the <b>Primary Stream key</b>.'
  },
  cdn_facebook: {
    group: 'platform', title: 'Facebook Live',
    desc: 'Push to Facebook Live via RTMPS.',
    icon: 'tv',
    available: function() {
      return hasProtocol('rtmps://') || hasProtocol('rtmp://');
    },
    targetPrefix: 'rtmps://live-api-s.facebook.com:443/rtmp/',
    targetLabel: 'Stream key',
    targetHelp: 'Paste the stream key from Facebook Live Producer.',
    placeholder: 'FB-xxxx-xxxx-xxxx-xxxx',
    isPlatform: true,
    hint: 'In <a href="https://www.facebook.com/live/producer" target="_blank">Facebook Live Producer</a>, select <b>Go Live</b>, then choose <b>Streaming software</b> and copy the <b>Stream key</b>. Enable "Persistent stream key" to reuse it across broadcasts.'
  },
  cdn_kick: {
    group: 'platform', title: 'Kick',
    desc: 'Push to Kick via RTMPS.',
    icon: 'tv',
    available: function() {
      return hasProtocol('rtmps://') || hasProtocol('rtmp://');
    },
    targetPrefix: 'rtmps://fa723fc1b171.global-contribute.live-video.net:443/app/',
    targetLabel: 'Stream key',
    targetHelp: 'Paste the stream key from your Kick Creator Dashboard.',
    placeholder: 'sk_xxxxxxxxxxxxxxxxxxxx',
    isPlatform: true,
    hint: 'In the <a href="https://kick.com/dashboard/settings/stream" target="_blank">Kick Creator Dashboard</a>, go to <b>Settings \u2192 Stream</b> and copy the <b>Stream key</b>.'
  },
  cdn_custom: {
    group: 'platform', title: 'Custom Platform',
    desc: 'X (Twitter), Instagram, TikTok, Restream, or any other RTMP/RTMPS ingest.',
    icon: 'tv',
    available: function() {
      return hasProtocol('rtmp://') || hasProtocol('rtmps://');
    },
    targetLabel: 'Ingest URL',
    targetHelp: 'Enter the full RTMP or RTMPS ingest URL including the stream key.',
    placeholder: 'rtmp://ingest.example.com/live/streamkey',
    defaultTarget: '',
    matchTarget: function() { return false; },
    hint: 'Most platforms provide a <b>Server URL</b> and a <b>Stream key</b> in their live streaming settings. Combine them into one URL: paste the server URL and append the stream key.<ul>'
      + '<li><b>X (Twitter)</b> - <a href="https://studio.x.com" target="_blank">Media Studio</a> \u2192 Producer \u2192 create an RTMP source. URL and key are generated per broadcast.</li>'
      + '<li><b>Instagram</b> - <a href="https://www.instagram.com/live/producer" target="_blank">Live Producer</a> (requires a Professional account with 1,000+ followers). URL and key change per session.</li>'
      + '<li><b>TikTok</b> - <a href="https://livecenter.tiktok.com/producer" target="_blank">LIVE Producer</a> \u2192 copy the Server URL and Stream key. Access may require a Creator Network.</li>'
      + '<li><b>Restream / Castr / etc.</b> - check your multistreaming dashboard for the RTMP ingest URL and key.</li>'
      + '</ul>'
  }
};

const GROUPS = [
  { id: 'record',   title: 'Record' },
  { id: 'server',   title: 'Push to Server' },
  { id: 'platform', title: 'Push to Platform' }
];

const TEMPLATE_VARS = [
  { name: '$stream',       desc: 'Full stream name (e.g. live+alice)' },
  { name: '$basename',     desc: 'Base name only, before + (e.g. live)' },
  { name: '$wildcard',     desc: 'Wildcard part only, after + (e.g. alice)' },
  { name: '$pluswildcard',  desc: 'Wildcard with + prefix (e.g. +alice, or empty if no wildcard)' },
  { name: '$datetime', desc: 'Full timestamp (YYYY.MM.DD.HH.MM.SS)' },
  { name: '$year',     desc: 'Current year' },
  { name: '$month',    desc: 'Current month number' },
  { name: '$day',      desc: 'Current day number' },
  { name: '$yday',     desc: 'Day of year (1\u2013366)' },
  { name: '$hour',     desc: 'Hour when stream was received' },
  { name: '$minute',   desc: 'Minute when stream was received' },
  { name: '$seconds',  desc: 'Seconds when stream was received' },
  { name: '$segmentCounter',    desc: 'Sequential segment number (DVR)' },
  { name: '$currentMediaTime',  desc: 'First media timestamp of the segment (DVR)' }
];

// cached values, rebuilt when capabilities change
let _cache = null;

function getCache() {
  if (_cache) return _cache;
  _cache = buildTargetMatchLists();
  return _cache;
}

function invalidateCache() {
  _cache = null;
}

function buildTargetMatchLists() {
  const file_match = [];
  const prot_match = [];
  let connector2target_match = {};
  const writer_protocols = [];

  const connectors = (mist.data.capabilities && mist.data.capabilities.connectors) || {};
  for (const i in connectors) {
    const conn = connectors[i];
    if ('push_urls' in conn) {
      connector2target_match[i] = conn.push_urls;
      for (const j in conn.push_urls) {
        if (conn.push_urls[j][0] === '/') {
          file_match.push(conn.push_urls[j]);
        } else {
          prot_match.push(conn.push_urls[j]);
        }
      }
    }
  }

  if (mist.data.external_writer_list) {
    for (const k in mist.data.external_writer_list) {
      const writer = mist.data.external_writer_list[k];
      if (writer.length >= 3) {
        for (const p in writer[2]) {
          writer_protocols.push(writer[2][p] + '://');
        }
      }
    }
  }

  const internal = (mist.data.capabilities && mist.data.capabilities.internal_writers) || [];
  for (let w = 0; w < internal.length; w++) {
    writer_protocols.push(internal[w] + '://');
  }

  file_match.sort();
  prot_match.sort();

  return {
    file_match: file_match,
    prot_match: prot_match,
    connector2target_match: connector2target_match,
    writer_protocols: writer_protocols
  };
}

function hasProtocol(prefix) {
  const c = getCache();
  for (let i = 0; i < c.prot_match.length; i++) {
    if (c.prot_match[i].indexOf(prefix.replace('://*', '://')) === 0 ||
        prefix.indexOf(c.prot_match[i].replace('*', '')) === 0) {
      return true;
    }
  }
  return false;
}

function hasFileTargets() {
  const c = getCache();
  return c.file_match.length > 0;
}

function getWriterProtocols() {
  const c = getCache();
  return c.writer_protocols;
}

function hasWriterPrefix(target) {
  const wp = getWriterProtocols();
  for (let i = 0; i < wp.length; i++) {
    if (target.indexOf(wp[i]) === 0) return true;
  }
  return false;
}

function getMatchingConnector(target) {
  if (!target) return null;
  const c = getCache();
  for (const connector in c.connector2target_match) {
    const urls = c.connector2target_match[connector];
    for (const i in urls) {
      if (mistHelpers.inputMatch(urls[i], target)) {
        return connector;
      }
      if (urls[i][0] === '/') {
        for (let j = 0; j < c.writer_protocols.length; j++) {
          if (mistHelpers.inputMatch(c.writer_protocols[j] + urls[i].slice(1), target)) {
            return connector;
          }
        }
      }
    }
  }
  return null;
}

function validateTarget(target) {
  if (!target) return 'Target is required.';
  const c = getCache();
  for (let i = 0; i < c.prot_match.length; i++) {
    if (mistHelpers.inputMatch(c.prot_match[i], target)) return false;
  }
  for (let j = 0; j < c.file_match.length; j++) {
    if (mistHelpers.inputMatch(c.file_match[j], target)) return false;
    for (let k = 0; k < c.writer_protocols.length; k++) {
      if (mistHelpers.inputMatch(c.writer_protocols[k] + c.file_match[j].slice(1), target)) return false;
    }
  }
  return 'Does not match a valid push target.';
}

function detectScenario(target) {
  if (!target) return null;
  for (const id in SCENARIOS) {
    if (SCENARIOS[id].matchTarget && SCENARIOS[id].matchTarget(target)) return id;
  }
  if (target.indexOf('rtmp://') === 0 || target.indexOf('rtmps://') === 0) return 'push_rtmp';
  if (target.indexOf('srt://') === 0) return 'push_srt';
  if (target.indexOf('tsudp://') === 0 || target.indexOf('tstcp://') === 0 || target.indexOf('tsrtp://') === 0) return 'push_ts';
  if (target.charAt(0) === '/') return 'record_file';
  if (target.indexOf('://') >= 0) return 'push_other';
  return null;
}

function loadStreamList(callback) {
  apiClient.send(function(d) {
    let allthestreams = d.active_streams || [];

    const wildcards = [];
    for (const i in allthestreams) {
      if (allthestreams[i].indexOf('+') !== -1) {
        wildcards.push(allthestreams[i].replace(/\+.*/, '') + '+');
      }
    }
    allthestreams = allthestreams.concat(wildcards);

    let browserequests = 0;
    let browsecomplete = 0;
    for (const s in mist.data.streams) {
      allthestreams.push(s);
      if (mistHelpers.inputMatch(findInput('Folder').source_match, mist.data.streams[s].source)) {
        allthestreams.push(s + '+');
        apiClient.send(function(bd, opts) {
          const sn = opts.stream;
          for (const fi in bd.browse.files) {
            for (const inp in mist.data.capabilities.inputs) {
              if (inp.indexOf('Buffer') >= 0 || inp.indexOf('Folder') >= 0 ||
                  inp.indexOf('Buffer.exe') >= 0 || inp.indexOf('Folder.exe') >= 0) continue;
              if (mistHelpers.inputMatch(mist.data.capabilities.inputs[inp].source_match, '/' + bd.browse.files[fi])) {
                allthestreams.push(sn + '+' + bd.browse.files[fi]);
              }
            }
          }
          browsecomplete++;
          if (browserequests === browsecomplete) {
            callback(dedup(allthestreams));
          }
        }, { browse: mist.data.streams[s].source }, { stream: s });
        browserequests++;
      }
    }
    if (browserequests === browsecomplete) {
      callback(dedup(allthestreams));
    }
  }, { active_streams: 1 });
}

function dedup(arr) {
  return arr.filter(function(e, i, a) {
    return a.lastIndexOf(e) === i;
  }).sort();
}

function getValidFileExtensions() {
  const c = getCache();
  const exts = [];
  for (let i = 0; i < c.file_match.length; i++) {
    const m = c.file_match[i].match(/\/\*(\.\w+)$/);
    if (m) exts.push(m[1]);
  }
  return exts;
}

function getValidProtocols() {
  const c = getCache();
  return c.prot_match.map(function(p) {
    return p.replace(/\*$/, '');
  });
}

function formatCondition(variable, operator, value) {
  const ops = {
    0: 'is true', 1: 'is false',
    2: '==', 3: '!=',
    10: '>', 11: '>=', 12: '<', 13: '<=',
    20: '> (lex)', 21: '>= (lex)', 22: '< (lex)', 23: '<= (lex)'
  };
  const op = Number(operator);
  let str = '$' + variable + ' ' + (ops[op] || '??');
  if (op >= 2 && value) str += ' ' + value;
  return str;
}

function friendlyTarget(target) {
  if (!target) return '';
  for (const id in SCENARIOS) {
    if (SCENARIOS[id].targetPrefix && target.indexOf(SCENARIOS[id].targetPrefix) === 0) {
      return SCENARIOS[id].title;
    }
  }
  return target.split('?')[0];
}

function getIcon(target) {
  const id = detectScenario(target);
  if (!id) return null;
  return SCENARIOS[id].icon || null;
}

function getLabel(target) {
  const id = detectScenario(target);
  if (!id) return '';
  return SCENARIOS[id].title || '';
}

export const pushScenarios = {
  SCENARIOS: SCENARIOS,
  GROUPS: GROUPS,
  TEMPLATE_VARS: TEMPLATE_VARS,
  buildTargetMatchLists: buildTargetMatchLists,
  getMatchingConnector: getMatchingConnector,
  validateTarget: validateTarget,
  detectScenario: detectScenario,
  loadStreamList: loadStreamList,
  invalidateCache: invalidateCache,
  getValidFileExtensions: getValidFileExtensions,
  getValidProtocols: getValidProtocols,
  getCache: getCache,
  formatCondition: formatCondition,
  friendlyTarget: friendlyTarget,
  getIcon: getIcon,
  getLabel: getLabel
};
