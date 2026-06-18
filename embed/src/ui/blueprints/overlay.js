import { MistUtil } from '../../core/util.js';

export const overlayBlueprints = {
    text: function(options){
      var container = document.createElement("span");
      var str = options.i18n ? this.translate(options.i18n, options.text || options.i18n) : (options.text || "");
      container.appendChild(document.createTextNode(str));

      return container;
    },
    placeholder: function(){
      var placeholder = document.createElement("div");
      var MistVideo = this;

      if (this.options.fillSpace && !this.player) {
        var w,h;
        placeholder.setSize = function(){
          placeholder.style.width = w+"px";
          placeholder.style.height = h+"px";
        }

        var onInserted = function(){
          if (MistVideo.destroyed) return;
          if (placeholder.parentNode) {
            //the node has been placed into the DOM
            w = window.innerWidth;
            var aspect = 16/9;
            h = w/aspect;

            if (MistVideo.options.poster) {
              //if there is a poster, use the aspect ratio of the poster
              var img = new Image();
              img.src = MistVideo.options.poster;
              img.onload = function(){
                aspect = img.naturalWidth / img.naturalHeight;
                h = w/aspect;
                placeholder.setSize();
              }
            }

            placeholder.setSize();

            if (MistVideo.container.clientWidth < w) {
              w = MistVideo.container.clientWidth;
              h = w/aspect;
            }
         
            placeholder.setSize();

            return;
          }
          else if (MistVideo.options.target.children.length > 0) {
            //we don't have a parent but the target container does have content ; the player probably has loaded already and we don't need to further calculate the placeholder size
            return;
          }
          //not placed yet
          setTimeout(onInserted,100);
        };
        onInserted();

        MistUtil.event.addListener(window,"resize",function(){
          onInserted();
        },placeholder);

      }
      else {
        var size = this.calcSize();
        placeholder.style.width = size.width+"px";
        placeholder.style.height = size.height+"px";
      }
      if (this.options.poster) placeholder.style.background = "url('"+this.options.poster+"') no-repeat 50%/contain";
      
      return placeholder;
    },
    timeout: function(options){
      if (!"function" in options) { return; }
      var delay = ("delay" in options ? options.delay : 5);
      
      var icon = this.skin.icons.build("timeout",false,{delay:delay});
      
      icon.timeout = this.timers.start(function(){
        options.function();
      },delay*1e3);
      
      return icon;
    },
    polling: function(){
      var div = document.createElement("div");
      var icon = this.skin.icons.build("loading");
      div.appendChild(icon);
      return div;
    },
    loading: function(){
      var MistVideo = this;
      var icon = this.skin.icons.build("loading",50);
      icon.setAttribute("aria-live","polite");
      icon.setAttribute("role","status");
      
      if (MistVideo.api) {
        var timer = false;
        function addIcon(e){
          MistVideo.container.setAttribute("data-loading",e.type);
          checkIfOk();
        }
        function removeIcon(){
          MistVideo.container.removeAttribute("data-loading");
          if (timer) { MistVideo.timers.stop(timer); }
          timer = false;
        }
        function checkIfOk(){
          if (!timer) {
            //if everything is playing fine, remove the icon
            timer = MistVideo.timers.start(function(){
              timer = false;
              if ((MistVideo.monitor.vars) && (MistVideo.monitor.vars.score  >= 0.999)) { //it's playing just fine
                removeIcon();
              }
              else {
                checkIfOk();
              }
            },1e3);
          }
        }
        
        //add loading icon
        var events = ["waiting","seeking","stalled"];
        for (var i in events) {
          MistUtil.event.addListener(MistVideo.video,events[i],function(e){
            if (!MistVideo.api.paused && ("container" in MistVideo)) {
              addIcon(e);
            }
          },icon);
        }
        //remove loading icon
        var events = ["seeked","playing","canplay","paused","ended"];
        for (var i in events) {
          MistUtil.event.addListener(MistVideo.video,events[i],function(e){
            if ("container" in MistVideo) {
              removeIcon();
            }
          },icon);
        }
        MistUtil.event.addListener(MistVideo.video,"progress",function(e){
          if (("container" in MistVideo) && ("monitor" in MistVideo) && ("vars" in MistVideo.monitor) && ("score" in MistVideo.monitor.vars) && (MistVideo.monitor.vars.score > 0.99)) {
            removeIcon();
          }
        },icon);
      }
      
      return icon;
    },
    error: function(){
      var MistVideo = this;
      var container = document.createElement("div");
      container.setAttribute("aria-live","polite");
      container.setAttribute("role","alert");
      container.message = function(message,details,options){
        MistUtil.empty(this);
        var message_container = document.createElement("div");
        message_container.className = "message";
        this.appendChild(message_container);
        
        if (!options.polling && !options.passive && !options.hideTitle) {
          var header = document.createElement("h3");
          message_container.appendChild(header);
          header.appendChild(document.createTextNode(MistVideo.translate(MistVideo.casting ? "chromecast encountered a problem" : "player encountered a problem")));
        }
        
        var p = document.createElement("p");
        message_container.appendChild(p);
        message_container.update = function(message){
          MistUtil.empty(p);
          //p.appendChild(document.createTextNode(message));
          p.innerHTML = message; //allow custom html messages (configured in MI/HTTP/nostreamtext)
        };
        if (message) {
          if (MistVideo.info.on_error) {
            message = MistVideo.info.on_error.replace(/\<error>/,message);
          }
          
          message_container.update(message);
          
          var d = document.createElement("p");
          d.className = "details mistvideo-description";
          message_container.appendChild(d);
          
          if (details) {
            d.appendChild(document.createTextNode(details));
          }
          else if ("decodingIssues" in MistVideo.skin.blueprints) { //dev mode
            if (("player" in MistVideo) && ("api" in MistVideo.player) && (MistVideo.video)) {
              details = [];
              if (typeof MistVideo.state != "undefined") {
                details.push(["Stream state:",MistVideo.state]);
              }
              if (typeof MistVideo.player.api.currentTime != "undefined") {
                details.push(["Current video time:",MistUtil.format.time(MistVideo.player.api.currentTime)]);
              }
              if (("video" in MistVideo) && ("getVideoPlaybackQuality" in MistVideo.video)) {
                var data = MistVideo.video.getVideoPlaybackQuality();
                if (("droppedVideoFrames" in data) && ("totalVideoFrames" in data) && (data.totalVideoFrames)) {
                  details.push(["Frames dropped/total:",MistUtil.format.number(data.droppedVideoFrames)+"/"+MistUtil.format.number(data.totalVideoFrames)]);
                }
                if (("corruptedVideoFrames" in data) && (data.corruptedVideoFrames)) {
                  details.push(["Corrupted frames:",MistUtil.format.number(data.corruptedVideoFrames)]);
                }
              }
              var networkstates = {
                0: ["NETWORK EMPTY:","not yet initialized"],
                1: ["NETWORK IDLE:","resource selected, but not in use"],
                2: ["NETWORK LOADING:","data is being downloaded"],
                3: ["NETWORK NO SOURCE:","could not locate source"]
              };
              details.push(networkstates[MistVideo.video.networkState]);
              var readystates = {
                0: ["HAVE NOTHING:","no information about ready state"],
                1: ["HAVE METADATA:","metadata has been loaded"],
                2: ["HAVE CURRENT DATA:","data for the current playback position is available, but not for the next frame"],
                3: ["HAVE FUTURE DATA:","data for current and next frame is available"],
                4: ["HAVE ENOUGH DATA:","can start playing"]
              };
              details.push(readystates[MistVideo.video.readyState]);
              
              if (!options.passive) {
                var table = document.createElement("table")
                for (var i in details) {
                  var tr = document.createElement("tr");
                  table.appendChild(tr);
                  for (var j in details[i]) {
                    var td = document.createElement("td");
                    tr.appendChild(td);
                    td.appendChild(document.createTextNode(details[i][j]));
                  }
                }
                d.appendChild(table);
              }
            }
            var c = document.createElement("div");
            c.className = "mistvideo-container mistvideo-column";
            c.style.textAlign = "left";
            c.style.marginBottom = "1em";
            message_container.appendChild(c);
            var s = MistVideo.UI.buildStructure({type:"forcePlayer"});
            if (s) { c.appendChild(s); }
            var s = MistVideo.UI.buildStructure({type:"forceType"});
            if (s) { c.appendChild(s); }
          }
          
        }
        
        return message_container;
      };
      
      var countdown = false;
      var showingError = false;
      var since = false;
      var message_global;
      var ignoreThese = {};
      
      //add control functions to overall MistVideo object
      this.showError = function(message,options){
        if (!options) {
          options = {
            softReload: !!(MistVideo.player && MistVideo.player.api && MistVideo.player.api.load),
            reload: true,
            nextCombo: !!MistVideo.info,
            polling: false,
            passive: false
          };
        }
        
        
        var identifyer = (options.type ? options.type : message);
        if (identifyer in ignoreThese) { return; }
        
        if (options.reload === true) {
          if ((MistVideo.options.reloadDelay) && (!isNaN(Number(MistVideo.options.reloadDelay)))) {
            options.reload = Number(MistVideo.options.reloadDelay);
          }
          else {
            options.reload  = 10;
          }
        }
        if (options.passive) {
          if (showingError === true) { return; }
          if (showingError) {
            //only update the text, not the buttons or their countdowns
            message_global.update(message);
            since = (new Date()).getTime();
            return;
          }
          container.setAttribute("data-passive","");
        }
        else {
          container.removeAttribute("data-passive");
        }
        if (showingError) { container.clear(); } //stop any countdowns still running
        
        showingError = (options.passive ? "passive" : true);
        since = (new Date()).getTime();
        
        
        var event;
        if (!MistVideo.casting) { 
          event = this.log(message,"error");
        }
        var message_container = container.message(message,false,options);
        message_global = message_container;
        
        var button_container = document.createElement("div");
        button_container.className = "mistvideo-buttoncontainer";
        message_container.appendChild(button_container);
        
        MistUtil.empty(button_container);
        if (MistVideo.casting && !options.passive) {
          var obj = {
            type: "button",
            label: MistVideo.translate("stop casting"),
            onclick: function(){
              MistVideo.detachFromCast();
            }
          };
          if (!isNaN(options.softReload+"")) { obj.delay = options.softReload; }
          button_container.appendChild(MistVideo.UI.buildStructure(obj));

        }
        if (options.softReload && !MistVideo.casting) {
          var obj = {
            type: "button",
            label: MistVideo.translate("reload video"),
            onclick: function(){
              MistVideo.player.api.load();
            }
          };
          if (!isNaN(options.softReload+"")) { obj.delay = options.softReload; }
          button_container.appendChild(MistVideo.UI.buildStructure(obj));
        }
        if (options.reload) {
          var obj = {
            type: "button",
            label: MistVideo.translate("reload player"),
            onclick: function(){
              MistVideo.reload("Reloading because reload button was clicked.");
            }
          };
          if (!isNaN(options.reload+"")) { obj.delay = options.reload; }
          button_container.appendChild(MistVideo.UI.buildStructure(obj));
        }
        if (options.nextCombo) {
          var obj = {
            type: "button",
            label: MistVideo.translate("next source"),
            onclick: function(){
              MistVideo.nextCombo();
            }
          };
          if (!isNaN(options.nextCombo+"")) { obj.delay = options.nextCombo; }
          button_container.appendChild(MistVideo.UI.buildStructure(obj));
        }
        if (options.ignore) {
          var obj = {
            type: "button",
            label: MistVideo.translate("ignore"),
            onclick: function(){
              this.clearError();
              ignoreThese[identifyer] = true;
              //stop showing this error
            }
          };
          if (!isNaN(options.ignore+"")) { obj.delay = options.ignore; }
          button_container.appendChild(MistVideo.UI.buildStructure(obj));
        }
        if (options.polling) {
          button_container.appendChild(MistVideo.UI.buildStructure({type:"polling"}));
        }
        
        MistUtil.class.add(container,"show");
        if ("container" in MistVideo) {
          MistVideo.container.removeAttribute("data-loading");
        }
        
        if (event && event.defaultPrevented) {
          MistVideo.log("Error event was defaultPrevented, not showing.");
          container.clear();
        }
      };
      container.clear = function(){
        var countdowns = container.querySelectorAll("svg.icon.timeout");
        for (var i = 0; i < countdowns.length; i++) {
          MistVideo.timers.stop(countdowns[i].timeout);
        }
        
        MistUtil.empty(container);
        MistUtil.class.remove(container,"show");
        
        showingError = false;
      };
      this.clearError = container.clear;
      
      //listener to clear error window
      if ("video" in MistVideo) {
        var events = ["timeupdate","playing","canplay"];//,"progress"];
        for (var i in events) {
          MistUtil.event.addListener(MistVideo.video,events[i],function(e){
            if (!showingError) { return; }
            if (e.type == "timeupdate") {
              if (MistVideo.player.api.currentTime == 0) { return; }
              if (((new Date()).getTime() - since) < 2e3) { return; }
            }
            MistVideo.log("Removing error window because of "+e.type+" event");
            container.clear();
          },container);
        }
      }
      
      return container;
    },
    tooltip: function(){
      var container = document.createElement("div");

      var mode = "text";
      var textNode = document.createTextNode("");
      container.appendChild(textNode);
      container.setText = function(text){
        textNode.nodeValue = text;
        if (mode != "text") {
          container.removeChild(htmlNode);
          container.appendChild(textNode);
          mode = "text";
        }
      };
      var htmlNode = document.createElement("div");
      htmlNode.empty = function(){
        htmlNode.innerText = "";
        for (var i = htmlNode.children.length - 1; i >= 0; i--) {
          htmlNode.removeChild(htmlNode.children[i]);
        }
      };
      container.setHtml = function(ele){
        htmlNode.empty();
        htmlNode.appendChild(ele);
        if (mode != "html") {
          container.removeChild(textNode);
          container.appendChild(htmlNode);
          mode = "html";
        }
      }
      
      var triangle = document.createElement("div");
      container.triangle = triangle;
      triangle.className = "triangle";
      container.appendChild(triangle);
      triangle.setMode = function(primary,secondary){
        if (!primary) { primary = "bottom"; }
        if (!secondary) { secondary = "left"; }
        
        //reset styles
        var sides = ["bottom","top","right","left"];
        for (var i in sides) {
          this.style[sides[i]] = "";             //bottom
          var cap = MistUtil.format.ucFirst(sides[i]);
          this.style["border"+cap] = "";         //borderBottom
          this.style["border"+cap+"Color"] = ""; //borderBottomColor
        }
        
        var opposite = {
          top: "bottom",
          bottom: "top",
          left: "right",
          right: "left"
        };
        
        //set styles
        this.style[primary] = "-10px";                                                 //bottom
        this.style["border"+MistUtil.format.ucFirst(opposite[primary])] = "none";      //borderTop
        this.style["border"+MistUtil.format.ucFirst(primary)+"Color"] = "transparent"; //borderBottomColor
        this.style[secondary] = 0;                                                     //left
        this.style["border"+MistUtil.format.ucFirst(opposite[secondary])] = "none";    //borderRight
      };
      
      container.setPos = function(pos){
        
        //also apply the "other" values, to reset if direction mode is switched
        var set = {
          left: "auto",
          right: "auto",
          top: "auto",
          bottom: "auto"
        };
        MistUtil.object.extend(set,pos);
        
        for (var i in set) {
          if (!isNaN(set[i])) { set[i] += "px"; } //add px if the value is a number
          this.style[i] = set[i];
        }
      };
      
      return container;
    },
    button: function(options){ //label,onclick,timeout){
      var button = document.createElement("button");
      var MistVideo = this;
      
      if (options.onclick) {
        MistUtil.event.addListener(button,"click",function(){
          options.onclick.call(MistVideo,arguments);
        });
        
        if (options.delay) {
          var countdown = this.UI.buildStructure({
            type: "timeout",
            delay: options.delay,
            function: options.onclick
          });
          if (countdown) {
            button.appendChild(countdown);
          }
        }
      }
      
      var labelStr = options.i18n ? this.translate(options.i18n, options.label || options.i18n) : (options.label || "");
      button.appendChild(document.createTextNode(labelStr));

      return button;
   },
   videobackground: function(options) {
      /* options.alwaysDisplay : if true, always draw the video on the canvas */
      /* options.delay         : delay of the draw timeout in seconds */
      if (!options) { options = {}; }
      if (!options.delay) { options.delay = 5; }

      var ele = document.createElement("div");
      var MistVideo = this;

      if (MistVideo.options.rotate) {
        switch (MistVideo.options.rotate) {
          case -1:
          case 1:
          case 2: {
            ele.setAttribute("data-mist-rotate",MistVideo.options.rotate);
            break;
          }
        }
      }
      
      var canvasses = [];
      for (var n = 0; n < 2; n++) {
        var c = document.createElement("canvas");
        c._context = c.getContext("2d");
        ele.appendChild(c);
        canvasses.push(c);
      }
      
      var index = 0;
      var drawing = false;
      function draw() {
        //only draw if the element is visible, don't waste cpu
        if (options.alwaysDisplay || (MistVideo.video.videoWidth/MistVideo.video.videoHeight != ele.clientWidth / ele.clientHeight)) {

          canvasses[index].removeAttribute("data-front"); //put last one behind again
          //console.log(new Date().toLocaleTimeString(),"draw");

          index++;
          if (index >= canvasses.length) { index = 0; }

          var c = canvasses[index];
          var ctx = c._context;

          c.width = MistVideo.video.videoWidth;
          c.height = MistVideo.video.videoHeight;
          ctx.drawImage(MistVideo.video,0,0);
          c.setAttribute("data-front","");
        }
        
        if (!MistVideo.player.api.paused) {
          MistVideo.timers.start(function(){
            draw();
          },options.delay * 1e3);
        }
        else {
          drawing = false;
        }

      }
      MistUtil.event.addListener(MistVideo.video,"playing",function(){
        if (!drawing) {
          draw();
          drawing = true;
        }
      });


      return ele;
    },
};
