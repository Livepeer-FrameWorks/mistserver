/**
 * Promise-based API wrappers around apiClient.send().
 * Methods reject if the server returns an error in authorize.status.
 */

import { apiClient } from '../core/api_client.js';

export const api = {};

/** Generic send - wraps mist.send in a Promise. Rejects on error responses. */
api.send = function(params) {
  return new Promise(function(resolve, reject) {
    apiClient.send(function(d) {
      if (d && d.authorize && d.authorize.status === 'CHALL') {
        reject(new Error('Authentication required'));
      } else {
        resolve(d);
      }
    }, params);
  });
};

/** Fetch the camera list. */
api.getCameraList = function() {
  return api.send({camera_list: true});
};

/** Add or update a camera. */
api.updateCamera = function(params) {
  return api.send({camera_update: params});
};

/** Remove a camera by ID. */
api.removeCamera = function(id) {
  return api.send({camera_remove: {id: id}});
};

/** Send a PTZ command to a camera. */
api.sendPTZ = function(id, command, args) {
  return api.send({camera_query: {id: id, command: command, args: args || {}}});
};

/** Create a MistServer stream from a camera stream. */
api.createStream = function(params) {
  return api.send({camera_create_stream: params});
};
