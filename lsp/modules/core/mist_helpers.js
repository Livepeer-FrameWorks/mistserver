import { apiClient } from './api_client.js';
import { deepExtend } from './dom_helpers.js';

export const mistHelpers = {
  inputMatch: function(match,string){
    if (typeof match == 'undefined') { return false; }
    if (typeof match == 'string') {
      match = [match];
    }
    for (const s in match){
      let m = match[s];
      let str = string;
      if (m.slice(-1) == "?") {
        m = m.slice(0,-1);
        str = str.split("?")[0];
      }
      let query = m.replace(/[^\w\s]/g,'\\$&');
      query = query.replace(/\\\*/g,'.*');
      const regex = new RegExp('^(?:[a-zA-Z]\:)?'+query+'(?:\\?[^\\?]*)?$','i');
      if (regex.test(str)){
        return true;
      }
    }
    return false;
  },
  convertPushArr2Obj: function(push,isAutoPush) {
    const out = {
      id: push[0],
      stream: push[1],
      target: push[2]
    };
    if (isAutoPush) {
      if (push.length >= 4) {
        if (push[3]) out.scheduletime = push[3];
        if (push.length >= 5) {
          if (push[4]) out.completetime = push[4];
          if (push.length >= 8) {
            if (push[5]) {
              out.start_rule([push[5],push[6],push[7]]);
            }
            if (push.length >= 11) {
              if (push[8]) {
                out.end_rule([push[8],push[9],push[10]]);
              }
            }
          }
        }
      }
    }
    else {
      if (push.length >= 4) {
        if (push[3]) out.resolved_target = push[3];
        if (push.length >= 5) {
          if (push[4]) out.logs = push[4];
          if (push.length >= 6) {
            if (push[5]) out.stats = push[5];
          }
        }
      }
    }
    return out;
  },
  stored: {
    get: function(key){
      if (key) {
        if (mist.data.ui_settings && (key in mist.data.ui_settings)) {
          return deepExtend({},mist.data.ui_settings[key]);
        }
        return {};
      }
      return mist.data.ui_settings || {};
    },
    set: function(name,val){
      const settings = this.get();
      const oldval = JSON.stringify(settings[name]);
      settings[name] = val;
      if (oldval != JSON.stringify(val)) {
        apiClient.send(function(){
        },{ui_settings: settings});
      }
    },
    del: function(name){
      delete mist.data.ui_settings[name];
      apiClient.send(function(){
      },{ui_settings: mist.data.ui_settings});
    }
  }
};
