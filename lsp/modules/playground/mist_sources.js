/**
 * Convert MistServer source JSON to FrameWorks ContentEndpoints.
 * Ported from FrameWorks playground mist-utils.ts.
 */

import { getHttpHost } from './api.js';

const PROTOCOL_PRIORITY = {
  HLS: 1,
  DASH: 2,
  MP4: 3,
  WEBM: 4,
  WHEP: 5,
  RTSP: 10,
  MIST_WEBRTC: 50,
  MEWS_WS: 99,
};

export function mapMistTypeToProtocol(mistType) {
  if (mistType.startsWith('ws/') || mistType.startsWith('wss/')) return 'MEWS_WS';
  if (mistType.includes('webrtc')) return 'MIST_WEBRTC';
  if (mistType.includes('mpegurl') || mistType.includes('m3u8')) return 'HLS';
  if (mistType.includes('dash') || mistType.includes('mpd')) return 'DASH';
  if (mistType.includes('whep')) return 'WHEP';
  if (mistType.includes('mp4')) return 'MP4';
  if (mistType.includes('webm')) return 'WEBM';
  if (mistType.includes('rtsp')) return 'RTSP';
  return mistType;
}

export function buildContentEndpoints(sources, streamName) {
  if (!sources || !sources.length) return null;

  const outputs = {};
  for (const s of sources) {
    const proto = mapMistTypeToProtocol(s.type);
    if (!outputs[proto]) outputs[proto] = {protocol: proto, url: s.url};
  }

  // Auto-select: prefer HTTP protocols, sorted by priority
  const httpSources = sources.filter(function(s) { return !s.url.startsWith('ws://'); });
  let primary;
  if (httpSources.length) {
    primary = httpSources.sort(function(a, b) {
      const pa = PROTOCOL_PRIORITY[mapMistTypeToProtocol(a.type)] || 50;
      const pb = PROTOCOL_PRIORITY[mapMistTypeToProtocol(b.type)] || 50;
      return pa - pb;
    })[0];
  } else {
    primary = sources[0];
  }
  if (!primary) return null;

  return {
    primary: {
      nodeId: 'mist-' + streamName,
      protocol: mapMistTypeToProtocol(primary.type),
      url: primary.url,
      baseUrl: getHttpHost(),
      outputs: outputs,
    },
    fallbacks: [],
  };
}

export function getSourceTable(sources) {
  if (!sources || !sources.length) return [];
  return sources.map(function(s) {
    return {
      protocol: mapMistTypeToProtocol(s.type),
      hrn: s.hrn || s.type,
      url: s.url,
      priority: s.priority,
    };
  });
}

export function buildIngestUris(streamName, httpHost) {
  let hostname;
  try { hostname = new URL(httpHost).hostname; } catch (e) { hostname = location.hostname; }
  const base = httpHost || location.origin;
  return {
    rtmp: 'rtmp://' + hostname + ':1935/live/' + streamName,
    srt: 'srt://' + hostname + ':9000?streamid=' + streamName,
    whip: base + '/webrtc/' + streamName,
  };
}
