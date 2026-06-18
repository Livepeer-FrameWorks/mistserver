import { MistUtil } from '../../core/util.js';

export const containerBlueprints = {
    container: function(){
      var container = document.createElement("div");
      
      return container;
    },
    video: function(){
      var MistVideo = this;

      if (MistVideo.options.rotate) {
        switch (MistVideo.options.rotate) {
          case -1:
          case 1:
          case 2: {
            MistVideo.video.setAttribute("data-mist-rotate",MistVideo.options.rotate);
            break;
          }
        }
      }
      
      //hide the cursor after some time
      MistVideo.video.hideTimer = false;
      MistVideo.video.hideCursor = function(){
        if (this.hideTimer) { clearTimeout(this.hideTimer); }
        this.hideTimer = MistVideo.timers.start(function(){
          MistVideo.container.setAttribute("data-hidecursor","");
          var controlsContainer = MistVideo.container.querySelector(".mistvideo-controls");
          if (controlsContainer) { controlsContainer.parentNode.setAttribute("data-hidecursor",""); }
        },3e3);
      };
      MistUtil.event.addListener(MistVideo.video,"mousemove",function(){
        MistVideo.container.removeAttribute("data-hidecursor");
        var controlsContainer = MistVideo.container.querySelector(".mistvideo-controls");
        if (controlsContainer) { controlsContainer.parentNode.removeAttribute("data-hidecursor"); }
        MistVideo.video.hideCursor();
      });
      MistUtil.event.addListener(MistVideo.video,"mouseout",function(){
        //stop the timer if no longer over the video element
        if (MistVideo.video.hideTimer) { MistVideo.timers.stop(MistVideo.video.hideTimer); }
      });
      
      // Double-tap to seek (mobile)
      if (MistUtil.isTouchDevice() && MistVideo.api) {
        var lastTap = 0;
        var lastSide = null;
        var tapTimer = null;
        MistUtil.event.addListener(MistVideo.video,"touchend",function(e){
          if (!MistVideo.api || !isFinite(MistVideo.api.duration)) { return; }
          if (MistVideo.info && MistVideo.info.type == "live") { return; }

          var now = Date.now();
          var rect = MistVideo.video.getBoundingClientRect();
          var touch = e.changedTouches[0];
          var x = touch.clientX - rect.left;
          var side = x < rect.width * 0.4 ? "left" : x > rect.width * 0.6 ? "right" : null;

          if (side && side === lastSide && (now - lastTap) < 300) {
            // Double-tap detected
            e.preventDefault();
            if (tapTimer) { clearTimeout(tapTimer); tapTimer = null; }
            var delta = side === "right" ? 10 : -10;
            MistVideo.api.currentTime = Math.max(0, Math.min(MistVideo.api.duration, MistVideo.api.currentTime + delta));

            // Show overlay animation
            var overlay = document.createElement("div");
            overlay.className = "mistvideo-doubletap-overlay";
            overlay.setAttribute("data-side",side);
            overlay.textContent = (delta > 0 ? "+" : "") + delta + "s";
            MistVideo.container.appendChild(overlay);
            setTimeout(function(){ if (overlay.parentNode) overlay.parentNode.removeChild(overlay); },600);

            lastTap = 0;
            lastSide = null;
          } else {
            lastTap = now;
            lastSide = side;
            tapTimer = setTimeout(function(){ lastTap = 0; lastSide = null; tapTimer = null; },300);
          }
        });
      }

      //improve autoplay behaviour
      if (MistVideo.options.autoplay) {
        //play without muting first, mute if play failed

        //because Mist doesn't send data instantly (but real time), it can take a little while before canplaythrough is fired. Rather than wait, we can just start playing at the canplay event
        var canplay = MistUtil.event.addListener(MistVideo.video,"canplay",function(){
          if (MistVideo.api && MistVideo.api.paused) {
            if (!MistVideo.info.hasAudio) {
              //we might as well..
              MistVideo.api.muted = true;
              //before wasMuted because we don't need to show the large muted button
            }

            var wasMuted = MistVideo.api.muted;
            //console.warn("Muted state:",wasMuted);

            if (MistUtil.getBrowser() == "safari") {
              MistVideo.log("Muting before autoplay because this is safari");
              //..and if you try to autoplay a non-muted video, safari will "lock" the video where the video element itself must be interacted with before it will play, even if player interaction had occured previously
              MistVideo.api.muted = true;
            }

            function autoplayFailed() {
              MistVideo.log("Autoplay failed even with muted video. Unmuting and showing play button.");

              //wait 5 seconds and then pause the download
              MistVideo.timers.start(function(){
                if (MistVideo.api.paused) {
                  //don't question it
                  //if the video is paused, also request the player api to pause
                  //for example, for mews, this would pause the download
                  MistVideo.api.pause(); 
                  if (MistVideo.monitor) { MistVideo.monitor.destroy(); }
                }
              },5e3);

              if (MistVideo.reporting) { MistVideo.reporting.stats.d.autoplay = "failed"; }
              MistVideo.api.muted = false;

              //show large centered play button
              var largePlayButton = MistVideo.skin.icons.build("largeplay",150);
              MistUtil.class.add(largePlayButton,"mistvideo-pointer");
              MistVideo.container.appendChild(largePlayButton);

              //start playing on click
              MistUtil.event.addListener(largePlayButton,"click",function(){
                if (MistVideo.api.paused) {
                  MistVideo.api.play();
                }
              });

              //remove large button on play
              var f = function (){
                MistVideo.container.removeChild(largePlayButton);
                MistVideo.video.removeEventListener("play",f);
              };
              MistUtil.event.addListener(MistVideo.video,"play",f);

            }
            function autoplayMuted() {
              //show large muted button
              MistVideo.log("Autoplay worked! Video will be unmuted on mouseover if the page has been interacted with.");
              
              if (MistVideo.reporting) { MistVideo.reporting.stats.d.autoplay = "muted"; }
              
              //show large "muted" icon
              var largeMutedButton = MistVideo.skin.icons.build("muted",100);
              MistUtil.class.add(largeMutedButton,"mistvideo-pointer");
              MistVideo.container.appendChild(largeMutedButton);
              
              MistUtil.event.addListener(largeMutedButton,"click",function(){
                MistVideo.api.muted = false;
                MistVideo.container.removeChild(largeMutedButton);
              });
              
              //listen for page interactions
              var interacted = false;
              var i = function(){
                interacted = true;
                document.body.removeEventListener("click",i);
              };
              MistUtil.event.addListener(document.body,"click",i,MistVideo.video);
              
              
              //turn sound back on on mouseover
              var f = function(){
                if (interacted) {
                  MistVideo.api.muted = false;
                  MistVideo.video.removeEventListener("mouseenter",f);
                  MistVideo.log("Re-enabled sound");
                }
              };
              MistUtil.event.addListener(MistVideo.video,"mouseenter",f);
              
              //remove all the things when unmuted
              var fu = function(){
                if (!MistVideo.api.muted) {
                  if (largeMutedButton.parentNode) {
                    MistVideo.container.removeChild(largeMutedButton);
                  }
                  MistVideo.video.removeEventListener("volumechange",fu);
                  document.body.removeEventListener("click",i);
                  MistVideo.video.removeEventListener("mouseenter",f);
                }
              }
              MistUtil.event.addListener(MistVideo.video,"volumechange",fu);
            }

            var promise = MistVideo.api.play();
            if (promise) {
              promise.then(function(){
                if (MistVideo.api.muted != wasMuted) {
                  autoplayMuted();
                }
                else {
                  MistVideo.log("Autoplay worked! muted:"+MistVideo.api.muted);
                }
              }).catch(function(e){
                if (MistVideo.destroyed) { return; }

                //play has failed
                if (MistVideo.info.hasVideo && !MistVideo.api.muted) {
                  MistVideo.log("Autoplay failed. Retrying with muted audio..");

                  //try again with sound muted
                  MistVideo.api.muted = true;

                  var promise = MistVideo.api.play();
                  if (promise) {
                    promise.then(function(){
                      if (MistVideo.reporting) { MistVideo.reporting.stats.d.autoplay = "success"; }
                    }).then(function(){
                      if (MistVideo.destroyed) { return; }
                      autoplayMuted();
                    }).catch(function(){
                      if (MistVideo.destroyed) { return; }
                      autoplayFailed();
                    });
                  }
                }
                else {
                  autoplayFailed();
                }
              });
            }
          }
          else if (MistVideo.reporting) { MistVideo.reporting.stats.d.autoplay = "success"; }

          MistUtil.event.removeListener(canplay); //only fire once
        });

      }
      
      return this.video;
    },
    videocontainer: function(){
      return this.UI.buildStructure(this.skin.structure.videocontainer);
    },
    secondaryVideo: function(o){
      if (!o) { o = {}; }
      if (!o.options) { o.options = {}; }
      
      var MistVideo = this;
      
      if (!("secondary" in MistVideo)) {
        MistVideo.secondary = [];
      }
      
      var options = MistUtil.object.extend({},MistVideo.options);
      options = MistUtil.object.extend(options,o.options);
      MistVideo.secondary.push(options);
      
      var pointer = {
        primary: MistVideo,
        secondary: false
      };
      
      options.target = document.createElement("div");
      delete options.container;
      
      var mvo = {};
      options.MistVideoObject = mvo;
      
      MistUtil.event.addListener(options.target,"initialized",function(){
        var mv = mvo.reference;
      //options.callback = function(mv){
        options.MistVideo = mv; //tell the main video we exist
        pointer.secondary = mv;
        
        mv.player.api.muted = true; //disable sound
        mv.player.api.loop = false; //disable looping, master will do that for us
        
        //as all event listeners are tied to the video element (not the container), events don't bubble up and disturb higher players
        
        //prevent clicks on the control container from bubbling down to underlying elements
        var controlContainers = options.target.querySelectorAll(".mistvideo-controls");
        for (var i = 0; i < controlContainers.length; i++) {
          MistUtil.event.addListener(controlContainers[i],"click",function(e){
            e.stopPropagation();
          });
        }
        
        //ensure the state of the main player is copied
        MistUtil.event.addListener(MistVideo.video,"play",function(){
          if (mv.player.api.paused) { mv.player.api.play(); }
        },options.target);
        MistUtil.event.addListener(MistVideo.video,"pause",function(e){
          if (!mv.player.api.paused) { mv.player.api.pause(); }
        },options.target);
        MistUtil.event.addListener(MistVideo.video,"seeking",function(){
          mv.player.api.currentTime = this.currentTime;
        },options.target);
        MistUtil.event.addListener(MistVideo.video,"timeupdate",function(){
          if (mv.player.api.pausedesync) { return; }
          
          //sync
          var desync = this.currentTime - mv.player.api.currentTime;
          var adesync = Math.abs(desync);
          if (adesync > 30) {
            mv.player.api.pausedesync = true;
            mv.player.api.currentTime = this.currentTime;
            mv.log("Re-syncing with main video by seeking (desync: "+desync+"s)");
          }
          else if (adesync > 0.01) {
            var rate = 0.1;
            if (adesync < 1) {
              rate = 0.05;
            }
            rate = 1 + rate * Math.sign(desync);
            if (rate != mv.player.api.playbackRate) {
              mv.log("Re-syncing by changing the playback rate (desync: "+Math.round(desync*1e3)+"ms, rate: "+rate+")");
            }
            mv.player.api.playbackRate = rate;
          }
          else if (mv.player.api.playbackRate != 1) {
            mv.player.api.playbackRate = 1;
            mv.log("Sync with main video achieved (desync: "+Math.round(desync*1e3)+"ms)");
          }
        },options.target);
        MistUtil.event.addListener(mv.video,"seeked",function(){
          //don't attempt to correct sync if we're already seeking
          mv.player.api.pausedesync = false;
        });
        
      });
      options.skin = MistUtil.object.extend({},MistVideo.skin,true);
      options.skin.structure.main = MistUtil.object.extend({},MistVideo.skin.structure.secondaryVideo(pointer));
      
      mistPlay(MistVideo.stream,options);
      
      return options.target;
    },
    switchVideo: function(options){
      var container = document.createElement("div");
      
      container.appendChild(this.skin.icons.build("switchvideo"));
      
      MistUtil.event.addListener(container,"click",function(){
        var primary = options.containers.primary;
        var secondary = options.containers.secondary;
        
        function findVideo(startAt,matchTarget) {
          if (startAt.video.currentTarget == matchTarget) {
            return startAt.video;
          }
          if (startAt.secondary) {
            for (var i = 0; i < startAt.secondary.length; i++) {
              var result = findVideo(startAt.secondary[i].MistVideo,matchTarget);
              if (result) { return result; }
            }
          }
          return false;
        }
        
        //find video element in primary/secondary containers
        var pv = findVideo(primary,primary.options.target);
        var sv = findVideo(primary,secondary.options.target);
        //prevent pausing the primary
        var playit = !pv.paused;
        //switch them
        var place = document.createElement("div");
        sv.parentElement.insertBefore(place,sv);
        pv.parentElement.insertBefore(sv,pv);
        place.parentElement.insertBefore(pv,place);
        place.parentElement.removeChild(place);
        if (playit) {
          try {
            pv.play();
            sv.play();
          } catch(e) {}
        }
        
        var tmp = {
          width: pv.style.width,
          height: pv.style.height,
          currentTarget: pv.currentTarget
        };/*
        pv.style.width = sv.style.width;
        pv.style.height = sv.style.height;
        sv.style.width = tmp.width;
        sv.style.height = tmp.height;*/
        pv.currentTarget = sv.currentTarget;
        sv.currentTarget = tmp.currentTarget;
        primary.player.resizeAll();
      });
      
      return container;
    },
};
