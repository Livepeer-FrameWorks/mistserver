import { APP_NAME } from '@brand';

function hasInput(name) {
  if (!mist.data.capabilities || !mist.data.capabilities.inputs) return false;
  for (const i in mist.data.capabilities.inputs) {
    if (i.toLowerCase().indexOf(name.toLowerCase()) >= 0) return true;
  }
  return false;
}

function getInput(name) {
  if (!mist.data.capabilities || !mist.data.capabilities.inputs) return null;
  for (const i in mist.data.capabilities.inputs) {
    if (i.toLowerCase().indexOf(name.toLowerCase()) >= 0)
      return mist.data.capabilities.inputs[i];
  }
  return null;
}

function detectScenario(source) {
  if (!source) return null;
  if (source.indexOf('push://') === 0) return 'push';
  if (source === 'multiview' || source === 'compose' || source.indexOf('multiview:') === 0 || source.indexOf('compose:') === 0) return 'compose';
  if (source.indexOf('srt://') === 0) return 'srt';
  if (source.indexOf('rist://') === 0) return 'rist';
  if (source.indexOf('rtsp://') === 0) return 'rtsp';
  if (source.indexOf('ndi:') === 0) return 'ndi';
  if (source.indexOf('v4l2:') === 0 || source.indexOf('/dev/video') === 0) return 'v4l2';
  if (source.match(/^folder:/) || (source.charAt(0) === '/' && source.charAt(source.length - 1) === '/')) return 'folder';
  if (source.charAt(0) === '/') return 'file';
  if (source.indexOf('://') >= 0) return 'pull';
  return null;
}

function getAllFiletypes() {
  const ft = [];
  if (!mist.data.capabilities || !mist.data.capabilities.inputs) return ft;
  for (const i in mist.data.capabilities.inputs) {
    let sm = mist.data.capabilities.inputs[i].source_match;
    if (!sm) continue;
    if (typeof sm === 'string') sm = [sm];
    for (let j = 0; j < sm.length; j++) ft.push(sm[j]);
  }
  return ft;
}

const SCENARIOS = {
  push: {
    group: 'receive', title: 'Push Stream',
    desc: 'Receive a live stream pushed into '+APP_NAME+' via RTMP, SRT, WebRTC, or other protocols.',
    hint: 'Your stream will listen for incoming media from encoders like OBS, vMix, or FFmpeg. Ingest via RTMP, SRT, WebRTC (WHIP), or RTSP.',
    icon: 'upload',
    available: function() { return true; },
    defaultSource: 'push://',
    lockedPrefix: 'push://',
    showAlwaysOn: false,
    sourceLabel: 'Only allow pushes from address:',
    sourceHelp: 'Optionally restrict ingest to a single source address (whitelist). Leave empty to accept the default ingest endpoint from any address.'
  },
  rtsp: {
    group: 'connect', title: 'RTSP Camera',
    desc: 'Pull a stream from an IP camera or other RTSP source.',
    hint: APP_NAME+' will pull the stream from the camera and make it available for delivery. Set Always On to keep the connection active.',
    icon: 'camera',
    available: function() { return hasInput('RTSP'); },
    defaultSource: 'rtsp://',
    lockedPrefix: 'rtsp://',
    sourceLabel: 'Camera address',
    sourceHelp: 'Enter the camera address. Example: user:password@192.168.1.100:554/stream',
    placeholder: 'user:password@host:554/path',
    showAlwaysOn: true, alwaysOnDefault: true, inputName: 'RTSP'
  },
  ndi: {
    group: 'connect', title: 'NDI Source',
    desc: 'Capture video from an NDI source on your local network.',
    hint: 'Select an NDI source discovered on the network. '+APP_NAME+' will capture audio and video from it in real time.',
    icon: 'cast',
    available: function() { return hasInput('NDI'); },
    defaultSource: 'ndi:',
    enumerate: true, showAlwaysOn: true, inputName: 'NDI'
  },
  v4l2: {
    group: 'connect', title: 'V4L2 Device',
    desc: 'Capture from a local video device (Linux).',
    hint: 'Select a local video capture device. This is typically a webcam or capture card connected to the server.',
    icon: 'video',
    available: function() { return hasInput('V4L2'); },
    defaultSource: 'v4l2:',
    enumerate: true, inputName: 'V4L2'
  },
  sdi: {
    group: 'connect', title: 'SDI Input',
    desc: 'Professional SDI video capture.',
    icon: 'monitor',
    available: function() { return hasInput('SDI'); },
    promo: true,
    promoText: 'SDI input requires additional licensing.',
    promoLink: 'https://mistserver.org/contact'
  },
  srt: {
    group: 'connect', title: 'SRT',
    desc: 'Secure Reliable Transport for low-latency streaming over unpredictable networks.',
    hint: 'SRT provides reliable, low-latency transport over lossy networks. Ideal for contribution feeds across the internet.',
    icon: 'zap',
    available: function() { return hasInput('TSSRT'); },
    defaultSource: 'srt://',
    lockedPrefix: 'srt://',
    sourceLabel: 'SRT address',
    sourceHelp: 'SRT supports two connection modes:<br><b>Caller</b> - '+APP_NAME+' connects to a remote SRT endpoint. Example: <code>hostname:port</code><br><b>Listener</b> - '+APP_NAME+' waits for incoming connections on a local port. Example: <code>:port</code>',
    placeholder: 'hostname:port',
    showAlwaysOn: true, inputName: 'TSSRT'
  },
  rist: {
    group: 'connect', title: 'RIST',
    desc: 'Reliable Internet Stream Transport for broadcast-quality delivery.',
    hint: 'RIST provides broadcast-grade reliability with ARQ-based error recovery. Suitable for professional contribution links.',
    icon: 'zap',
    available: function() { return hasInput('TSRIST'); },
    defaultSource: 'rist://',
    lockedPrefix: 'rist://',
    sourceLabel: 'RIST address',
    sourceHelp: 'RIST supports two connection modes:<br><b>Caller</b> - '+APP_NAME+' connects to a remote RIST endpoint. Example: <code>hostname:port</code><br><b>Listener</b> - '+APP_NAME+' waits for incoming connections on a local port. Example: <code>:port</code>',
    placeholder: 'hostname:port',
    showAlwaysOn: true, inputName: 'TSRIST'
  },
  file: {
    group: 'serve', title: 'File / VoD',
    desc: 'Serve a media file for on-demand playback.',
    hint: 'Serve a media file for Video-on-Demand playback. Supports MP4, MKV, FLV, TS, and other formats.',
    icon: 'file',
    available: function() { return true; },
    defaultSource: '/',
    sourceLabel: 'File path',
    sourceHelp: 'Select a media file on the server. Use the browse button to navigate.',
    useBrowse: true, showAlwaysOn: true, showProcesses: false
  },
  folder: {
    group: 'serve', title: 'Folder',
    desc: 'Serve a folder of media files as on-demand wildcard streams.',
    hint: 'Point to a folder of media files. Each file becomes a wildcard sub-stream, accessible as streamname+filename.',
    icon: 'folder',
    available: function() { return hasInput('Folder'); },
    defaultSource: '/',
    sourceLabel: 'Folder path',
    sourceHelp: 'Enter the path to a folder of media files. The path should end with /.',
    useBrowse: true, showProcesses: false
  },
  compose: {
    group: 'receive', title: 'Compose / Multiview',
    desc: 'Combine multiple streams into a single composited output with a visual layout designer.',
    hint: 'Create a multiview layout combining multiple input streams. Use the visual designer to arrange sources, set a resolution, and pick a layout mode (grid, focussed, or freestyle). The composed output becomes this stream.',
    icon: 'layout',
    available: function() {
      if (!mist.data.capabilities || !mist.data.capabilities.processes) return false;
      for (var i in mist.data.capabilities.processes) {
        if (i.toLowerCase().indexOf('composer') >= 0) return true;
      }
      return false;
    },
    defaultSource: 'compose',
    showAlwaysOn: true, alwaysOnDefault: true,
    showProcesses: false,
    isCompose: true
  },
  pull: {
    group: 'serve', title: 'Pull URL',
    desc: 'Pull a stream from a remote URL (HTTP, HTTPS, RTMP, and more).',
    hint: APP_NAME+' will pull from the given URL on demand. Supports HTTP, HTTPS, RTMP, and other protocols.',
    icon: 'globe',
    available: function() { return true; },
    defaultSource: '',
    sourceLabel: 'Source URL',
    sourceHelp: 'Enter the URL to pull media from. '+APP_NAME+' auto-detects the protocol.',
    placeholder: 'https://example.com/stream.m3u8',
    useBrowse: true, showProcesses: false
  }
};

const GROUPS = [
  { id: 'receive', title: 'Receive' },
  { id: 'connect', title: 'Connect' },
  { id: 'serve',   title: 'Serve' }
];

export const streamScenarios = {
  SCENARIOS: SCENARIOS,
  GROUPS: GROUPS,
  hasInput: hasInput,
  getInput: getInput,
  detectScenario: detectScenario,
  getAllFiletypes: getAllFiletypes
};
