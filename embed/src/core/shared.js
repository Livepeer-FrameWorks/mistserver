import { MistUtil } from './util.js';

export function ControlChannel(channel,MistVideo,externalListenersObj){
  /*
    Takes a WebSocket or RTCDataChannel and adds:
    - send: function(msg)
      if the channel is not yet connected, queues the messages
    - addListener: function(type,callback)
      if callback is omitted, returns a promise (for listening for a single message)
      listeners can also be added to the channel state: "channel_open", "channel_close", "channel_error", "channel_timeout"

    externalListeners (optional): object to store listeners that does not get destroyed if the control channel is re-initialized
  */

  var control = this;
  this.channel = channel;
  this.debugging = false;
  this.was_connected = false;
  var queue = [];
  var listeners = externalListenersObj || {};

  //the api can add listeners for types of messages here
  this.addListener = function(type,callback){
    if (!(type in listeners)) {
      listeners[type] = {};
    }
    var eid = Object.keys(listeners[type]).length;

    if (callback) {
      listeners[type][eid] = callback;
      return eid;
    }
    else {
      return new Promise(function(resolve){
        listeners[type][eid] = function(data){
          delete listeners[type][eid];
          resolve(data);
        };
      });
    }
  };
  function callListeners(type,data) {
    if (type in listeners) {
      for (var eid in listeners[type]) {
        try {
          listeners[type][eid].apply(control,[data]);
        }
        catch(err) {
          MistVideo.log("Error in "+type+" listener "+eid+": "+err,"error");
        }
      }
    }
  }

  this.channel.addEventListener("open",function(ev){
    control.was_connected = true;
    callListeners("channel_open",ev);
    if (queue.length) {
      for (var i = 0; i <= queue.length; i++) {
        control.send(queue[i]);
      }
      queue = [];
    }
    if (control.timeout) {
      MistVideo.timers.stop(control.timeout);
    }
  });
  this.timeout = MistVideo.timers.start(function(){
    if (control.readyState == "connecting") {
      MistVideo.log("Control socket timeout","error");
      if (control.debugging) console.log("The control channel timed out");
      callListeners("channel_timeout",control.channel);
    }
  },5e3);

  this.channel.addEventListener("message",function(e){
    var message;
    try {
      message = JSON.parse(e.data);
    } catch(err) {
      MistVideo.log("Received invalid control message: "+err+" in "+e.data,"error");
    }

    if (message) {
      var data = "data" in message ? message.data : message; //some control messages do not have the data key, e.g. websocket webrtc's on_answer_sdp
      if (!message.type) {
        MistVideo.log("Received invalid control message: missing type in "+e.data,"error");
      }
      if (control.debugging) console.log("Received:",message.type,data);
      callListeners(message.type,data);
    }
  });

  this.channel.addEventListener("close",function(ev){
    callListeners("channel_close",ev);
    if (control.debugging) console.log("The control channel was closed",ev);
    MistVideo.log("The control channel was closed");
    //callListeners("on_stop",ev);
  });
  this.channel.addEventListener("error",function(ev){
    callListeners("channel_error",ev);
    if (control.debugging) console.log("The control channel threw an error",ev);
    MistVideo.log("The control channel threw an error: "+ev);
  });

  Object.defineProperty(this,"readyState",{
    get: function(){
      var state = this.channel.readyState;
      if (typeof state == "string") return state;
      switch (state) {
        case 0: return "connecting";
        case 1: return "open";
        case 2: return "closing";
        case 3: return "closed";
      }
      return state; //unknown readyState
    }
  });

  this.send = function(cmdObj){
    if (!this.channel || this.readyState != "open") {
      //wait for it to open
      queue.push(cmdObj);
      if (this.debugging) console.warn("Want to send but control channel is "+this.readyState+". Queue: "+queue.length);
      return;
    }
    var str;
    try {
      str = JSON.stringify(cmdObj);
    } catch(e) {
      MistVideo.log("Tried to send invalid command: "+e,"error");
    }
    if (str) {
      this.channel.send(str);
      if (this.debugging) console.warn("Sent:",cmdObj.type,cmdObj);
    }
  };

  this.addListener("on_error",function(msg){
    callListeners("on_stop",msg);
    MistVideo.showError(msg.message);
  });
  
}

export function DataChannel2WebSocket(){
  /*

    Takes a WebRTC data channel and pretends it is a metadata websocket.
    Note: it is possible MistServer is compiled without datachannel support. If it is available, this is exposed in MistVideo.info.capa.datachannels

    Usage:
     api.metaTrackSocket = new new DataChannel2WebSocket()
   
    Once the datachannel is created, call
     api.metaTrackSocket.init(datachannel);

  */

  this.origin = {};
  this.CONNECTING = 0;
  this.OPEN = 1;
  this.CLOSING = 2;
  this.CLOSED = 3;

  this.debugging = false; //when true, logs metadata received to dev console

  this.readyState = 0;
  //follow readystate of origin, except when the converter is asked to close, then *pretend* to close and remove event listeners.

  this.listeners = [];
  var converter = this;

  this.init = function(datachannel){
    this.origin = datachannel || {};
    if (this.origin._processed) { return true; } //already set up

    if ("readyState" in this.origin) {
      this.origin._processed = true;

      function onopen() {
        converter.readyState = converter.OPEN;
        converter.onopen();
      }

      //for some reason, onopen gets called twice if added with an eventlistener
      this.origin.addEventListener("open",function(){
        onopen();
      });
      //this.origin.onopen = onopen; 

      this.origin.onmessage = function(e){
        if (converter.debugging) console.log("Received metadata:",JSON.parse(e.data));
      };
      this.origin.addEventListener("close",function(){
        converter.readyState = converter.CLOSED;
        converter.onclose();
      });
      if (this.origin.readyState == "open") { onopen(); }

      return true;
    }
    else {
      return false;
    }
  };

  this.open = function(){
    //should be open once webrtc is active

    if (this.readyState == this.OPEN) return; //already open

    switch (this.origin.readyState) {
      case "connecting":  { this.readyState = this.CONNECTING; break; }
      case "open":        { this.readyState = this.OPEN; break; }
      case "closing":     { this.readyState = this.CLOSING; break; }
      case "closed":      { this.readyState = this.CLOSED; break; }
    }

    for (var i in this.listeners) {
      this.origin.addEventListener.apply(this.origin,this.listeners[i]);
    }
  };
  this.close = function(){
    //don't actually close, but pretend
    if (this.readyState >= this.CLOSING) return; //already closed

    this.readyState = this.CLOSED;

    //remove listeners
    for (var i in this.listeners) {
      this.removeEventListener.apply(this,this.listeners[i]);
    }
  };
  this.send = function(){
    return false; //MistServer ignores control messages that are sent over the metadata channel
    //if (converter.debugging) { console.warn("Sent to metadata:",arguments[0]); }
    //if (this.origin.readyState == "open") return this.origin.send.apply(this.origin,arguments);
  };
  this.onopen = function(){};
  this.onclose = function(){};
  this.addEventListener = function(){
    this.listeners.push(arguments);
    return this.origin.addEventListener.apply(this.origin,arguments);
  };
  this.removeEventListener = function(name,func){
    //remove them from the listeners array and the origin
    for (var i = this.listeners.length-1; i >= 0; i--) {
      if ((name == this.listeners[i][0]) && (func == this.listeners[i][1])) {
        this.listeners.splice(i,1);
        break;
      }
    }
    return this.origin.removeEventListener.apply(this.origin,arguments);
  };

  this.init();

  return this;

}

export function ControlChannelAPI(controller,MistVideo,video){

  /*

   Takes a (WebRTC) controller object and video DOMelement and exposes MistPlayer api functions
   The controller object must have:
   {
     connection: {
       connectionState: "connected|failed|closed"
     } for example instanceof RTCPeerConnection, (required)
     connecting: false or instanceof Promise (required), set by .connect()
     control: instanceof ControlChannel, (required)
          should also attempt to reconnect the channel if closed etc
     connect: function(){}, (required)
          initializes the data and control connection, returns a promise
     meta: instanceof RTCDataChannel (optional)
   }

   Usage:
     api = new ControlChannelAPI();

  */


  var api = this;
  var control = controller.control;

  video.setAttribute("playsinline",""); //iphones. effin' iphones.

  //apply options
  var attrs = ["autoplay","loop","poster"];
  for (var i in attrs) {
    var attr = attrs[i];
    if (MistVideo.options[attr]) {
      video.setAttribute(attr,(MistVideo.options[attr] === true ? "" : MistVideo.options[attr]));
    }
  }
  if (MistVideo.options.muted) {
    video.muted = true; //don't use attribute because of Chrome bug
  }
  if (MistVideo.info.type == "live") {
    video.loop = false;
  }
  if (MistVideo.options.controls == "stock") {
    video.setAttribute("controls","");
  }
  video.setAttribute("crossorigin","anonymous");

  //redirect properties
  ["volume"
    ,"muted"
    ,"loop"
    ,"paused"
    ,"error"
    ,"textTracks"
    ,"webkitDroppedFrameCount"
    ,"webkitDecodedFrameCount"
  ].forEach(function(item){
    Object.defineProperty(api,item,{
      get: function(){ return video[item]; },
      set: function(value){
        return video[item] = value;
      }
    });
  });

  //redirect methods
  ["load","getVideoPlaybackQuality"].forEach(function(item){
    if (item in video) {
      api[item] = function(){
        return video[item].call(video,arguments);
      };
    }
  });

  //other properties and methods that cannot be copied one on one
  this.play = function(){
    if (controller.connection){
      switch (controller.connection.connectionState) {
        case "connected": {
          controller.control.send({type:"play"});
          return video.play();
        }
        case "failed":
        case "closed": {
          return new Promise(function(resolve,reject){
            controller.connect().then(function(){
              video.play().then(resolve).catch(reject);
            }).catch(reject);
          })
        }
        default: {
          //we're still connecting - wait
          //=> bubble down to controller.connection == false
        }
      }

    }

    //we're not connected
    return new Promise(function(resolve,reject){
      if (!controller.connecting) {
        MistVideo.log("Received call to play while not connected, connecting..");
        controller.connect();
      }
      else {
        MistVideo.log("Received call to play while still connecting, waiting..");
      }

      if (controller.connecting) {
        controller.connecting.then(resolve).catch(reject);
      }
      else {
        //the connecting Promise should exist after the call to connect, this code should not be triggered
        reject();
      }
    });

  }

  this.pause = function(){
    return new Promise(function(resolve,reject){
      try {
        video.pause();
        controller.control.send({type: "hold"});
        resolve();
      }
      catch(err) {
        reject(err);
      }
    });
  }
  this.stop = function(){
    return new Promise(function(resolve,reject){
      try {
        video.pause();
        controller.control.send({type: "stop"});
        resolve();
      }
      catch(err) {
        reject(err);
      }
    });
  }

  var seekoffset = 0, last_on_time, duration, play_rate, currenttracks = [], looping = false;

  //set generic on_time handler
  controller.control.addListener("on_time",function(msg){
    last_on_time = msg;
    last_on_time._received = new Date();

    //save currentTime offset
    seekoffset = msg.current*1e-3 - video.currentTime;

    //save duration
    var d = (msg.end == 0 ? Infinity : msg.end*1e-3);
    if (d != duration) {
      duration = d;
      MistUtil.event.send("durationchange",d,video);
    }

    //save buffer
    if (MistVideo.info && MistVideo.info.meta) MistVideo.info.meta.buffer_window = msg.end - msg.begin;

    //save playback speed
    play_rate = msg.play_rate_curr;

    //save which tracks are playing and monitor changes
    if ((msg.tracks) && (currenttracks.join(",") != msg.tracks.join(","))) {
      var tracks = MistVideo.info ? MistUtil.tracks.parse(MistVideo.info.meta.tracks) : [];
      for (var i in msg.tracks) {
        if (currenttracks.indexOf(msg.tracks[i]) < 0) {
          //find track type
          var type;
          for (var j in tracks) {
            if (msg.tracks[i] in tracks[j]) {
              type = j;
              break;
            }
          }
          if (!type) {
            //track type not found, this should not happen
            continue;
          }
          if (type == "subtitle") { continue; }

          //create an event to pass this to the skin
          MistUtil.event.send("playerUpdate_trackChanged",{
            type: type,
            trackid: msg.tracks[i]
          },video);
        }
      }

      currenttracks = msg.tracks;
    }

    if (MistVideo.reporting && msg.tracks) {
      MistVideo.reporting.stats.d.tracks = msg.tracks.join(",");
    }
  });

  controller.control.addListener("on_stop",function(msg){
    //if (MistVideo.info.type != "live") 
    MistUtil.event.send("ended",null,video);
    if (api.loop) {
      if (!looping) {
        looping = true;
        seekoffset = 0;
        MistVideo.log("Looping..");
        //console.warn(controller.connection.connectionState);
        controller.close().then(function(){
          //console.warn("closed, connecting");
          controller.connect().then(function(){
            looping = false;
            //console.warn("Looping complete");
          });
        });
      }
    }
    else {
      video.pause();
    }
  });

  //override seeking
  Object.defineProperty(api,"currentTime",{
    get: function(){
      return seekoffset + video.currentTime;
    },
    set: function(value){
      MistUtil.event.send("seeking",value,video);

      seekoffset = (value == "live" ? Infinity : value) - video.currentTime; //immediately place playback cursor at seek point

      controller.control.send({
        type: "seek",
        "seek_time": (value == "live" ? "live" : value*1e3)
      });
      controller.control.addListener("seek").then(function(msg){
        //the message "seek" was received
        return controller.control.addListener("on_time");
      }).then(function(msg){
        //the next "on_time" message was received

        //seekoffset is set in the generic on_time handler
        //seekoffset = msg.current*1e-3 - video.currentTime;

        MistUtil.event.send("seeked",seekoffset,video);

        return video.play();
      }).catch(function(){
        //do nothing
      });
    }
  });

  //duration
  Object.defineProperty(api,"duration",{
    get: function(){
      if (MistVideo.info.type == "live") {
        return duration + (last_on_time ? new Date().getTime() - last_on_time._received.getTime() : 0)*1e-3;
      }
      return duration; 
    }
  });


  //playbackrate
  controller.control.addListener("set_speed",function(msg){
    play_rate = msg.play_rate_curr;
  });
  Object.defineProperty(api,"playbackRate",{
    get: function(){
      if (play_rate) {
        switch (play_rate) {
          case "auto":
          case "fast-forward": {
            //MistServer is controlling the playback rate
            //fast forwards means it is speeding now but it will return to auto once the requested position is reached (after seeking)
            return 1;
          }
          default: {
            return play_rate;
          }
        }
      }
      return 1;
    },
    set: function(value){
      control.send({
        type: "set_speed",
        play_rate: value == 1 ? "auto" : value
      });
    }
  });

  //setTracks
  this.setTracks = function(obj){
    obj.type = "tracks";
    control.send(obj);
  };

  //getStats
  if (window.RTCPeerConnection && (controller.connection instanceof RTCPeerConnection)) {
    this.getStats = function(){
      if (controller && controller.connection && controller.connection.connectionState == "connected") {
        return controller.connection.getStats().then(function(a){
          var r = {
            audio: null,
            video: null
          };
          var obj = Object.fromEntries(a);
          for (var i in obj) {
            var s = obj[i];
            switch (s.type) {
              case "inbound-rtp": {
                r[s.kind] = s;
                break;
              }
              case "data-channel": {
                var label;
                switch (s.label) {
                  case "MistControl": { label = "Control Channel";  break; }
                  case "*":           { label = "Metadata Channel"; break; }
                  default:            { label = s.label; }
                }
                r[label] = s;
                break;
              }
            }
          }
          return r;
        });
      }
    };

    if ("decodingIssues" in MistVideo.skin.blueprints) {
      //get additional dev stats
      var vars = ["nackCount","pliCount","packetsLost","packetsReceived","bytesReceived","messagesReceived","messagesSent"];
      for (var j in vars) {
        api[vars[j]] = {};
      }
      api.jitterDelay = {};
      var last_stats;
      var f = function() {
        MistVideo.timers.start(function(){
          var stats = api.getStats();
          if (stats) {
            stats.then(function(d){
              for (var i in vars) {
                var v = vars[i];
                var out = {}
                for (var channel in d) {
                  if (d[channel] && (v in d[channel])) out[channel] = d[channel][v];
                }
                if (Object.keys(out).length) {
                  api[v] = out;
                }
              }

              api.jitterDelay = {};
              if (last_stats) {
                for (var channel in d) {
                  if ((channel in last_stats) && last_stats[channel] && d[channel] && ("jitterBufferDelay" in d[channel])) {
                    api.jitterDelay[channel] = (d[channel].jitterBufferDelay - last_stats[channel].jitterBufferDelay) / (d[channel].jitterBufferEmittedCount - last_stats[channel].jitterBufferEmittedCount); 
                    //average jitterDelay [s] between the last time stats were checked
                  }
                }
              }

              last_stats = d;
            });
          }
          f();
        },1e3);
      };
      f();

      this.getLatency = function() {
        return api.jitterDelay;
      }
    }

  }

  //metaTrackSocket: use datachannel instead of (another) websocket
  if (controller.meta && MistVideo.info && MistVideo.info.capa && MistVideo.info.capa.datachannels) {
    this.metaTrackSocket = function() {
      var converter = DataChannel2WebSocket();
      if (controller.meta) converter.init(controller.meta);
      else if (controller.connecting) {
        controller.connecting.then(function(){
          converter.init(controller.meta);
        });
      }
      else {
        MistVideo.log("Failed to attach to MetaTrack datachannel","error");
      }

      //live passthrough of the debugging flag
      Object.defineProperty(converter,"debugging",{
        get: function(){
          return MistVideo.player.debugging; 
        }
      });

      return converter;
    }
  }

  // ABR_resize
  this.ABR_resize = function(size){
    MistVideo.log("Requesting the video track with the resolution that best matches the player size");
    this.setTracks({video:"~"+[size.width,size.height].join("x")});
  };
  // unload
  this.unload = function(){
    controller.control.send({type: "stop"});
    controller.connection.close();
  };



}
