import { APP_NAME } from '@brand';
import { el } from '../core/dom_helpers.js';
import { navto } from '../core/navigation.js';
import { editStream } from '../streams/streams.js';

var PAGE_HINTS = {
  Overview: [
    { icon: 'settings', text: 'Use the Protocols page to configure which output formats are available to your viewers.' },
    { icon: 'list', text: 'Right-click any stream on the Streams page for quick actions like preview, embed code, or deletion.' },
    { icon: 'hard-drive', text: 'Download a backup of your current configuration from the header actions on this page.' },
    { icon: 'up', text: 'Upload a saved configuration to restore a backup or migrate settings from another instance.' },
    { icon: 'activity', text: 'The overview refreshes automatically - active stream and viewer counts update in real time.' },
    { icon: 'globe', text: 'Click any protocol name in the Enabled/Disabled lists to jump to the Protocols configuration page.' },
    { icon: 'zap', text: APP_NAME+' supports rolling updates - replace binaries while running and new connections route to the updated version automatically.' },
    { icon: 'hard-drive', text: 'Configuration auto-saves one minute after the last change, and reloads if it detects external edits to the config file.' }
  ],
  Streams: [
    { icon: 'list', text: 'Right-click any stream for quick actions like preview, embed code, or deletion.' },
    { icon: 'folder', text: 'Use folders to serve a directory of media files as wildcard sub-streams - each file becomes streamname+filename.' },
    { icon: 'zap', text: 'Add processes to a stream for on-the-fly transcoding, compositing, or AI analysis.' },
    { icon: 'upload', text: 'Push streams accept incoming media from encoders like OBS, vMix, or FFmpeg via RTMP, SRT, or WebRTC (WHIP).' },
    { icon: 'camera', text: 'Pull from IP cameras using RTSP, or capture local devices with V4L2 or NDI.' },
    { icon: 'layout', text: 'Use Compose to combine multiple streams into a single multiview or composited output.' },
    { icon: 'repeat', text: 'Enable Always On to keep a stream active even when no viewers are connected - required for DVR and RTSP cameras.' },
    { icon: 'globe', text: 'Pull streams fetch media from a remote URL on demand - great for restreaming or proxying.' },
    { icon: 'search', text: 'Use the filter bar to quickly find streams by name or source type.' },
    { icon: 'settings', text: 'Switch between thumbnail and list view using the view toggle in the toolbar.' },
    { icon: 'key', text: 'Stream keys let you restrict who can push to a stream - configure them in the stream\u2019s settings.' },
    { icon: 'shield', text: 'Use passphrases or IP whitelists (CIDR notation) in the push:// source to control who can ingest.' },
    { icon: 'file', text: 'VoD streams serve a media file for on-demand playback - MP4, MKV, FLV, TS, and more are supported.' },
    { icon: 'cast', text: 'NDI sources are auto-discovered on the local network. Select from the dropdown or type a custom name.' },
    { icon: 'activity', text: 'Stream health is monitored in real time - check the Status page for bitrate, codec, and error details.' },
    { icon: 'code', text: 'Stream names are limited to 100 characters: lowercase a\u2013z, digits, underscores, dashes, and periods only.' },
    { icon: 'hard-drive', text: 'MP4 and MKV inputs support S3 sources directly - use s3+http:// or s3+https:// as the source URL.' },
    { icon: 'plus', text: 'Wildcard streams use + as the separator (streamname+identifier). The total length must stay under 100 bytes.' },
    { icon: 'users', text: 'Push streams are automatic wildcard templates - push to streamname+anything to create independent child streams that inherit the parent config.' },
    { icon: 'shield', text: 'Use PUSH_REWRITE triggers to authenticate incoming pushes and remap them to wildcard stream names for secure multi-user ingest.' }
  ],
  Status: [
    { icon: 'activity', text: 'The status page shows real-time stream health including bitrate, codec, and connection details.' },
    { icon: 'eye', text: 'Preview the stream directly from this page to verify what viewers will see.' },
    { icon: 'hard-drive', text: 'Start a new push or recording directly from the stream status page.' },
    { icon: 'zap', text: 'Active processes like transcoding or AI analysis are listed with their current state.' },
    { icon: 'users', text: 'See which viewers are currently connected, with session details and connection info.' },
    { icon: 'code', text: 'Use track selectors to control which audio/video tracks are delivered - e.g. for multi-language streams.' }
  ],
  Preview: [
    { icon: 'eye', text: 'The preview player uses the same technology as your viewers - what you see is what they get.' },
    { icon: 'settings', text: 'Use the protocol selector to test different output formats (HLS, DASH, WebRTC, etc.).' },
    { icon: 'monitor', text: 'Open the preview in a new window for a full-screen, distraction-free view.' },
    { icon: 'zap', text: 'WebRTC preview gives the lowest latency - ideal for verifying real-time interactions.' }
  ],
  Embed: [
    { icon: 'code', text: 'Copy the embed code to place a player on any web page with a single script tag.' },
    { icon: 'globe', text: 'Set a base URL if '+APP_NAME+' is behind a reverse proxy so embed URLs point to the right address.' },
    { icon: 'settings', text: 'The player is designed for white-labelling - use built-in skins, custom CSS, or your own component library.' },
    { icon: 'scissors', text: 'Clip and download stream segments by adding ?startunix, ?duration, and ?dl=1 to a stream URL.' },
    { icon: 'book', text: 'Check the NPM package @optimist-video/mistplayer-core for advanced player integration.' },
    { icon: 'eye', text: 'The embed player auto-selects the best protocol for each viewer\u2019s browser and network conditions.' },
    { icon: 'monitor', text: 'Player skins and keyboard shortcuts are fully configurable via the MistVideoObject API.' },
    { icon: 'shield', text: 'Embed URLs must match the page protocol - mixing HTTP and HTTPS causes browser mixed-content errors.' }
  ],
  Protocols: [
    { icon: 'shield', text: 'Enable HTTPS for secure playback, or terminate TLS at a reverse proxy.' },
    { icon: 'plus', text: 'Some protocols support multiple instances - click a protocol to see options.' },
    { icon: 'settings', text: 'Set a public address if '+APP_NAME+' is behind a reverse proxy, so playback URLs are correct.' },
    { icon: 'globe', text: 'HLS and DASH are the most widely supported formats for web and mobile playback.' },
    { icon: 'zap', text: 'WebRTC offers the lowest latency (<1s) - ideal for interactive or real-time applications.' },
    { icon: 'monitor', text: 'RTSP output lets other servers or software pull streams directly from '+APP_NAME+'.' },
    { icon: 'toggle-left', text: 'Click on a protocol card to configure it, or use the toggle to enable or disable.' },
    { icon: 'hard-drive', text: 'DVR-style seeking in live streams requires TS-based segmented recording with an M3U8 playlist.' },
    { icon: 'file', text: 'HTTP Progressive (HTTPTS, FLV) provides simple low-latency streaming for compatible players.' },
    { icon: 'key', text: 'Use certbot or your own PEM certificates to enable HTTPS directly in '+APP_NAME+'.' },
    { icon: 'zap', text: 'WebRTC requires B-frames to be disabled in the encoder - they cause stutters and visual artifacts.' },
    { icon: 'settings', text: 'SRT: use separate ports for input and output to avoid a ~3-second latency penalty on some systems.' },
    { icon: 'code', text: 'Default ports: HTTP 8080, HTTPS 4433, RTMP 1935, RTSP 5554, SRT 8889, API 4242.' },
    { icon: 'shield', text: 'SRT supports Forward Error Correction (FEC) and passphrase-based encryption for secure delivery.' },
    { icon: 'shield', text: 'RIST supports AES-128 and AES-256 encryption with automatic key rotation.' },
    { icon: 'globe', text: 'DTSC is the most efficient protocol for pulling streams between '+APP_NAME+' instances.' }
  ],
  Push: [
    { icon: 'hard-drive', text: 'Record as .ts or .mkv for crash resilience. Fragmented .fmp4 also survives interrupted writes.' },
    { icon: 'repeat', text: 'Automatic pushes start whenever their matched stream goes live and retry on failure.' },
    { icon: 'variable', text: 'Use variables like $stream, $datetime, $day, and $currentMediaTime in recording paths for dynamic file names.' },
    { icon: 'globe', text: 'Push to external platforms like YouTube, Twitch, or Facebook using RTMP or RTMPS targets.' },
    { icon: 'zap', text: 'SRT pushes provide reliable, low-latency delivery over unpredictable networks.' },
    { icon: 'file', text: 'TS and MKV recordings survive interrupted writes - ideal for always-on recording.' },
    { icon: 'list', text: 'Click a section header to collapse or expand it for a cleaner view.' },
    { icon: 'settings', text: 'Automatic push rules use wildcards - a single rule can match many streams at once.' },
    { icon: 'variable', text: 'Use streamname+ (with trailing +) in auto-push rules to match all wildcard children. Combine with $wildcard in file paths for per-user recordings.' },
    { icon: 'clock', text: 'Schedule recordings using custom variables with date-checking logic, or calendar-based schedules.' },
    { icon: 'scissors', text: 'Use the clipping API to extract time-based segments without re-encoding (requires '+APP_NAME+' 3.3+).' },
    { icon: 'settings', text: 'Use split=N in push targets to restart recording every ~N seconds, aligned to the nearest keyframe.' },
    { icon: 'hard-drive', text: 'DVR recording requires TS format - it\u2019s the only format supported on the input side for DVR playback.' },
    { icon: 'file', text: 'Regular MP4 must finalize headers on close - a crash means a broken file. Use .ts, .mkv, or .fmp4 instead.' },
    { icon: 'repeat', text: 'Stopping an automatic push that still matches will cause it to reactivate. Use $variables in file names to avoid overwrites.' }
  ],
  'Push Config': [
    { icon: 'hard-drive', text: 'Choose .ts or .mkv for recordings that need to survive crashes or power loss. Fragmented .fmp4 is also resilient.' },
    { icon: 'variable', text: 'Use $stream, $datetime, $day, and $currentMediaTime in file paths to generate unique file names automatically.' },
    { icon: 'globe', text: 'For RTMP targets, include the stream key in the URL path: rtmp://host/app/streamkey.' },
    { icon: 'settings', text: 'Some parameters only apply to file-based targets and are hidden for network protocols.' },
    { icon: 'file', text: 'Regular MP4 needs to finalize headers on close - a crash means a broken file. Use .ts, .mkv, or .fmp4 for long recordings.' },
    { icon: 'repeat', text: 'For 24/7 DVR, add noendlist=1 and append=1 to keep playlists usable across recording restarts.' },
    { icon: 'code', text: 'Combine split=N with m3u8=path/playlist.m3u8 for seekable segmented recordings.' },
    { icon: 'globe', text: 'For RTMPS targets (e.g. Facebook), add a trailing ? to the URL to prevent parameter parsing.' }
  ],
  General: [
    { icon: 'settings', text: 'Set the debug level higher to see more detail in the logs. Level 3 is recommended for production.' },
    { icon: 'key', text: 'Add JSON Web Keys to enable JWT-based authentication - valid tokens bypass all triggers and security hooks.' },
    { icon: 'activity', text: 'Configure Prometheus stats output under General to export metrics for Grafana or other dashboards.' },
    { icon: 'users', text: 'Session settings control how viewers are identified - a new session starts when stream name, token, or IP changes.' },
    { icon: 'variable', text: 'Custom variables come in two types: static (fixed text, 63-char limit) and dynamic (execute commands on a schedule, 511-char limit).' },
    { icon: 'globe', text: 'Load balancer settings let multiple '+APP_NAME+' instances share the workload.' },
    { icon: 'hard-drive', text: 'External writers route stream data to an external command via stdin - useful for custom outputs like S3 uploads or HTTP PUT.' },
    { icon: 'shield', text: 'JWT authentication validates tokens using configured JWKs, without needing an external trigger endpoint.' },
    { icon: 'hard-drive', text: 'Configuration auto-saves one minute after the last change, and reloads if it detects external edits to the config file.' },
    { icon: 'shield', text: 'Localhost API connections skip authentication entirely. Remote access uses MD5 challenge-response.' },
    { icon: 'clock', text: 'JWT-approved sessions persist up to 10 minutes after disconnect. Use the invalidate_sessions API call to revoke access immediately.' }
  ],
  Automation: [
    { icon: 'zap', text: 'Use USER_NEW triggers to authenticate viewers before they can watch.' },
    { icon: 'shield', text: 'Triggers can call external URLs (HTTP POST with X-Trigger header) or run local executables with the payload on stdin.' },
    { icon: 'repeat', text: 'Triggers fire on events like stream start, viewer connect, push start, recording end, and more - 32 types in total.' },
    { icon: 'settings', text: 'Use PUSH_REWRITE triggers to dynamically remap incoming push stream names or reject unauthorised pushes.' },
    { icon: 'globe', text: 'HTTP trigger handlers receive a POST with the trigger name in the X-Trigger header and event details in the body.' },
    { icon: 'list', text: 'Filter triggers by type to quickly find the ones you need.' },
    { icon: 'users', text: 'USER_END triggers fire when a viewer disconnects - useful for analytics or session cleanup.' },
    { icon: 'activity', text: 'STREAM_BUFFER triggers fire when a stream\u2019s buffer state changes (fill, empty, etc.).' },
    { icon: 'code', text: 'A trigger response is positive if it starts with 1, yes, true, or cont - anything else denies the request.' },
    { icon: 'hard-drive', text: 'RECORDING_END triggers fire when a file recording finishes, with details like bytes written, duration, and exit reason.' },
    { icon: 'key', text: 'STREAM_PUSH triggers fire before an incoming push is accepted - return true to allow or false to reject.' },
    { icon: 'shield', text: 'Triggers can be scoped globally (all streams) or limited to specific streams for fine-grained control.' },
    { icon: 'users', text: 'PUSH_REWRITE triggers can remap incoming push URLs to wildcard stream names - use this for token-based authentication on multi-user platforms.' }
  ],
  Logs: [
    { icon: 'settings', text: 'Increase the debug level on the General page to see more detail in the logs.' },
    { icon: 'activity', text: 'Log entries are colour-coded by severity - errors and failures are highlighted in red.' },
    { icon: 'search', text: 'Use the filter input to search logs by keyword, stream name, or error message.' },
    { icon: 'external-link', text: 'Open logs in a raw window for easier copying, searching, and sharing.' },
    { icon: 'down', text: 'Click the snap-to-bottom button to auto-scroll to the latest log entries.' },
    { icon: 'terminal-square', text: 'Debug levels: 0 = off, 1 = failures, 2 = errors, 3 = warnings (recommended for production), 4\u201310 = increasing developer detail.' }
  ],
  Statistics: [
    { icon: 'activity', text: 'Statistics for inactive sessions are retained for approximately 10 minutes.' },
    { icon: 'users', text: 'Viewer Analytics shows connection counts and protocols used. Geographic data requires a separate GeoIP setup.' },
    { icon: 'list', text: 'Per-Stream stats let you compare bandwidth and viewer counts across all active streams.' },
    { icon: 'settings', text: 'Export metrics to Prometheus for long-term storage and custom Grafana dashboards.' },
    { icon: 'globe', text: 'Use the API base URL shown in the intro to build custom stats integrations.' },
    { icon: 'code', text: APP_NAME+'\u2019s stats are available in JSON and Prometheus output formats on the API port.' },
    { icon: 'key', text: 'Prometheus export requires a passphrase configured under General - access it at /passphrase on the API port.' }
  ],
  'Stream Config': [
    { icon: 'settings', text: 'The wizard guides you through source, settings, access, and processes step by step.' },
    { icon: 'zap', text: 'Processes run on the stream in real time - transcode, overlay, analyse, or composite.' },
    { icon: 'repeat', text: 'Always On keeps the stream connected even when no viewers are watching - required for DVR and RTSP cameras.' },
    { icon: 'shield', text: 'Use the Access step to restrict who can push to this stream by IP (CIDR notation) or passphrase.' },
    { icon: 'camera', text: 'For RTSP cameras, enable Always On to keep the connection alive continuously.' },
    { icon: 'zap', text: 'SRT supports Caller and Listener modes - Caller connects out, Listener waits for connections.' },
    { icon: 'key', text: 'SRT passphrases must be between 10 and 79 characters. Connections fail silently on mismatch.' },
    { icon: 'code', text: 'Push source syntax: push://[host][@passphrase] - host supports CIDR notation like 192.168.0.0/16.' }
  ]
};

var GENERIC_HINTS = [
  { icon: 'book', text: 'Visit the '+APP_NAME+' documentation for guides on setting up live streaming, VOD, and more.' },
  { icon: 'shield', text: 'Enable HTTPS for secure playback - either directly in '+APP_NAME+' or via a reverse proxy with certbot.' },
  { icon: 'zap', text: APP_NAME+' supports ultra-low-latency delivery via WebRTC (WHEP) for real-time applications.' },
  { icon: 'globe', text: APP_NAME+' automatically negotiates the best protocol for each viewer\u2019s device and network.' },
  { icon: 'key', text: 'JWT keys let you authenticate viewers without trigger scripts. Triggers offer more control but require a handler endpoint.' },
  { icon: 'repeat', text: 'Automatic pushes can record every stream that goes live - no manual intervention needed.' },
  { icon: 'hard-drive', text: 'Download your configuration as a backup from the Overview page before making big changes.' },
  { icon: 'layout', text: 'Use the Compose scenario to combine multiple streams into a single multiview output.' },
  { icon: 'monitor', text: APP_NAME+' can pull from RTSP cameras, NDI sources, V4L2 devices, and SRT/RIST feeds.' },
  { icon: 'file', text: 'Serve a folder of media files as on-demand wildcard sub-streams for easy VoD delivery.' },
  { icon: 'variable', text: 'Custom variables defined under General can be referenced in push targets and trigger URLs.' },
  { icon: 'settings', text: 'Right-click most UI elements for contextual quick actions.' },
  { icon: 'code', text: 'The '+APP_NAME+' player supports white-labelling - bring your own CSS, skins, or component library.' },
  { icon: 'activity', text: 'Check the Logs page for real-time server output, filterable by keyword or severity.' },
  { icon: 'upload', text: 'Ingest live streams via RTMP, SRT, WebRTC (WHIP), RTSP, RIST, or direct TS push.' },
  { icon: 'key', text: 'Stream keys add an extra layer of ingest security - only encoders with the right key can push.' },
  { icon: 'scissors', text: 'Use the clipping API to extract time-based segments from recordings without re-encoding.' },
  { icon: 'cast', text: 'NDI sources are auto-discovered on the local network for zero-config video capture.' },
  { icon: 'clock', text: 'Use $datetime and $day variables in push paths for automatic date-stamped recordings.' },
  { icon: 'eye', text: 'Preview any stream from the Streams page to verify output quality before going live.' },
  { icon: 'terminal-square', text: APP_NAME+' exposes a full JSON API on port 4242 for automation - manage streams, pushes, and config programmatically.' },
  { icon: 'hard-drive', text: 'DVR-style seeking in live streams requires TS-based segmented recording - not a built-in protocol feature.' },
  { icon: 'zap', text: 'Processes can transcode, analyse with AI, overlay graphics, or transform streams in real time.' },
  { icon: 'users', text: 'The Statistics page tracks viewer counts, protocols, and bandwidth in real time.' },
  { icon: 'monitor', text: APP_NAME+' runs on Linux, macOS, and Windows - or in Docker for easy deployment.' },
  { icon: 'shield', text: 'Reverse proxies must forward the X-Real-IP header - without it, '+APP_NAME+' sees all connections as localhost.' },
  { icon: 'key', text: 'Valid JWTs bypass all triggers and security hooks. Use the invalidate_sessions API call to revoke access immediately.' },
  { icon: 'shield', text: APP_NAME+' does not hot-reload SSL certificates. After updating certs, restart the HTTPS connector to apply them.' },
  { icon: 'globe', text: 'Multiple API commands can be combined in a single request for efficient automation.' },
  { icon: 'file', text: 'Add ?dl=1 to any stream URL to download it as a file instead of streaming it.' }
];

function shuffle(arr) {
  for (var i = arr.length - 1; i > 0; i--) {
    var j = Math.floor(Math.random() * (i + 1));
    var tmp = arr[i];
    arr[i] = arr[j];
    arr[j] = tmp;
  }
  return arr;
}

function getOverviewStateHints() {
  var hints = [];
  if (typeof mist === 'undefined' || !mist.data || !mist.data.config) return hints;

  var streamCount = mist.data.streams ? Object.keys(mist.data.streams).length : 0;
  var activeCount = mist.data.active_streams ? mist.data.active_streams.length : 0;
  var protocolCount = mist.data.config.protocols ? mist.data.config.protocols.length : 0;
  var httpsConfigured = false;
  if (mist.data.config.protocols) {
    for (var i = 0; i < mist.data.config.protocols.length; i++) {
      if (/^HTTPS(\.exe)?$/i.test(mist.data.config.protocols[i].connector)) {
        httpsConfigured = true;
        break;
      }
    }
  }

  if (streamCount === 0) {
    hints.push({
      icon: 'plus',
      text: 'No streams configured yet. Create your first stream to start publishing or serving media.',
      action: { label: 'Create a stream', fn: function(){ editStream(''); } }
    });
  } else if (activeCount === 0) {
    hints.push({
      icon: 'activity',
      text: 'You have ' + streamCount + ' stream' + (streamCount === 1 ? '' : 's') + ' configured but none are active. Push data to a stream or enable always-on.',
      action: { label: 'View streams', fn: function(){ navto('Streams'); } }
    });
  }
  if (protocolCount === 0) {
    hints.push({
      icon: 'alert-triangle',
      text: 'No protocols are enabled. Viewers won\'t be able to connect until you enable at least one output protocol.',
      action: { label: 'Configure protocols', fn: function(){ navto('Protocols'); } }
    });
  }
  if (!httpsConfigured) {
    hints.push({
      icon: 'shield',
      text: 'HTTPS is not configured. For secure playback, either enable the HTTPS protocol in '+APP_NAME+' or place '+APP_NAME+' behind a reverse proxy that terminates TLS.',
      action: { label: 'Set up HTTPS', fn: function(){ navto('Protocols'); } }
    });
  }
  if (mist.data.update && !mist.data.update.uptodate && mist.data.update.version) {
    hints.push({
      icon: 'down',
      text: 'Version ' + mist.data.update.version + ' is available.'
    });
  }

  return hints;
}

function getHintsForPage(page) {
  var hints = [];

  if (page === 'Overview') {
    hints = hints.concat(getOverviewStateHints());
  }

  var pageSpecific = (PAGE_HINTS[page] || []).slice();
  var generic = GENERIC_HINTS.slice();

  shuffle(pageSpecific);
  shuffle(generic);

  hints = hints.concat(pageSpecific);
  hints = hints.concat(generic);
  return hints;
}

var _footer = null;
var _hintContent = null;
var _hintProgress = null;
var _hintIndex = 0;
var _allHints = [];

function showHint(index) {
  if (!_allHints.length) { _footer.hidden = true; return; }
  _footer.hidden = false;
  var hint = _allHints[index % _allHints.length];
  _hintContent.classList.remove('hint-visible');
  setTimeout(function() {
    _hintContent.innerHTML = '';
    var iconSpan = el('span', {class: 'hint-icon'});
    iconSpan.setAttribute('data-icon', hint.icon);
    _hintContent.appendChild(iconSpan);
    var textSpan = el('span', {class: 'hint-text'});
    textSpan.textContent = hint.text;
    _hintContent.appendChild(textSpan);
    if (hint.action) {
      var actionBtn = el('button');
      actionBtn.textContent = hint.action.label;
      actionBtn.addEventListener('click', hint.action.fn);
      _hintContent.appendChild(actionBtn);
    }
    _hintContent.classList.add('hint-visible');
  }, 200);

  _hintProgress.innerHTML = '';
  if (_allHints.length > 1) {
    var bar = el('div', {class: 'hint-progress-bar'});
    bar.addEventListener('animationend', function() {
      _hintIndex = (_hintIndex + 1) % _allHints.length;
      showHint(_hintIndex);
    });
    _hintProgress.appendChild(bar);
  }
}

export function getHintsFooter() {
  if (!_footer) {
    _footer = el('div', {class: 'global-hints-footer overview-hints'});
    _hintContent = el('div', {class: 'hint-content'});
    _hintProgress = el('div', {class: 'hint-progress'});
    _footer.appendChild(_hintContent);
    _footer.appendChild(_hintProgress);
    _footer.addEventListener('mouseenter', function() { _footer.classList.add('paused'); });
    _footer.addEventListener('mouseleave', function() { _footer.classList.remove('paused'); });
  }
  return _footer;
}

export function updateHintsFooter(page) {
  if (!_footer) return;
  _allHints = getHintsForPage(page);
  _hintIndex = 0;
  showHint(_hintIndex);
}
