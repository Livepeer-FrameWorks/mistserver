import { uiHelpers } from '../core/ui_helpers.js';
import { apiClient } from '../core/api_client.js';
import { navto } from '../core/navigation.js';
import { mistHelpers } from '../core/mist_helpers.js';
import { streamHints } from './stream_hints.js';
import { setval } from '../core/dom_helpers.js';

/**
 * Save a stream configuration to the server.
 * @param {Object} saveas     Stream config object (mutated in place)
 * @param {string} other      Original stream identifier (for rename detection)
 * @param {string} targetTab  Where to navigate after save ('Streams' or 'Preview')
 */
export function streamSave(saveas, other, targetTab, afterSave) {
  if (!saveas || (typeof saveas != "object")) {
    console.warn("streamSave called without a valid save object.");
    return;
  }
  if (!saveas.name || (typeof saveas.name != "string")) {
    console.warn("streamSave called without a valid stream name.",saveas);
    return;
  }

  const originalStream = (typeof other == "string") ? other : "";
  const originalBase = originalStream ? originalStream.split("+")[0] : saveas.name.split("+")[0];

  let send = {};

  if (!mist.data.streams) mist.data.streams = {};

  mist.data.streams[saveas.name] = saveas;
  if (typeof saveas.source != "string") saveas.source = "";
  if (!Array.isArray(saveas.streamkeys)) saveas.streamkeys = [];
  if (typeof saveas.streamkey_only != "boolean") saveas.streamkey_only = false;

  send.addstream = {};
  send.addstream[saveas.name] = saveas;
  if (originalStream && (originalStream != saveas.name)) {
    delete mist.data.streams[originalStream];
    send.deletestream = [originalStream];
  }
  if ((saveas.stop_sessions) && originalStream) {
    send.stop_sessions = originalStream;
    delete saveas.stop_sessions;
  }

  if (saveas.source.slice(0, 7) == "push://") {
    send.streamkey_del = [];
    send.streamkey_add = {};
    const old = streamHints.findStreamKeys(originalBase);
    for (let k = 0; k < old.length; k++) {
      if (saveas.streamkeys.indexOf(old[k]) < 0) {
        send.streamkey_del.push(old[k]);
      }
    }
    for (let k = 0; k < saveas.streamkeys.length; k++) {
      const key = saveas.streamkeys[k];
      if (!mist.data.streamkeys || !(key in mist.data.streamkeys) || (mist.data.streamkeys[key] != saveas.name)) {
        send.streamkey_add[key] = saveas.name;
      }
    }
    if (saveas.streamkey_only) {
      saveas.source = saveas.source.replace(/push:\/\/[^:@\/]*/, "push://invalid,host");
    } else {
      saveas.source = saveas.source.replace("push://invalid,host", "push://");
    }
  }

  let type = null;
  for (const i in mist.data.capabilities.inputs) {
    if (typeof mist.data.capabilities.inputs[i].source_match == 'undefined') continue;
    if (mistHelpers.inputMatch(mist.data.capabilities.inputs[i].source_match, saveas.source)) {
      type = i;
      break;
    }
  }
  if (type) {
    const input = mist.data.capabilities.inputs[type];
    for (const i in saveas) {
      if (["name", "source", "stop_sessions", "processes", "tags"].indexOf(i) >= 0) continue;
      if (("optional" in input) && (i in input.optional)) continue;
      if (("required" in input) && (i in input.required)) continue;
      if ((i == "always_on") && ("always_match" in input) && (mistHelpers.inputMatch(input.always_match, saveas.source))) continue;
      delete saveas[i];
    }
  }

  apiClient.send(function() {
    delete mist.data.streams[saveas.name].online;
    delete mist.data.streams[saveas.name].error;
    if (afterSave) {
      afterSave(targetTab, saveas, other);
    }
    else {
      navto(targetTab, (targetTab == 'Preview' ? (originalStream.indexOf("+") < 0 ? saveas.name : originalStream) : ''));
    }
  }, send);
};

export const streamPresetHelpers = {
  presetDescriptions: {
    name_only: "Use only the stream name for incoming pushes. No stream keys are accepted.",
    name_or_key: "Allow pushes using either stream name or stream key. Recommended balanced default.",
    key_only: "Require a stream key for every incoming push. Most restrictive option."
  },

  randomKey: function(length) {
    return uiHelpers.randomKey(length);
  },

  detect: function(saveas) {
    if (saveas.streamkey_only) return "key_only";
    if (saveas.streamkeys && saveas.streamkeys.length) return "name_or_key";
    return "name_only";
  },

  apply: function(saveas, preset, $main) {
    switch (preset) {
      case "key_only":
        saveas.streamkey_only = true;
        if (!saveas.streamkeys || !saveas.streamkeys.length) {
          saveas.streamkeys = [this.randomKey(32)];
        }
        break;
      case "name_or_key":
        saveas.streamkey_only = false;
        if (!saveas.streamkeys || !saveas.streamkeys.length) {
          saveas.streamkeys = [this.randomKey(32)];
        }
        break;
      case "name_only":
      default:
        saveas.streamkey_only = false;
        saveas.streamkeys = [];
        break;
    }
    if ($main) {
      const skOnlyEl = $main.querySelector('[name=streamkey_only]');
      if (skOnlyEl) setval(skOnlyEl, saveas.streamkey_only, ["preset_sync"]);
      const skEl = $main.querySelector('[name=streamkeys]');
      if (skEl) setval(skEl, saveas.streamkeys, ["preset_sync"]);
    }
  }
};
