/**
 * Base component class for camera modules.
 * Provides lifecycle (render/build), data loading, and camera lookup helpers.
 * Constructor/prototype pattern for Closure Compiler compatibility.
 */

import { el } from '../core/dom_helpers.js';
import { apiClient } from '../core/api_client.js';
import { navto } from '../core/navigation.js';

export var DEVICES_SOURCE_PRIORITY = [
  {type: ['whep', 'ws/video/mp4', 'html5/video/mp4', 'html5/video/x-matroska', 'html5/application/vnd.apple.mpegurl']}
];

export function Component() {}

/** @param {HTMLElement} container  @param {Object=} params */
Component.prototype.render = function(container, params) {
  this.$el = container;
  this.params = params || {};
  this.build();
};

/** Override in subclasses to build the UI. */
Component.prototype.build = function() {};

/**
 * If camera_list is not yet loaded, fetch it and re-navigate.
 * @param {string} tab   Tab name to re-navigate to
 * @param {string=} other  Optional parameter (camera ID)
 * @return {boolean} true if data is ready, false if a fetch was triggered
 */
Component.prototype.requireCameras = function(tab, other) {
  if ('camera_list' in mist.data && 'streams' in mist.data) return true;
  this.$el.appendChild(el('em', 'Loading camera data…'));
  apiClient.send(function() {
    navto(tab, other);
  }, {camera_list: true, streams: true});
  return false;
};

/**
 * Look up a camera by ID in the loaded camera_list.
 * @param {string} id
 * @return {?Object}
 */
Component.prototype.findCamera = function(id) {
  const list = mist.data.camera_list || [];
  for (let i = 0; i < list.length; i++) {
    if (list[i].id === id) return list[i];
  }
  return null;
};

/**
 * Returns MistServer stream entries matching this camera's stream URIs.
 * @param {Object} cam  Camera object from camera_list
 * @return {Array<{name:string, uri:string, protocol:string, label:string}>}
 */
Component.prototype.getStreamNames = function(cam) {
  var results = [];
  if (!cam || !cam.streams || !mist.data.streams) return results;
  var sourceToStream = {};
  for (var sn in mist.data.streams) {
    if (mist.data.streams[sn].source) sourceToStream[mist.data.streams[sn].source] = sn;
  }
  for (var i = 0; i < cam.streams.length; i++) {
    var st = cam.streams[i];
    if (st.uri && sourceToStream[st.uri]) {
      results.push({
        name: sourceToStream[st.uri],
        uri: st.uri,
        protocol: (st.protocol || '').toLowerCase(),
        label: (st.protocol || '').toUpperCase() +
          (st.resolution ? ' ' + st.resolution : st.width && st.height ? ' ' + st.width + 'x' + st.height : '')
      });
    }
  }
  if (!results.length && cam.id) {
    var sanitized = cam.id.replace(/[^a-z0-9_\-\.]/gi, '_').toLowerCase();
    var conventionName = 'cam_' + sanitized;
    if (mist.data.streams[conventionName]) {
      results.push({
        name: conventionName,
        uri: mist.data.streams[conventionName].source || '',
        protocol: '',
        label: 'Auto'
      });
    }
  }
  return results;
};

/**
 * Add an audio transcode process to a stream.
 * @param {string} streamName  MistServer stream name
 * @param {string} targetCodec Target audio codec (e.g. 'opus', 'AAC')
 * @param {function(boolean, string=)} callback  Called with (success, errorMsg)
 */
Component.prototype.addMjpegProcess = function(streamName, callback) {
  var config = mist.data.streams[streamName];
  if (!config) { callback(false, 'Stream not found'); return; }
  if (!config.processes) config.processes = {};
  var caps = (mist.data.capabilities && mist.data.capabilities.processes) || {};
  var proc = caps.AV ? 'AV' : (caps.FFMPEG ? 'FFMPEG' : null);
  if (!proc) { callback(false, 'No transcoder available'); return; }
  config.processes[proc + '_mjpeg'] = {
    process: proc, 'x-LSP-kind': 'video', codec: 'JPEG', quality: 15, gopsize: 30
  };
  var req = {addstream: {}};
  req.addstream[streamName] = config;
  apiClient.send(function() { callback(true); }, req);
};

Component.prototype.addAudioTranscode = function(streamName, targetCodec, callback) {
  var config = mist.data.streams[streamName];
  if (!config) { callback(false, 'Stream not found'); return; }
  if (!config.processes) config.processes = {};
  var caps = (mist.data.capabilities && mist.data.capabilities.processes) || {};
  var proc = caps.AV ? 'AV' : (caps.FFMPEG ? 'FFMPEG' : null);
  if (!proc) { callback(false, 'No transcoder available'); return; }
  var procName = proc + '_audio_' + targetCodec.toLowerCase();
  config.processes[procName] = {
    process: proc, 'x-LSP-kind': 'audio', codec: targetCodec, bitrate: 128000
  };
  var req = {addstream: {}};
  req.addstream[streamName] = config;
  apiClient.send(function() { callback(true); }, req);
};
