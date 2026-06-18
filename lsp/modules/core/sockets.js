import { stream } from '../streams/stream_utils.js';
import { apiClient } from './api_client.js';
import { parseURL } from './app_state.js';
import { MD5 } from '../../plugins/md5.js';

export const sockets = {
  http_host: null,
  http: {
    api: {
      command: {},
      listeners: [],
      interval: false,
      inFlight: false,
      queued: false,
      retryTimer: false,
      failures: 0,
      clear: function(){
        if (this.retryTimer) {
          clearTimeout(this.retryTimer);
          this.retryTimer = false;
        }
        this.command = {};
        this.listeners = [];
        this.interval = false;
        this.inFlight = false;
        this.queued = false;
        this.failures = 0;
      },
      get: function(){
        const me = this;
        if (me.inFlight) {
          me.queued = true;
          return;
        }
        if (me.retryTimer) {
          return;
        }
        me.inFlight = true;
        apiClient.send(function(d){
          me.inFlight = false;
          me.failures = 0;
          for (const i in me.listeners) {
            me.listeners[i](d);
          }
          if (me.queued) {
            me.queued = false;
            me.get();
          }
        },me.command,{
          hide: true,
          noLoginOnError: true,
          onError: function(){
            me.inFlight = false;
            me.failures++;
            if (!me.interval || !(me.interval in UI.interval.list) || me.retryTimer) {
              return;
            }
            const delay = Math.min(3e4, Math.pow(2, Math.min(me.failures, 5)) * 250);
            me.retryTimer = setTimeout(function(){
              me.retryTimer = false;
              me.get();
            }, delay);
          }
        });
      },
      init: function(){
        const me = this;
        me.get();
        me.interval = UI.interval.set(function(){
          me.get();
        },5e3);
      },
      subscribe: function(callback,command){
        if (!this.interval || !(this.interval in UI.interval.list)) {
          this.clear();
        }
        this.command = Object.assign(this.command,command);
        if (!this.interval || !(this.interval in UI.interval.list)) {
          this.listeners.push(callback);
          this.init();
        }
        else {
          this.listeners.push(callback);
          this.get();
        }
      }
    },
    clients: {
      requests: [],
      listeners: [],
      interval: false,
      clear: function(){
        this.requests = [];
        this.listeners = [];
        this.interval = false;
      },
      subscribe: function(callback,request){
        if (!this.interval || !(this.interval in UI.interval.list)) {
          this.clear();
        }

        if (!("time" in request)) { request.time = -3; }
        this.requests.push(request);
        this.listeners.push(callback);
        const me = this;

        if (this.listeners.length == 1) {
          sockets.http.api.subscribe(function(d){
            const clientResponses = Array.isArray(d.clients) ? d.clients : [];
            for (let i = 0; i < me.listeners.length; i++) {
              const res = clientResponses[i];
              const out = { data: [], time: null };
              if (res && Array.isArray(res.data) && Array.isArray(res.fields)) {
                out.time = res.time;
                for (const j in res.data) {
                  const entry = {};
                  for (const k in res.fields) {
                    const key = res.fields[k];
                    entry[key] = res.data[j][k];
                  }
                  out.data.push(entry);
                }
              }
              me.listeners[i].call(null,out);
            }
          },{clients:this.requests});
          this.interval = sockets.http.api.interval;
        }
        else {
          sockets.http.api.get();
        }
      }
    },
  },
  ws: {
    info_json: {
      children: {},
      _failures: {},
      init: function(url){
        const me = this;
        if (me._failures[url] >= 3) { return false; }
        const ws = UI.websockets.create(url, function(e){
          me._failures[url] = (me._failures[url] || 0) + 1;
          if (url in me.children) {
            for (const i in me.children[url].listeners) {
              me.children[url].listeners[i](e);
            }
          }
        });
        if (!ws) {
          me._failures[url] = (me._failures[url] || 0) + 1;
          return false;
        }
        this.children[url] = {
          ws: ws,
          messages: [],
          listeners: []
        };
        ws.onmessage = function(d){
          const data = JSON.parse(d.data);
          if (url in me.children) {
            me._failures[url] = 0;
            me.children[url].messages.push(data);
            for (const i in me.children[url].listeners) {
              me.children[url].listeners[i](data);
            }
          }
        };
        ws.cleanup = function(){
          delete me.children[url];
          ws.onclose = function(){};
          ws.onmessage = function(){};
        };
        const close = ws.close;
        ws.close = function(){
          ws.cleanup();
          return close.apply(this,arguments);
        };
        ws.onclose = function(){
          ws.cleanup();
        };
        return true;
      },
      subscribe: function(callback,streamname,url,params){
        if (!callback) { throw "Callback function not specified."; }
        if (!streamname && !url) { throw "Stream name not specified."; }
        if (!params) { params = ""; }
        if (!url) {
          if (!sockets.http_host) {
            const me = this;
            const args = arguments;
            stream.findMist(function(url){
              me.subscribe.apply(me,args);
            });
            return;
          }
          url = sockets.http_host.replace(/^http/,"ws") + "json_" + encodeURIComponent(streamname) + ".js"+params;
        }

        if (!(url in this.children) || (this.children[url].ws.readyState > 1)) {
          if (!this.init(url)) return;
        }
        for (const i in this.children[url].messages) {
          callback(this.children[url].messages[i]);
        }
        this.children[url].listeners.push(callback);
      }
    },
    active_streams: {
      ws: false,
      listeners: [],
      messages: [],
      init: function(error_callback){
        let url = parseURL(mist.user.host);
        url = parseURL(mist.user.host,{pathname:url.pathname.replace(/\/api$/,"")+"/ws",search:"?logs=100&accs=100&streams=1"});
        const apiWs = UI.websockets.create(url.full.replace(/^http/,"ws"),error_callback);
        if (!apiWs) {
          if (error_callback) error_callback({message: "WebSocket creation failed"});
          return;
        }
        this.ws = apiWs;
        const me = this;
        apiWs.authState = 0;
        apiWs.onmessage = function(d){
          const da = JSON.parse(d.data);
          const type = da[0];
          const data = da[1];

          if (type == "auth") {
            if (data === true) { this.authState = 2; }
            else if (data === false) {
              this.send(JSON.stringify(["auth",{
                password: MD5(mist.user.password+mist.user.authstring),
                username: mist.user.name
              }]));
              this.authState = 1;
            }
            else if (typeof data == "object") {
              if ("challenge" in data) {
                this.send(JSON.stringify(["auth",{
                  password: MD5(mist.user.password+data.challenge),
                  username: mist.user.name
                }]));
                this.authState = 1;
              }
              else if (("status" in data) && (data.status == "OK")) {
                this.authState = 2;
              }
            }
            return;
          }
          me.messages.push([type,data]);
          for (const i in me.listeners){
            me.listeners[i](type,data);
          }
        };
        const close = apiWs.close;
        apiWs.close = function(){
          me.listeners = [];
          me.messages = [];
          me.ws = false;
          return close.apply(this,arguments);
        };
      },
      subscribe: function(callback){
        for (const i in this.messages) {
          callback(this.messages[i][0],this.messages[i][1]);
        }
        const me = this;
        if (!this.ws || (this.ws.readyState > 1)) {
          this.init(function(){
            for (const i in me.listeners) {
              me.listeners[i]("error","An error occured: failed to connect to the active streams api websocket.");
            }
          });
        }
        this.listeners.push(callback);
      }
    }
  }
};
