import { MistUtil } from '../../core/util.js';

export const mediaBlueprints = {
    subtitles: function(options){
      if (!("WebSocket" in window)) { return false; }
      var MistVideo = this;
      if (!MistVideo.api || !("currentTime" in MistVideo.api)) { return false; }
      
      if (!("metaTrackSubscriptions" in MistVideo)) { return false; }

      function clearFormatting(str) {
        str = str.replace(/\<\/?[bui]\>/gi,""); //remove <b>,</b>,<u>,</u>,<i>,</i>
        str = str.replace(/{\/?[bui]}/gi,"");   //remove {b},{/b},{u},{/u},{i},{/i}
        str = str.replace(/{\\a\d+}/gi,"");     //remove {\a3} (line position)
        str = str.replace(/\<\/?font[^>]*?\>/gi,""); //remove <font color="white">,</font>
        return str;
      }

      var container = document.createElement("div");

      container.addEventListener("click",function(e){
        if (this.hasAttribute("data-position")) this.removeAttribute("data-position");
        else this.setAttribute("data-position","top");
        e.stopPropagation();
      });

      var c = document.createElement("span");
      container.appendChild(c);
      var textNode = document.createTextNode("");
      c.appendChild(textNode);

      var timer = false;
      function displayMessage(message) {
        //console.warn("displaymessage",message);
        textNode.nodeValue = clearFormatting(message.data);
        if (timer) {
          //a previous message is still being displayed, remove the timer so that it doesn't remove the new message
          MistVideo.timers.stop(timer);
          timer = null;
        }


        function setTimer(delay) {
          timer = MistVideo.timers.start(function(){

            if (MistVideo.api.paused) {
              var playing = MistUtil.event.addListener(MistVideo.video,"playing",function(){
                setTimer(message.time + ("duration" in message ? message.duration : 5e3) - MistVideo.api.currentTime*1e3);
                MistUtil.event.removeListener(playing);
              });

              return;
            }

            textNode.nodeValue = "";
          },delay);
        }
        setTimer("duration" in message ? message.duration : 5e3);

      }

      //when seeking, clear the current subtitle message
      MistUtil.event.addListener(MistVideo.video,"seeked",function(){
        textNode.nodeValue = "";
        if (timer) { MistVideo.timers.stop(timer); }
        timer = null;
      });

      if (!("setWSSubtitle" in MistVideo.api)) {
        var trackid = false;
        MistVideo.player.api.setWSSubtitle = function(id){
          if (id == trackid) { return; } //already selected

          //first add, then remove: this prevents the websocket closing because no tracks are selected

          if (typeof id != "undefined") {
            MistVideo.metaTrackSubscriptions.add(id,displayMessage);
          }

          if (id != trackid) {
            MistVideo.metaTrackSubscriptions.remove(trackid,displayMessage);
          }
          //console.warn("subscribed to ",id);
          trackid = (id == "undefined" ? false : id);
        }
      }
    
      return container;
    },
    chromecast: function(){
      var MistVideo = this;
      
      if ((!"".indexOf) || (MistVideo.options.host.indexOf("localhost") > -1) || (MistVideo.options.host.indexOf("::1") > -1)) { return; } //it's old IE or the Mist host is localhost and so it won't work ^_^

      var ele = document.createElement("div");
      var casting_ele = document.createElement("div");
      casting_ele.className = "mistvideo-casting";

      var api, reload, nextCombo, unload;
      var selectedTracks = {};
      var remoteData = {
        currentTime: false,
        paused: true,
        volume: 1,
        muted: false,
        buffer: [],
        loop: (MistVideo.player.api ? MistVideo.player.api.loop : MistVideo.options.loop)
      };
      MistVideo.casting = false;

      function onCastLoad(tries) {
        if (!window.chrome || !window.chrome.cast || !window.chrome.cast.isAvailable || (tries > 5)) {
          if (ele.parentNode) { ele.parentNode.removeChild(ele); }
          MistVideo.log("Chromecast is not supported");
          return;
        }

        if (!window.cast) {
          if (!tries) { tries = 0; }
          MistVideo.log("Casting api loaded but cast function not yet available, retrying..");
          MistVideo.timers.start(function(){
            onCastLoad(tries++);
          },200)
          return;
        }

        MistVideo.log("Chromecast API loaded");
        if (!window.loadedCastApi || (window.loadedCastApi == "loading")) {
          window.loadedCastApi = true; //so we don't try this multiple times
        }

        var cast_ele = document.createElement("google-cast-launcher");
        cast_ele.setAttribute("role","button");
        cast_ele.setAttribute("tabindex","0");
        cast_ele.setAttribute("aria-label",MistVideo.translate("chromecast"));
        ele.appendChild(cast_ele);
        cast.framework.CastContext.getInstance().setOptions({
          receiverApplicationId: "E5F1558C",
          autoJoinPolicy: chrome.cast.AutoJoinPolicy.ORIGIN_SCOPED
        });

        function detachFromCast(keepSession) {
          MistUtil.class.remove(cast_ele,"active");
          MistUtil.class.remove(MistVideo.container,"casting");
          if (casting_ele.parentNode) { casting_ele.parentNode.removeChild(casting_ele) }
          if (!keepSession && (cast.framework.CastContext.getInstance().getCurrentSession())) {
            cast.framework.CastContext.getInstance().getCurrentSession().endSession(true);
          }

          if (api) { 
            MistVideo.player.api = api;
            api.currentTime = remoteData.currentTime;
            api.play();
            MistVideo.reload = reload;
            MistVideo.nextCombo = nextCombo;
            MistVideo.unload = unload;
          }
          else {
            MistVideo.player.api.play();
          }
          if (MistVideo.player.api && MistVideo.player.api.setTracks && MistUtil.object.keys(selectedTracks).length) {
            MistVideo.player.api.setTracks(selectedTracks);
          }
          MistVideo.casting = false;
          MistVideo.log("Detached chromecast session");
        }
        MistVideo.detachFromCast = detachFromCast;

        cast_ele.addEventListener("click",function(e){
          e.stopPropagation();
          
          //if (false) { 
          if (MistUtil.class.has(cast_ele,"active")) {
            detachFromCast();
          }
          else {

            MistUtil.class.add(cast_ele,"active");
            casting_ele.innerHTML = MistVideo.translate("select cast device");
            MistVideo.container.appendChild(casting_ele);
            MistUtil.class.add(MistVideo.container,"casting");
            MistVideo.log("chromecast: pausing player")
            MistVideo.player.api.pause();
            MistVideo.container.setAttribute("data-loading","waiting for cast");
            MistVideo.casting = true;

            if (!window.MistCast) {
              window.MistCast = {
                send: function(obj){
                  cast.framework.CastContext.getInstance().getCurrentSession().sendMessage("urn:x-cast:mistcaster",JSON.stringify(obj));
                }
              };
            }
            
            function loadStream() {

              cast.framework.CastContext.getInstance().getCurrentSession().addMessageListener("urn:x-cast:mistcaster",function(ns,message){
                if (MistVideo.destroyed) { detachFromCast(); }
                message = JSON.parse(message);
                //console.log("chromecast message received:",message);
                if (message.type) {
                  switch(message.type){
                    case "log":
                    case "error": {
                      MistVideo.log("[Chromecast] "+message.message,message.type);
                      break;
                    }
                    case "showError":{
                      MistVideo.showError.apply(MistVideo,message.args);
                      break;
                    }
                    case "event": {
                      switch (message.event) {
                        case "timeupdate": {
                          remoteData.currentTime = message.currentTime;
                          MistUtil.event.send(message.event,"chromecast",MistVideo.video);
                          break;
                        }
                        case "progress": {
                          remoteData.buffer = message.buffer;
                          MistUtil.event.send(message.event,"chromecast",MistVideo.video);
                          break;
                        }
                        case "pause":
                        case "paused":
                        case "ended": 
                        case "play":
                        case "playing": {
                          remoteData.paused = message.paused;
                          MistUtil.event.send(message.event,"chromecast",MistVideo.video);
                          break;
                        }

                        case "volumechange": {
                          remoteData.volume = message.volume;
                          remoteData.muted = message.muted;
                          MistUtil.event.send(message.event,"chromecast",MistVideo.video);
                          break;
                        }
                        default: {
                          MistUtil.event.send(message.event,"chromecast",MistVideo.video);
                        }
                      }
                      break;
                    }
                    case "detach": {
                      if (message.n == MistVideo.n) {
                        detachFromCast(true); //don't murder the session 
                      }
                      break;
                    }
                    default: {
                      console.log("Unknown chromecast message type",message);
                    }
                  }
                }
              });

              var d = {
                type: "load",
                n: MistVideo.n, //indentify which player instance we are
                options: {
                  host: MistVideo.options.host,
                  loop: MistVideo.options.loop, //will be overwritten with the current value if there is a player api
                  poster: MistVideo.options.poster, //should be an absolute url, because the location will be different
                  streaminfo: MistVideo.options.streaminfo,
                  urlappend: MistVideo.options.urlappend,
                  forcePriority: MistVideo.options.forcePriority,
                  setTracks: MistVideo.options.setTracks, //when the track selection is changed through the UI, the selected track is saved in the options, so this passes on the currently enforced tracks
                  controls: false,
                  skin: "default" //TODO: right now the skin can't really be transferred because there are functions in there that won't be in the JSON. At some point we should fix this, probably by having the Mist backend include a custom skin definition with the player code.
                },
                stream: MistVideo.stream
              };
              if (MistVideo.info && (MistVideo.info.type != "live")) {
                d.time = MistVideo.player.api.currentTime;
              }
              if (MistVideo.options.skin == "dev") {
                d.options.skin = MistVideo.options.skin;
              }
              if (MistVideo.player && MistVideo.player.api) {
                d.volume = MistVideo.player.api.volume;
                d.muted = MistVideo.player.api.muted;
                d.options.loop = MistVideo.player.api.loop;
              }
              
              MistCast.send(d);

              api = MistVideo.player.api;
              reload = MistVideo.reload;
              nextCombo = MistVideo.nextCombo;
              unload = MistVideo.unload;
              selectedTracks = MistVideo.options.setTracks ? MistVideo.options.setTracks : {};

              MistVideo.player.api = new Proxy(api,{
                get: function(target,key,receiver){
                  var property = target[key];
                  switch (key) {
                    case "muted":
                    case "volume":
                    case "currentTime":
                    case "paused":
                    case "loop": {
                      return remoteData[key];
                    }
                    case "buffered": {
                      return new function(){
                        this.length = remoteData.buffer.length;
                        this.start = function(i){
                          return remoteData.buffer[i][0];
                        }
                        this.end = function(i){
                          return remoteData.buffer[i][1];
                        }

                      }
                    }
                    case "setTracks":
                    case "play":
                    case "pause": {
                      return function() {
                        //put the arguments into an array so JSON doesn't turn it into an object
                        var a = [];
                        for (var i = 0; i < arguments.length; i++) {
                          a.push(arguments[i]);
                        }
                        if (key == "setTracks") {
                          //save the selected tracks so that we can restore them when casting detaches
                          MistUtil.object.extend(selectedTracks,arguments[0]);
                        }
                        MistCast.send({type: "cmd", cmd: key, args: a});
                      }
                    } 
                  }
                  return property;
                },
                set: function(target,key,value){
                  if (key == "loop") {
                    remoteData[key] = value;
                  }
                  MistCast.send({type:"set",cmd:key,value:value});
                  return true;
                }
              });
              api.pause();
              MistVideo.reload = function(){
                MistCast.send({type:"reload"});
              };
              MistVideo.nextCombo = function(){
                MistCast.send({type:"nextCombo"});
              };
              MistVideo.unload = function(){
                //this one should actually unload the current player, but detach the chromecast session first
                detachFromCast();
                return unload.apply(this,arguments);
              };

              var d = cast.framework.CastContext.getInstance().getCurrentSession().getCastDevice();
              if (d && d.friendlyName) { casting_ele.innerHTML = MistVideo.translate("casting to")+" "+d.friendlyName; }

              var on_session_end = function(){
                if (cast.framework.CastContext.getInstance().getSessionState() == "SESSION_ENDED") {
                  if (MistUtil.class.has(cast_ele,"active")) {
                    detachFromCast();
                  }
                  cast.framework.CastContext.getInstance().removeEventListener(cast.framework.CastContextEventType.SESSION_STATE_CHANGED,on_session_end);
                }
              };
              cast.framework.CastContext.getInstance().addEventListener(cast.framework.CastContextEventType.SESSION_STATE_CHANGED,on_session_end);

              /*for (let i of ["sessionstatechanged","caststatechanged"]) {
                cast.framework.CastContext.getInstance().addEventListener(i,function(){
console.warn(i,cast.framework.CastContext.getInstance().getSessionState());
                });
              }*/

              MistVideo.log("Attached chromecast session");
            }


            if (cast.framework.CastContext.getInstance().getCurrentSession()) {
              loadStream();
            }
            else {

              cast.framework.CastContext.getInstance().requestSession().then(function(){
                //console.log("Session requested");
                if (!cast.framework.CastContext.getInstance().getCurrentSession()) { throw "Could not connect to the cast device";  }
                loadStream();
              },function(e){
                MistVideo.log("Chromecast session ended: "+e);
                detachFromCast();
              });
            }
          }

        },true); //capture so that chrome's own handler doesn't fire
      }

      if ((!window.chrome || !window.chrome.cast) && (!window.loadedCastApi)) {
        window['__onGCastApiAvailable'] = function(loaded, errorInfo) {
          if (!loaded) {
            MistVideo.log("Error while loading chromecast API: "+errorInfo);
          }
          onCastLoad();
        };
        window.loadedCastApi = "loading";
        var script = document.createElement("script");
        script.setAttribute("src","//www.gstatic.com/cv/js/sender/v1/cast_sender.js?loadCastFramework=1");
        document.head.appendChild(script);
        MistVideo.log("Appending chromecast script");
      }
      else {
        if (window.loadedCastApi == "loading") {
          MistVideo.log("Not appending chromecast script - still loading");
          MistVideo.timers.start(function(){
            onCastLoad();
          },200);
        }
        else {
          MistVideo.log("Not appending chromecast script - already loaded");
          onCastLoad();
        }
      }

      return ele;
    },
    airplay: function(){
      if (!window.WebKitPlaybackTargetAvailabilityEvent) { return; }
      if (!this.video) { return; }

      var MistVideo = this;
      var ele = document.createElement("div");
      ele.style.display = "none";

      var button = this.skin.icons.build("airplay");
      button.setAttribute("role","button");
      button.setAttribute("tabindex","0");
      button.setAttribute("aria-label",MistVideo.translate("airplay","AirPlay"));
      MistUtil.controlTooltip(button,MistVideo.translate("airplay","AirPlay"));
      ele.appendChild(button);

      MistVideo.video.addEventListener("webkitplaybacktargetavailabilitychanged",function(e){
        if (e.availability === "available") {
          ele.style.display = "";
        } else {
          ele.style.display = "none";
        }
      });

      MistUtil.event.addListener(button,"click",function(){
        MistVideo.video.webkitShowPlaybackTargetPicker();
      });

      return ele;
    },
    keyControls: function(){
      var MistVideo = this;
      if (!MistVideo.options.keyControls) return;
      if (!MistVideo.api) return;
      var api = MistVideo.api;

      // Configurable key map
      var defaultKeyMap = {
        togglePlay: ["k","K"," "],
        seekForward: ["l","L","ArrowRight"],
        seekBackward: ["j","J","ArrowLeft"],
        volumeUp: ["ArrowUp","+","="],
        volumeDown: ["ArrowDown","-","_"],
        toggleFullscreen: ["f","F"],
        toggleMute: ["m","M"],
        togglePip: ["i","I"],
        cycleSubtitle: ["c","C"],
        speedUp: [">","."],
        speedDown: ["<",","],
        seekToStart: ["Home"],
        seekToEnd: ["End"],
        seekPercent: ["0","1","2","3","4","5","6","7","8","9"]
      };
      var keyMap = {};
      for (var action in defaultKeyMap) {
        if (MistVideo.options.keyMap && action in MistVideo.options.keyMap) {
          keyMap[action] = MistVideo.options.keyMap[action];
        } else {
          keyMap[action] = defaultKeyMap[action];
        }
      }
      function matchesAction(key, action) {
        var keys = keyMap[action];
        if (!keys) return false;
        for (var i = 0; i < keys.length; i++) {
          if (keys[i] === key) return true;
        }
        return false;
      }

      var ele = document.createElement("div");
      var iconCon = document.createElement("div");
      var span = document.createElement("span");
      var textEle = document.createTextNode("");
      span.appendChild(textEle);
      ele.appendChild(iconCon);
      ele.appendChild(span);
      function show(text,icon) {
        if (iconCon.children.length) iconCon.removeChild(iconCon.children[0]);
        if (icon) {
          var i = MistVideo.skin.icons.build(icon,80);
          iconCon.appendChild(i);
        }
        textEle.nodeValue = text;

        ele.setAttribute("data-show","");
        setTimeout(function(){
          ele.removeAttribute("data-show");
        },500);
      }

      function getDisplayVolume() {
        return Math.round((1-Math.pow(1-api.volume,2))*100)+"%";
      }
      function snapRate(rate) {
        var speeds = [0.25,0.5,0.75,1,1.5,2,3,5];
        var index = speeds.length-1;
        for (var i in speeds) {
          var speed = speeds[i];
          if (rate < speed) {
            if ((i > 0) && (speed - rate > rate - speeds[i-1])) {
              index = i-1;
            }
            else {
              index = i;
            }
            break;
          }
        }
        return { index: index, rate: speeds[index], speeds: speeds };
      }
      var keepTimer = false;
      function showControls(keep){
        if (MistVideo.container) {
          MistVideo.container.setAttribute("data-controls","show");
          if (keepTimer) {
            MistVideo.timers.stop(keepTimer);
          }
          if (!keep) {
            MistVideo.timers.start(function(){
              MistVideo.container.removeAttribute("data-controls");
            },3e3);
          }
        }
      }

      var holdingspace = false;
      MistUtil.event.addListener(MistVideo.options.keyControls == "focus" ? MistVideo.container : document.body,"keydown",function(e){
        if (MistVideo.destroyed) {
          //should not happen
          return;
        }
        if (e.target.matches("input,textarea,select,[contenteditable]:not([contenteditable=\"false\"])")) {
          return;
        }
        
        //console.warn("keydown",JSON.stringify(e.key));
        if (matchesAction(e.key, "togglePlay")) {
          if (e.key === " ") {
            e.preventDefault();
            if (holdingspace) { return; }
            holdingspace = true;
            var speeding = false;
            var timer = false;
            var keyup = MistUtil.event.addListener(document.body,"keyup",function(e){
              if (e.key == " ") {
                holdingspace = false;
                MistUtil.event.removeListener(keyup);
                if (timer) MistVideo.timers.stop(timer);
                if (speeding) {
                  api.playbackRate = speeding;
                  show(MistVideo.translate("speed")+": "+Math.round(speeding*10)/10+"x","play");
                  speeding = false;
                }
                else {
                  if (api.paused) {
                    api.play();
                    show("","play");
                  }
                  else {
                    api.pause();
                    show("","pause");
                  }
                }
                e.preventDefault();
              }
            },ele);
            if (!(MistVideo.info && MistVideo.info.type == "live")) {
              timer = MistVideo.timers.start(function(){
                speeding = api.playbackRate;
                api.playbackRate = 2*speeding;
                if (api.paused) api.play();
                show(MistVideo.translate("speed doubled"),"forward");
              },200);
            }
          }
          else {
            if (api.paused) {
              api.play();
              show("","play");
            }
            else {
              api.pause();
              show("","pause");
            }
            e.preventDefault();
          }
        }
        else if (matchesAction(e.key, "toggleFullscreen")) {
          var button = MistVideo.container.querySelector(".mistvideo-fullscreen");
          if (button) {
            var wasFullscreen = MistVideo.container.hasAttribute("data-fullscreen");
            MistUtil.event.send("click",null,button);
            show((wasFullscreen ? "Exit" : "Enable")+" Full Screen");
          }
          e.preventDefault();
        }
        else if (matchesAction(e.key, "toggleMute")) {
          api.muted = !api.muted;
          if (!api.muted && (api.volume == 0)) api.volume = 0.29;
          if (api.muted) {
            show(MistVideo.translate("muted"),"muted");
          }
          else {
            show(MistVideo.translate("volume")+": "+getDisplayVolume(),"unmuted");
          }
          e.preventDefault();
        }
        else if (matchesAction(e.key, "volumeUp")) {
          var current = (1-Math.pow(1-api.volume,2));
          var target = Math.min(1,Math.round((current+0.1)*10)/10);
          api.volume = Math.min(1,1-Math.pow(1-target,0.5));
          if (api.muted) api.muted = false;
          show(MistVideo.translate("volume")+": "+getDisplayVolume(),"unmuted");
          e.preventDefault();
        }
        else if (matchesAction(e.key, "volumeDown")) {
          var current = (1-Math.pow(1-api.volume,2));
          var target = Math.round((current-0.1)*10)/10;
          api.volume = Math.max(0,1-Math.pow(1-target,0.5));
          show(MistVideo.translate("volume")+": "+getDisplayVolume()+(api.muted ? "\n"+MistVideo.translate("(muted)") : ""),api.muted || api.volume <= 0 ? "muted" : "unmuted");
          e.preventDefault();
        }
        else if (matchesAction(e.key, "cycleSubtitle")) {
          var select = MistVideo.container.querySelector(".mistvideo-tracks select[data-type=\"subtitle\"]");
          if (select) {
            var current = select.querySelector("option[value=\""+select.value+"\"]");
            var index = Array.from(select.children).indexOf(current);
            if (!e.shiftKey) {
              index++;
              if (index >= select.children.length) index = 0;
            }
            else {
              index--;
              if (index < 0) index = select.children.length-1;
            }
            var next = select.children[index];
            select.value = next.value;
            show(select.value == "" ? "Hide subtitles" : "Subtitle: "+next.innerHTML);
            MistUtil.event.send("change",select.value,select);
            e.preventDefault();
          }
        }
        else if (matchesAction(e.key, "seekBackward")) {
          if (MistVideo.info && MistVideo.info.type == "live") { return; }
          api.currentTime -= 10;
          show(MistVideo.translate("seek backward seconds"),"left");
          showControls();
          e.preventDefault();
        }
        else if (matchesAction(e.key, "seekForward")) {
          if (MistVideo.info && MistVideo.info.type == "live") { return; }
          api.currentTime += 10;
          show(MistVideo.translate("seek forward seconds"),"right");
          showControls();
          e.preventDefault();
        }
        else if (matchesAction(e.key, "seekToStart")) {
          api.currentTime = 0;
          show(MistVideo.translate("to start"),"left");
          showControls();
          e.preventDefault();
        }
        else if (matchesAction(e.key, "seekToEnd")) {
          api.currentTime = api.duration - (MistVideo.info && MistVideo.info.type == "live" ? 0 : 3);
          show(MistVideo.translate("to end"),"right");
          showControls();
          e.preventDefault();
        }
        else if (matchesAction(e.key, "speedUp")) {
          if (e.key === "." && api.paused && !e.shiftKey) {
            api.currentTime += 1/30;
            show(MistVideo.translate("frame forward"),"forward");
            showControls();
          }
          else {
            if (MistVideo.info && MistVideo.info.type == "live") { return; }
            var snap = snapRate(api.playbackRate);
            var set = api.playbackRate;
            if (snap.index+1 < snap.speeds.length) {
              set = snap.speeds[snap.index+1];
              api.playbackRate = set;
              if (api.paused) api.play();
            }
            show(MistVideo.translate("speed")+": "+set+"x","forward");
          }
          e.preventDefault();
        }
        else if (matchesAction(e.key, "speedDown")) {
          if (e.key === "," && api.paused && !e.shiftKey) {
            api.currentTime -= 1/30;
            show(MistVideo.translate("frame backward"),"backward");
            showControls();
          }
          else {
            if (MistVideo.info && MistVideo.info.type == "live") { return; }
            var snap = snapRate(api.playbackRate);
            var set = api.playbackRate;
            if (snap.index > 0) {
              set = snap.speeds[snap.index-1];
              api.playbackRate = set;
            }
            show(MistVideo.translate("speed")+": "+set+"x","backward");
          }
          e.preventDefault();
        }
        else if (matchesAction(e.key, "togglePip")) {
          var button = MistVideo.container.querySelector(".mistvideo-picture-in-picture");
          if (button) {
            MistUtil.event.send("click",null,button);
            e.preventDefault();
          }
        }
        else if (matchesAction(e.key, "seekPercent")) {
          if (MistVideo.info && MistVideo.info.type == "live") { return; }
          if (!isFinite(api.duration)) { return; }
          var perc = parseInt(e.key) / 10;
          api.currentTime = perc * api.duration;
          show(Math.round(perc * 100) + "%");
          showControls();
          e.preventDefault();
        }
      },ele);

      return ele;
    }
};
