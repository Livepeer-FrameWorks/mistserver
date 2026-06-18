/**
 * Stream creation modal - adds a camera stream as a MistServer stream.
 * Server-side constructs the full RTSP URI with embedded credentials.
 */
import { APP_NAME } from '@brand';
import { el } from '../core/dom_helpers.js';
import { api } from './api.js';
import { uiHelpers } from '../core/ui_helpers.js';
import { navto } from '../core/navigation.js';

export const StreamDialog = {};

/**
 * Show the stream creation modal.
 * @param {string} camId       Camera ID
 * @param {string} camName     Camera display name
 * @param {Object} stream      Stream object from camera data
 * @param {number} streamIdx   Index in the camera's streams array
 */
StreamDialog.show = function(camId, camName, stream, streamIdx) {
  const suggestedName = (camName || 'cam').replace(/[^a-zA-Z0-9_-]/g, '_')
    + '_' + (stream.profile || stream.name || streamIdx);
  const streamData = {name: suggestedName};
  const meta = [];
  if (stream.profile || stream.name) meta.push(stream.profile || stream.name);
  if (stream.resolution || (stream.width && stream.height)) {
    meta.push(stream.resolution || (stream.width + 'x' + stream.height));
  }
  if (stream.framerate || stream.fps) meta.push((stream.framerate || stream.fps) + ' fps');
  if (stream.codec || stream.encoding) meta.push(stream.codec || stream.encoding);
  if (stream.protocol) meta.push(stream.protocol.toUpperCase());
  const formFields = [];
  if (meta.length) {
    formFields.push({
      type: 'text',
      text: el('p', {style: 'color: var(--secondaryTextColor)'}, meta.join(' - '))
    });
  }

  if (stream.uri) {
    formFields.push({
      type: 'custom',
      build: function() {
        return el('div', {class: 'cam-section'},
          el('code', {style: 'word-break: break-all; font-size: 0.9em'}, stream.uri)
        );
      }
    });
  }

  formFields.push({label: 'Stream Name', type: 'str', pointer: {main: streamData, index: 'name'}, validate: ['required']});
  const modal = uiHelpers.openFormModal({
    size: 'md',
    title: 'Add Stream to '+APP_NAME,
    form: formFields,
    buttons: [
      {label: 'Create Stream', type: 'save', 'function': function() {
        api.createStream({
          id: camId,
          stream_index: streamIdx,
          stream_name: streamData.name
        }).then(function() {
          modal.close();
          navto('Camera Detail', camId);
        })['catch'](function(err) {
          alert('Failed to create stream: ' + err.message);
        });
      }},
      {label: 'Cancel', type: 'cancel'}
    ]
  });
};
