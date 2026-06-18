import { MistUtil } from '../../core/util.js';
import { ThumbnailPreview } from './thumbnail.js';

var LS_VOLUME_KEY = "mistVolume";

export const playbackBlueprints = {
    progress: function(){

      var margincontainer = document.createElement("div");
      margincontainer.setAttribute("role","slider");
      margincontainer.setAttribute("aria-label",this.translate("seek bar"));
      margincontainer.setAttribute("aria-valuemin","0");
      margincontainer.setAttribute("aria-valuemax","100");
      margincontainer.setAttribute("aria-valuenow","0");
      margincontainer.setAttribute("tabindex","0");

      var container = document.createElement("div");
      margincontainer.appendChild(container);

      container.kids = {};

      container.kids.bar = document.createElement("div");
      container.kids.bar.className = "bar";
      container.appendChild(container.kids.bar);

      var video = this.video;
      var MistVideo = this;

      var first = Infinity;
      if (MistVideo.info && MistVideo.info.meta && MistVideo.info.meta.tracks) {
        for (var i in MistVideo.info.meta.tracks) {
          if (MistVideo.info.meta.tracks[i].firstms*1e-3 < first) {
            first = MistVideo.info.meta.tracks[i].firstms*1e-3;
          }
        }
      }
      if (first == Infinity) {
        first = 0;
      }
      function firsts() {
        if (MistVideo.api.duration < first) { return 0; }
        return first;
      }

      function getBufferWindow() {
        var buffer_window = MistVideo.info.meta.buffer_window;
        if (typeof buffer_window == "undefined") {
          if (MistVideo.api.buffered && MistVideo.api.buffered.length) {
            buffer_window = (MistVideo.api.duration - MistVideo.api.buffered.start(0))*1e3;
          }
          else {
            buffer_window = 60e3;
          }
        }
        return buffer_window *= 1e-3;
      }

      container.updateBar = function(currentTime){
        if (this.kids.bar) {
          if (!isFinite(MistVideo.api.duration)) { this.kids.bar.style.display = "none"; return; }
          else { this.kids.bar.style.display = ""; }

          var w = Math.min(1,Math.max(0,this.time2perc(currentTime)));
          this.kids.bar.style.width = w*100+"%";
          margincontainer.setAttribute("aria-valuenow",Math.round(w*100));
        }
      };
      container.time2perc = function(time) {
        if (!isFinite(MistVideo.api.duration)) { return 0; }
        var result = 0;
        if (MistVideo.info.type == "live") {
          var buffer_window = getBufferWindow();
          result =  (time - MistVideo.api.duration + buffer_window) / buffer_window;
        }
        else {
          result =  (time - firsts()) / (MistVideo.api.duration - firsts());
        }
        return Math.min(1,Math.max(0,result));
      }
      container.buildBuffer = function(start,end){
        var buffer = document.createElement("div");
        buffer.className = "buffer";
        buffer.style.left = (this.time2perc(start)*100)+"%";
        buffer.style.width = ((this.time2perc(end) - this.time2perc(start))*100)+"%";
        return buffer;
      };
      container.updateBuffers = function(buffers){
        var old = this.querySelectorAll(".buffer");
        for (var i = 0; i < old.length; i++) {
          this.removeChild(old[i]);
        }

        if (buffers) {
          for (var i = 0; i < buffers.length; i++) {
            this.appendChild(this.buildBuffer(
              buffers.start(i),
              buffers.end(i)
            ));
          }
        }
      };

      // Subscribe to reactive state for progress/time updates
      var lastBufferUpdate = 0;
      var bufferTimer = false;
      MistVideo.playerState.on("buffered", function(buffers){
        function updateBuffers(){
          if (new Date().getTime() - lastBufferUpdate > 1e3) {
            container.updateBuffers(buffers);
            lastBufferUpdate = new Date().getTime();
          }
          else if (!bufferTimer) {
            bufferTimer = MistVideo.timers.start(function(){
              updateBuffers();
              bufferTimer = false;
            },1e3);
          }
        }
        if (buffers) updateBuffers();
      });
      var lastBarUpdate = 0;
      var barTimer = false;
      MistVideo.playerState.on("currentTime", function(time){
        function updateBar(){
          if ((new Date().getTime() - lastBarUpdate > 200) && (!dragging)) {
            container.updateBar(time);
            lastBarUpdate = new Date().getTime();
          }
          else if (!barTimer) {
            barTimer = MistVideo.timers.start(function(){
              updateBar();
              barTimer = false;
            },1e3);
          }
        }
        updateBar();
      });
      MistVideo.playerState.on("seeking", function(isSeeking){
        if (isSeeking) {
          container.updateBar(MistVideo.api.currentTime);
        }
      });

      //control video states
      container.getPos = function(e){
        var perc = isNaN(e) ? MistUtil.getPos(this,e) : e;
        if (MistVideo.info.type == "live") {
          var bufferWindow = getBufferWindow();
          return (perc-1) * bufferWindow + MistVideo.api.duration;
        }
        else {
          if (!isFinite(MistVideo.api.duration)) { return false; }
          return perc * (MistVideo.api.duration - firsts()) + firsts();
        }
      };
      container.seek = function(e){
        var pos = this.getPos(e);
        MistVideo.api.currentTime = pos;
      };
      MistUtil.event.addListener(margincontainer,"mouseup",function(e){
        if (e.which != 1) { return;}
        container.seek(e);
      });

      //hovering
      var tooltip = MistVideo.UI.buildStructure({type:"tooltip"});

      tooltip.style.opacity = 0;
      container.appendChild(tooltip);

      // Thumbnail preview support
      var thumbCues = null;
      var thumbBaseUrl = null;
      var thumbContainer = document.createElement("div");
      thumbContainer.className = "mistvideo-thumb-container";
      tooltip.insertBefore(thumbContainer, tooltip.firstChild);

      function loadThumbnails(url) {
        thumbBaseUrl = url;
        var xhr = new XMLHttpRequest();
        xhr.open("GET", url, true);
        xhr.onload = function() {
          if (xhr.status === 200) {
            thumbCues = ThumbnailPreview.parseVTT(xhr.responseText);
          }
        };
        xhr.send();
      }
      if (MistVideo.options.thumbnails) {
        loadThumbnails(MistVideo.options.thumbnails);
      }

      MistUtil.event.addListener(margincontainer,"mouseout",function(){
        if (!dragging) { tooltip.style.opacity = 0; }
      });
      container.moveTooltip = function(e){
        var secs = this.getPos(e);
        if (secs === false) {
          tooltip.style.opacity = 0;
          return;
        }

        tooltip.setDisplay(secs);
        tooltip.style.opacity = 1;

        var perc = MistUtil.getPos(this,e);
        var pos = {bottom:20};
        if (perc > 0.5) {
          pos.right = (1-perc)*100+"%";
          tooltip.triangle.setMode("bottom","right");
        }
        else {
          pos.left = perc*100+"%";
          tooltip.triangle.setMode("bottom","left");
        }
        tooltip.setPos(pos);
      };
      var realtime = document.createElement("span");
      realtime.setAttribute("class","mistvideo-realtime");
      var realtimetext = document.createTextNode("");
      realtime.appendChild(realtimetext);

      tooltip.setDisplay = function(secs){
        // Thumbnail preview
        MistUtil.empty(thumbContainer);
        if (thumbCues && thumbCues.length) {
          var cue = ThumbnailPreview.findCue(thumbCues, secs);
          if (cue) {
            var preview = ThumbnailPreview.createPreview(cue, thumbBaseUrl);
            thumbContainer.appendChild(preview);
          }
        }

        if (MistVideo.options.useDateTime && MistVideo.info && MistVideo.info.unixoffset) {
          var range = container.getPos(1) - container.getPos(0);
          var ago = new Date().getTime()*1e-3 - (MistVideo.info.unixoffset*1e-3 + container.getPos(1));
          var scale = Math.max(range,ago);

          var str = "" ;
          var t = MistVideo.translate.bind(MistVideo);
          if (MistVideo.info.type == "live") {
            if (scale < 60) {
              str = MistUtil.format.ago(new Date(MistVideo.info.unixoffset + secs*1e3),null,t);
            }
            else {
              var secsago = new Date().getTime()*1e-3 - (MistVideo.info.unixoffset*1e-3 + secs);
              if (secsago < 48*3600) {
                str += " - "+MistUtil.format.time(secsago);
              }
            }
          }
          else {
            str += MistUtil.format.time(secs);
          }
          if (scale >= 60) {
            realtimetext.nodeValue = " "+MistVideo.translate("at")+" "+MistUtil.format.ago(new Date(MistVideo.info.unixoffset + secs*1e3),scale*1e3);
            var f = document.createDocumentFragment();
            f.appendChild(document.createTextNode(str));
            f.appendChild(realtime);
            tooltip.setHtml(f);
          }
          else {
            realtimetext.nodeValue = "";
            tooltip.setText(str);
          }

        }
        else {
          tooltip.setText(MistUtil.format.time(secs));
        }
      };
      MistUtil.event.addListener(margincontainer,"mousemove",function(e){
        container.moveTooltip(e);
      });

      //dragging
      var dragging = false;
      MistUtil.event.addListener(margincontainer,"mousedown",function(e){
        if (e.which != 1) { return;}

        dragging = true;

        container.updateBar(container.getPos(e));

        var moveListener = MistUtil.event.addListener(document,"mousemove",function(e){
          container.updateBar(container.getPos(e));
          container.moveTooltip(e);
        },container);


        var upListener = MistUtil.event.addListener(document,"mouseup",function(e){
          if (e.which != 1) { return;}
          dragging = false;

          MistUtil.event.removeListener(moveListener);
          MistUtil.event.removeListener(upListener);

          tooltip.style.opacity = 0;

          if ((!e.composedPath()) || (MistUtil.array.indexOf(e.composedPath(),margincontainer) < 0)) {
            container.seek(e);
          }
        },container);
      });

      return margincontainer;
    },
    play: function(){
      var MistVideo = this;
      var button = document.createElement("div");
      button.setAttribute("role","button");
      button.setAttribute("tabindex","0");
      button.setAttribute("aria-label",MistVideo.translate("play"));

      button.appendChild(this.skin.icons.build("play"));
      button.appendChild(this.skin.icons.build("pause"));

      var playTip = MistUtil.controlTooltip(button,MistVideo.translate("play")+" (k)");
      button.setState = function(state){
        this.setAttribute("data-state",state);
        var label = MistVideo.translate(state == "playing" ? "pause" : "play");
        this.setAttribute("aria-label",label);
        playTip.textContent = label+" (k)";
      };
      button.setState("paused");

      // Subscribe to reactive state
      MistVideo.playerState.on("playing", function(isPlaying){
        button.setState(isPlaying ? "playing" : "paused");
        if (isPlaying) MistVideo.options.autoplay = true;
      });
      MistVideo.playerState.on("ended", function(isEnded){
        if (isEnded) button.setState("paused");
      });

      //control video states
      MistUtil.event.addListener(button,"click",function(){
        if (MistVideo.api.error) { MistVideo.api.load(); }
        if (MistVideo.api.paused) {
          MistVideo.api.play();
        }
        else {
          MistVideo.api.pause();
          MistVideo.options.autoplay = false;
        }
      });

      //toggle play/pause on click on video container
      if (MistVideo.api) {
        MistUtil.event.addListener(MistVideo.video,"click",function(){
          if (MistVideo.container.hasAttribute("data-show-submenu")) {
            MistVideo.container.removeAttribute("data-show-submenu");
            return;
          }
          if (MistVideo.api.paused) { MistVideo.api.play(); }
          else if (!MistUtil.isTouchDevice()) {
            MistVideo.api.pause();
            MistVideo.options.autoplay = false;
          }
        },button);
      }

      return button;
    },
    speaker: function(){

      if (!this.api || !("muted" in this.api)) { return false; }

      var hasaudio = false;
      var tracks = this.info.meta.tracks;
      for (var i in tracks) {
        if (tracks[i].type == "audio") { hasaudio = true; break; }
      }
      if (!hasaudio) { return false; }

      var MistVideo = this;
      var button = this.skin.icons.build("speaker");
      button.setAttribute("role","button");
      button.setAttribute("tabindex","0");
      button.setAttribute("aria-label",MistVideo.translate("mute"));
      var speakerTip = MistUtil.controlTooltip(button,MistVideo.translate("mute")+" (m)");

      function updateSpeakerState(vol, isMuted) {
        if (vol && !isMuted) {
          MistUtil.class.remove(button,"off");
          button.setAttribute("aria-label",MistVideo.translate("mute"));
          speakerTip.textContent = MistVideo.translate("mute")+" (m)";
        }
        else {
          MistUtil.class.add(button,"off");
          button.setAttribute("aria-label",MistVideo.translate("unmute"));
          speakerTip.textContent = MistVideo.translate("unmute")+" (m)";
        }
      }
      // Subscribe to reactive state for mute/volume
      MistVideo.playerState.on("muted", function(isMuted){
        updateSpeakerState(MistVideo.playerState.get("volume"), isMuted);
      });
      MistVideo.playerState.on("volume", function(vol){
        updateSpeakerState(vol, MistVideo.playerState.get("muted"));
      });

      //control video states
      MistUtil.event.addListener(button,"click",function(e){
        MistVideo.api.muted = !MistVideo.api.muted;
      });

      button.addEventListener("wheel",function(e){
        e.preventDefault();
        var delta = e.deltaY > 0 ? -0.05 : 0.05;
        MistVideo.api.muted = false;

        if (MistVideo.gain) {
          var currentGain = MistVideo.gain.value;
          if (MistVideo.api.volume >= 1 && delta > 0) {
            MistVideo.gain.init();
            MistVideo.gain.value = Math.min(2, currentGain + delta);
            return;
          } else if (currentGain > 1 && delta < 0) {
            MistVideo.gain.value = Math.max(1, currentGain + delta);
            return;
          }
        }

        var newVol = Math.max(0, Math.min(1, MistVideo.api.volume + delta));
        MistVideo.api.volume = newVol;
        if (MistVideo.gain && MistVideo.gain.value > 1) { MistVideo.gain.value = 1; }
        try { localStorage[LS_VOLUME_KEY] = newVol; } catch (ex) {}
      },{passive: false});

      return button;
    },
    volume: function(options){

      if (!this.api || !("volume" in this.api)) { return false; }

      var hasaudio = false;
      var tracks = this.info.meta.tracks;
      for (var i in tracks) {
        if (tracks[i].type == "audio") { hasaudio = true; break; }
      }
      if (!hasaudio) { return false; }

      var container = document.createElement("div");
      var button = this.skin.icons.build("volume",("size" in options ? options.size : false));
      button.setAttribute("role","slider");
      button.setAttribute("aria-label",this.translate("volume"));
      button.setAttribute("aria-valuemin","0");
      button.setAttribute("aria-valuemax","100");
      button.setAttribute("aria-valuenow","100");
      button.setAttribute("tabindex","0");
      container.appendChild(button);
      var MistVideo = this;

      button.mode = ("mode" in options ? options.mode : "vertical");
      if (button.mode == "vertical") { button.style.transform = "rotate(90deg)"; }

      button.margin = {
        start: 0.15,
        end: 0.1
      };

      var video = this.video;
      button.set = function(perc){

        perc = 100 - 100 * Math.pow(1 - perc/100,2);

        if ((perc != 100) && (perc != 0)) {
          perc = this.addPadding(perc/100) * 100;
        }

        var sliders = button.querySelectorAll(".slider");
        for (var i = 0; i < sliders.length; i++) {
          sliders[i].setAttribute(button.mode == "vertical" ? "height" : "width",perc+"%");
        }
      }

      // Subscribe to reactive state for volume changes
      function updateVolume() {
        var isMuted = MistVideo.playerState.get("muted");
        var vol = MistVideo.playerState.get("volume");
        var display = isMuted ? 0 : (vol*100);
        button.set(display);
        button.setAttribute("aria-valuenow",Math.round(display));
      }
      MistVideo.playerState.on("volume", updateVolume);
      MistVideo.playerState.on("muted", updateVolume);

      //apply stored volume
      var initevent = MistUtil.event.addListener(video,"loadedmetadata",function(){
        if (('localStorage' in window) && (localStorage != null) && (LS_VOLUME_KEY in localStorage)) {
          MistVideo.api.volume = localStorage[LS_VOLUME_KEY];
        }
        MistUtil.event.removeListener(initevent);
      });

      button.addPadding = function(actual){
        return actual * (1 - (this.margin.start + this.margin.end)) + this.margin.start;
      }

      button.removePadding = function(padded){
        var val = (padded - this.margin.start) / (1 - (this.margin.start + this.margin.end));
        val = Math.max(val,0);
        val = Math.min(val,1);
        return val;
      }

      //control video states
      button.getPos = function(e){
        return this.addPadding(MistUtil.getPos(this,e));
      };
      button.setVolume = function(e){
        MistVideo.api.muted = false;

        var val = this.removePadding(MistUtil.getPos(this,e));

        val = 1 - Math.pow((1-val),0.5);
        MistVideo.api.volume = val;
        // Reset gain back to 1 when using slider
        if (MistVideo.gain && MistVideo.gain.value > 1) { MistVideo.gain.value = 1; }
        try {
          localStorage[LS_VOLUME_KEY] = MistVideo.api.volume;
        }
        catch (e) {}
      };
      MistUtil.event.addListener(button,"mouseup",function(e){
        if (e.which != 1) { return;}
        button.setVolume(e);
      });

      //hovering
      var tooltip = MistVideo.UI.buildStructure({type:"tooltip"});
      tooltip.style.opacity = 0;
      tooltip.triangle.setMode("bottom","right");
      container.style.position = "relative";
      container.appendChild(tooltip);

      MistUtil.event.addListener(button,"mouseover",function(){
        tooltip.style.opacity = 1;
      });
      MistUtil.event.addListener(button,"mouseout",function(){
        if (!dragging) { tooltip.style.opacity = 0; }
      });
      button.moveTooltip = function(e){
        tooltip.style.opacity = 1;
        var pos = MistUtil.getPos(this,e);
        var displayVol = Math.round(this.removePadding(pos)*100);
        if (MistVideo.gain && MistVideo.gain.value > 1) {
          displayVol = Math.round(MistVideo.gain.value * 100);
        }
        tooltip.setText(displayVol+"%");
        tooltip.setPos({
          bottom: 46,
          right: 100*(1-pos)+"%"
        });
      };
      MistUtil.event.addListener(button,"mousemove",function(e){
        button.moveTooltip(e);
      });

      //dragging
      var dragging = false;
      var volumeGroup = container.closest ? null : null; // resolved after DOM insertion
      function getVolumeGroup() {
        if (!volumeGroup) {
          var el = container.parentElement;
          while (el) {
            if (MistUtil.class.has(el,"mistvideo-volume_group")) { volumeGroup = el; break; }
            el = el.parentElement;
          }
        }
        return volumeGroup;
      }
      MistUtil.event.addListener(button,"mousedown",function(e){
        if (e.which != 1) { return;}
        dragging = true;
        var vg = getVolumeGroup();
        if (vg) MistUtil.class.add(vg,"dragging");
        button.setVolume(e);
        tooltip.style.opacity = 1;

        var rafId = null;
        var lastEvent = null;
        function onDragMove(ev) {
          lastEvent = ev;
          if (!rafId) {
            rafId = requestAnimationFrame(function(){
              rafId = null;
              if (lastEvent) {
                button.setVolume(lastEvent);
                button.moveTooltip(lastEvent);
              }
            });
          }
        }
        var moveListener = MistUtil.event.addListener(document,"mousemove",onDragMove,button);

        var upListener = MistUtil.event.addListener(document,"mouseup",function(e){
          if (e.which != 1) { return;}
          dragging = false;
          if (vg) MistUtil.class.remove(vg,"dragging");
          if (rafId) { cancelAnimationFrame(rafId); rafId = null; }

          MistUtil.event.removeListener(moveListener);
          MistUtil.event.removeListener(upListener);

          tooltip.style.opacity = 0;

          if ((!e.composedPath) || (!e.composedPath()) || (MistUtil.array.indexOf(e.composedPath(),button) < 0)) {
            button.setVolume(e);
          }
        },button);
      });

      container.addEventListener("wheel",function(e){
        e.preventDefault();
        var delta = e.deltaY > 0 ? -0.05 : 0.05;
        MistVideo.api.muted = false;

        if (MistVideo.gain) {
          // When at max native volume, adjust gain instead
          var currentGain = MistVideo.gain.value;
          if (MistVideo.api.volume >= 1 && delta > 0) {
            MistVideo.gain.init();
            MistVideo.gain.value = Math.min(2, currentGain + delta);
            return;
          } else if (currentGain > 1 && delta < 0) {
            MistVideo.gain.value = Math.max(1, currentGain + delta);
            return;
          }
        }

        var newVol = Math.max(0, Math.min(1, MistVideo.api.volume + delta));
        MistVideo.api.volume = newVol;
        if (MistVideo.gain && MistVideo.gain.value > 1) { MistVideo.gain.value = 1; }
        try { localStorage[LS_VOLUME_KEY] = newVol; } catch (ex) {}
      },{passive: false});

      return container;
    },
    currentTime: function(){
      var MistVideo = this;

      var container = document.createElement("div");
      var text = document.createTextNode("");
      var realtime = document.createElement("span");
      realtime.setAttribute("class","mistvideo-realtime");
      container.appendChild(text);
      container.appendChild(realtime);
      var realtimetext = document.createTextNode("");
      realtime.appendChild(realtimetext);

      var formatTime = MistUtil.format.time;
      var tr = MistVideo.translate.bind(MistVideo);
      container.set = function(){
        var v = MistVideo.api.currentTime;
        var t;
        if (MistVideo.options.useDateTime && MistVideo.info && MistVideo.info.unixoffset) {
          var d = new Date(MistVideo.info.unixoffset + v*1e3);
          var ago = new Date() - d;
          realtimetext.nodeValue = "";
          if (MistVideo.info.type == "live") {
            if (ago < 60e3){
              t = MistUtil.format.ago(d,null,tr);
            }
            else if (ago < 12*3600e3) {
              t = "- " + MistUtil.format.time(ago*1e-3);
            }
            else {
              t = MistUtil.format.ago(d,null,tr);
            }
          }
          else {
            t = formatTime(v);
            if ((ago > 60e3) && (MistVideo.size.width >= 600)) {
              realtimetext.nodeValue = " ("+MistVideo.translate("at")+" "+MistUtil.format.ago(d,null,tr)+")";
            }
          }
          container.setAttribute("title",MistUtil.format.ago(d,34560e6));
        }
        else {
          t = formatTime(v);
          container.setAttribute("title",t);
        }

        text.nodeValue = t;
      };
      container.set();

      // Subscribe to reactive state
      MistVideo.playerState.on("currentTime", function(){
        container.set();
      });
      MistVideo.playerState.on("seeking", function(isSeeking){
        if (isSeeking) container.set();
      });

      return container;
    },
    totalTime: function(){
      var MistVideo = this;

      var container = document.createElement("div");
      var text = document.createTextNode("");
      container.appendChild(text);

      if (MistVideo.info.type == "live") {
        text.nodeValue = MistVideo.translate("live");
        container.className = "live";
      }
      else {
        container.set = function(duration){
          if (isNaN(duration) || !isFinite(duration)) {
            this.style.display = "none";
            return;
          }
          this.style.display = "";

          if (MistVideo.options.useDateTime && MistVideo.info && ((MistVideo.info.type == "live") && MistVideo.info.unixoffset)) {
            var t = new Date(duration*1e3 + MistVideo.info.unixoffset)
            var tr = MistVideo.translate.bind(MistVideo);
            text.nodeValue = MistUtil.format.ago(t,null,tr);
            container.setAttribute("title",MistUtil.format.ago(t,34560e6));
          }
          else {
            text.nodeValue = MistUtil.format.time(duration);
            container.setAttribute("title",text.nodeValue);
          }
        };

        // Subscribe to reactive state
        MistVideo.playerState.on("duration", function(dur){
          container.set(dur);
        });
      }

      return container;
    },
    seekBackward: function(){
      if (!this.api || this.info.type == "live") { return; }
      var MistVideo = this;
      var button = this.skin.icons.build("seekbackward");
      button.setAttribute("role","button");
      button.setAttribute("tabindex","0");
      button.setAttribute("aria-label",MistVideo.translate("seek backward"));
      MistUtil.controlTooltip(button,MistVideo.translate("-10s"));
      MistUtil.event.addListener(button,"click",function(){
        MistVideo.api.currentTime = Math.max(0, MistVideo.api.currentTime - 10);
      });
      return button;
    },
    seekForward: function(){
      if (!this.api || this.info.type == "live") { return; }
      var MistVideo = this;
      var button = this.skin.icons.build("seekforward");
      button.setAttribute("role","button");
      button.setAttribute("tabindex","0");
      button.setAttribute("aria-label",MistVideo.translate("seek forward"));
      MistUtil.controlTooltip(button,MistVideo.translate("+10s"));
      MistUtil.event.addListener(button,"click",function(){
        MistVideo.api.currentTime = Math.min(MistVideo.api.duration, MistVideo.api.currentTime + 10);
      });
      return button;
    },
};
