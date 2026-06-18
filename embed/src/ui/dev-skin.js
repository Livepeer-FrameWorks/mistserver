import { MistUtil } from '../core/util.js';
import { MistSkins } from './default-skin.js';
import { mistplayers } from '../core/registry.js';
import { createMenu } from './blueprints/menu.js';
import { locales } from '../core/locales.js';
import { MistThemes, resolveTheme } from '../core/themes.js';


MistSkins.dev = {
  structure: MistUtil.object.extend({},MistSkins["default"].structure,true),
  blueprints: {
    timeout: function(){
      //don't use countdowns on buttons unless MistVideo.options.reloadDelay is set
      if (this.options.reloadDelay !== false) {
        return MistSkins.default.blueprints.timeout.apply(this,arguments);
      }
      return false;
    },
    log: function(){
      var container = document.createElement("div");
      container.appendChild(document.createTextNode(this.translate("logs")));
      var logsc = document.createElement("div");//scroll this
      logsc.className = "logs";
      container.appendChild(logsc);
      var logs = document.createElement("table");
      logsc.appendChild(logs);
      
      var MistVideo = this;
      var lastmessage = {message: false};
      var count = false;
      var scroll = true;
      function addMessage(time,message,data) {
        if (!data) { data = {}; }
        
        if (lastmessage.message == message) {
          count++;
          
          lastmessage.counter.nodeValue = count;
          if ((count == 2) && (lastmessage.counter.parentElement)) {
            lastmessage.counter.parentElement.style.display = "";
          }
          
          return;
        }
        
        count = 1;
        
        var entry = document.createElement("tr");
        entry.className = "entry";
        if ((data.type) && (data.type != "log")) {
          MistUtil.class.add(entry,"type-"+data.type);
          message = MistUtil.format.ucFirst(data.type)+": "+message;
        }
        logs.appendChild(entry);
        
        var timestamp = document.createElement("td");
        timestamp.className = "timestamp";
        entry.appendChild(timestamp);
        var stamp = time.toLocaleTimeString(); //get current time in local format
        //add miliseconds
        var t = stamp.split(" ");
        t[0] += "."+("00"+time.getMilliseconds()).slice(-3);
        //t = t.join(" ");
        timestamp.appendChild(document.createTextNode(t[0]));
        if ("currentTime" in data) {
          timestamp.title = "Video playback time: "+MistUtil.format.time(data.currentTime,{ms:true});
        }
        
        var td = document.createElement("td");
        entry.appendChild(td);
        var msg = document.createElement("span");
        msg.className = "message";
        td.appendChild(msg);
        msg.appendChild(document.createTextNode(message));
        
        var counter = document.createElement("span");
        counter.style.display = "none";
        counter.className = "counter";
        td.appendChild(counter);
        var countnode = document.createTextNode(count);
        counter.appendChild(countnode);
        
        if (scroll) { logsc.scrollTop = logsc.scrollHeight; }
        
        lastmessage = {message: message, counter: countnode};
      }
      
      MistUtil.event.addListener(logsc,"scroll",function(){
        //console.log(logsc.scrollTop + logsc.clientHeight,logsc.scrollHeight);
        if (logsc.scrollTop + logsc.clientHeight >= logsc.scrollHeight - 5) {
          scroll = true;
        }
        else {
          scroll = false;
        }
        //console.log(scroll);
      })
      
      //add previously generated log messages
      for (var i in MistVideo.logs) {
        addMessage(MistVideo.logs[i].time,MistVideo.logs[i].message,MistVideo.logs[i].data);
      }
      
      MistUtil.event.addListener(MistVideo.options.target,"log",function(e){
        if (!e.message) { return; }
        var data = {};
        if (MistVideo.player && MistVideo.player.api && ("currentTime" in MistVideo.player.api)) {
          data.currentTime = MistVideo.player.api.currentTime;
        }
        addMessage(new Date(),e.message,data);
      },container);
      MistUtil.event.addListener(MistVideo.options.target,"error",function(e){
        if (!e.message) { return; }
        var data = {type:"error"};
        if (MistVideo.player && MistVideo.player.api && ("currentTime" in MistVideo.player.api)) {
          data.currentTime = MistVideo.player.api.currentTime;
        }
        addMessage(new Date(),e.message,data);
      },container);
      
      return container;
    },
    decodingIssues: function(){
      if (!this.player) { return; }
      
      var MistVideo = this;
      var container = document.createElement("div");
      
      function buildItem(options){
        var label = document.createElement("label");
        container.appendChild(label);
        label.style.display = "none";
        
        var text = document.createElement("span");
        label.appendChild(text);
        text.appendChild(document.createTextNode(options.name+":"));
        text.className = "mistvideo-description";
        
        var valuec = document.createElement("span");
        label.appendChild(valuec);
        var value = document.createTextNode((options.value ? options.value : ""));
        valuec.appendChild(value);
        var ele = document.createElement("span");
        valuec.appendChild(ele);
        
        label.set = function(val){
          if (val !== 0 && (typeof val != "object" || MistUtil.object.keys(val).length > 0 )) { this.style.display = ""; }
          else return;
          if (typeof val == "object") {
            try {
              if (val instanceof Promise) {
                val.then(function(val){
                  label.set(val)
                },function(){});
                return;
              }
              if (val instanceof HTMLElement) {
                value.nodeValue = "";
                ele.innerHTML = "";
                ele.appendChild(val);
                return;
              }
              if (!("val" in val)) {
                return label.set(JSON.stringify(val));
              }
            }
            catch (e) {}
            if ("val" in val) {
              value.nodeValue = val.val;
              valuec.className = "value";
            }
            //is there a graph already?
            if (ele.children.length) {
              var graph = ele.children[0];
              return graph.addData(val);
            }
            else {
              //create a graph
              var graph = MistUtil.createGraph({x:[val.x],y:[val.y]},val.options);

              //it's (probably) a DOM element, insert it
              ele.style.display = "";
              MistUtil.empty(ele);
              return ele.appendChild(graph);
            }
          }
          return value.nodeValue = val;
        };
        
        container.appendChild(label);
        updates.push(function(){
          var result = options.function();
          label.set(result);
        });
      }
      
      if (MistVideo.player.api) {
        var videovalues = {
          "playback score": function(){
            if ("monitor" in MistVideo) {
              if (("vars" in MistVideo.monitor) && ("score" in MistVideo.monitor.vars)) {
                if (MistVideo.monitor.vars.values.length) {
                  var last = MistVideo.monitor.vars.values[MistVideo.monitor.vars.values.length - 1];
                  if ("score" in last) {
                    var score = Math.min(1,Math.max(0,last.score));
                    return {
                      x: last.clock,
                      y: Math.min(1,Math.max(0,last.score)),
                      options: {
                        y: {
                          min: 0,
                          max: 1
                        },
                        x: {
                          count: 10
                        }
                      },
                      val: Math.round(Math.min(1,Math.max(0,MistVideo.monitor.vars.score))*100)+"%"
                    };
                  }
                }
              }
              return 0;
            }
          },
          "corrupted frames": function(){
            if ((MistVideo.player.api) && ("getVideoPlaybackQuality" in MistVideo.player.api)) {
              var r = MistVideo.player.api.getVideoPlaybackQuality();
              if (r) {
                if (r.corruptedVideoFrames) {
                  return {
                    val: MistUtil.format.number(r.corruptedVideoFrames),
                    x: (new Date()).getTime()*1e-3,
                    y: r.corruptedVideoFrames,
                    options: {
                      x: { count: 10 }
                    }
                  };
                }
                return 0;
              }
            }
          },
          "dropped frames": function(){
            if (MistVideo.player.api) {
              if ("getVideoPlaybackQuality" in MistVideo.player.api) {
                var r = MistVideo.player.api.getVideoPlaybackQuality();
                if (r) {
                  if (r.droppedVideoFrames) {
                    return MistUtil.format.number(r.droppedVideoFrames);
                    /* show a graph: return {
                    val: MistUtil.format.number(r.droppedVideoFrames),
                    x: (new Date()).getTime()*1e-3,
                    y: r.droppedVideoFrames,
                    options: {
                      x: { count: 10 },
                      differentiate: true,
                      reverseGradient: true
                    }
                  };*/
                  }
                  return 0;
                }
              }
              if ("webkitDroppedFrameCount" in MistVideo.player.api) {
                return MistVideo.player.api.webkitDroppedFrameCount;
              }
            }
          },
          "total frames": function(){
            if ((MistVideo.player.api) && ("getVideoPlaybackQuality" in MistVideo.player.api)) {
              var r = MistVideo.player.api.getVideoPlaybackQuality();
              if (r) { return MistUtil.format.number(r.totalVideoFrames); }
            }
          },
          "decoded audio": function(){
            if (MistVideo.player.api) {
              return MistUtil.format.bytes(MistVideo.player.api.webkitAudioDecodedByteCount);
            }
          },
          "decoded video": function(){
            if (MistVideo.player.api) {
              return MistUtil.format.bytes(MistVideo.player.api.webkitVideoDecodedByteCount);
            }
          },
          "nack": function(){
            if (MistVideo.player.api && MistVideo.player.api.nackCount) {
              return processMultiOutput(MistVideo.player.api.nackCount,MistUtil.format.number);
            }
          },
          "picture losses": function(){
            if (MistVideo.player.api && MistVideo.player.api.pliCount) {
              return processMultiOutput(MistVideo.player.api.pliCount,MistUtil.format.number);
            }
          },
          "packets lost": function(){
            if (MistVideo.player.api && MistVideo.player.api.packetsLost) {
              return processMultiOutput(MistVideo.player.api.packetsLost,MistUtil.format.number);
            }
          },
          "packets received": function(){
            if (MistVideo.player.api && MistVideo.player.api.packetsReceived) {
              return processMultiOutput(MistVideo.player.api.packetsReceived,MistUtil.format.number);
            }
          },
          "bytes received": function(){
            if (MistVideo.player.api && MistVideo.player.api.bytesReceived) {
              return processMultiOutput(MistVideo.player.api.bytesReceived,function(v){
                return MistUtil.format.bytes(v);
              });
            }
          },
          "local latency": function(){
            if (MistVideo.player.api && MistVideo.player.api.jitterDelay) {
              return processMultiOutput(MistVideo.player.api.jitterDelay,function(v){
                return MistUtil.format.number(v*1e3)+"ms";
              });
            }
          },
          "messages received": function(){
            if (MistVideo.player.api && MistVideo.player.api.messagesReceived) {
              return processMultiOutput(MistVideo.player.api.messagesReceived,MistUtil.format.number);
            }
          },
          "messages sent": function(){
            if (MistVideo.player.api && MistVideo.player.api.messagesSent) {
              return processMultiOutput(MistVideo.player.api.messagesSent,MistUtil.format.number);
            }
          },
          "current bitrate": function(){
            if (MistVideo.player.monitor && ("currentBps" in MistVideo.player.monitor)) {
              var out = MistUtil.format.bits(MistVideo.player.monitor.currentBps);
              return out ? out+"ps" : out;
            }
            if (MistVideo.player.api && "currentBps" in MistVideo.player.api) {
              var out = MistUtil.format.bits(MistVideo.player.api.currentBps());
              return out ? out+"ps" : out;
            }
          },
          "framerate in": function(){
            if (MistVideo.player.api && "framerate_in" in MistVideo.player.api) {
              return MistUtil.format.number(MistVideo.player.api.framerate_in());
            }
          },
          "framerate out": function(){
            if (MistVideo.player.api && "framerate_out" in MistVideo.player.api) {
              return MistUtil.format.number(MistVideo.player.api.framerate_out());
            }
          }
        };
        function processMultiOutput(values,formatter){
          if (typeof values == "number") {
            return formatter(values);
          }
          var out = document.createElement("div");
          for (var i in values) {
            if (values[i]) {
              var c = document.createElement("div"); //container
              var l = document.createElement("span"); //label
              l.className = "mistvideo-description";
              c.appendChild(l);
              l.appendChild(document.createTextNode(i[0]+": "));
              l.setAttribute("title",i);
              var v = document.createElement("span"); //value
              c.appendChild(v);
              v.appendChild(document.createTextNode(formatter(values[i])));
              out.appendChild(c);
            }
          }
          return out.children.length ? out : 0;
        }
        var updates = [];
        for (var i in videovalues) {
          if (typeof videovalues[i]() == "undefined") { continue; }
          buildItem({
            name: MistVideo.translate(i),
            function: videovalues[i]
          });
        }
        container.update = function(){
          for (var i in updates) {
            updates[i]();
          }
          MistVideo.timers.start(function(){
            container.update();
          },1e3);
        };
        container.update();
      }
      
      return container;
    },
    forcePlayer: function(){
      var container = document.createElement("div");
      container.className = "mistvideo-force-row";
      container.title = "Reload MistVideo and use the selected player";
      var MistVideo = this;

      var label = document.createElement("span");
      label.className = "mistvideo-force-label";
      label.textContent = MistVideo.translate("player")+":";
      container.appendChild(label);

      var menuItems = [{ value: "", label: MistVideo.translate("automatic") }];
      for (var i in mistplayers) {
        menuItems.push({ value: i, label: mistplayers[i].name });
      }

      var current = this.options.forcePlayer || "";
      var menuWrap = document.createElement("div");
      menuWrap.style.position = "relative";
      menuWrap.style.display = "inline-block";
      container.appendChild(menuWrap);

      var menuBtn = document.createElement("button");
      menuBtn.className = "mistvideo-menu-trigger";
      menuBtn.textContent = MistVideo.translate("automatic");
      for (var m = 0; m < menuItems.length; m++) {
        if (menuItems[m].value === current) { menuBtn.textContent = menuItems[m].label; break; }
      }
      menuWrap.appendChild(menuBtn);

      var menu = createMenu(MistVideo, {
        type: "forcePlayer",
        items: menuItems,
        selected: current,
        onChange: function(val) {
          for (var m = 0; m < menuItems.length; m++) {
            if (menuItems[m].value === val) { menuBtn.textContent = menuItems[m].label; break; }
          }
          MistVideo.options.forcePlayer = (val === "" ? false : val);
          if (MistVideo.options.forcePlayer != MistVideo.playerName) {
            MistVideo.reload("Reloading to force player.");
          }
        }
      });
      menuWrap.appendChild(menu);
      menuBtn.addEventListener("click", function(e){ e.stopPropagation(); menu.toggle(); });

      return container;
    },
    forceType: function(){
      if (!this.info) { return; }

      var container = document.createElement("div");
      container.className = "mistvideo-force-row";
      container.title = "Reload MistVideo and use the selected protocol";
      var MistVideo = this;

      var label = document.createElement("span");
      label.className = "mistvideo-force-label";
      label.textContent = MistVideo.translate("protocol")+":";
      container.appendChild(label);

      var menuItems = [{ value: "", label: MistVideo.translate("automatic") }];
      var sofar = {};
      for (var i in MistVideo.info.source) {
        var source = MistVideo.info.source[i];
        if (source.type in sofar) { continue; }
        sofar[source.type] = 1;
        menuItems.push({ value: source.type, label: MistUtil.format.mime2human(source.type) });
      }

      var current = this.options.forceType || "";
      var menuWrap = document.createElement("div");
      menuWrap.style.position = "relative";
      menuWrap.style.display = "inline-block";
      container.appendChild(menuWrap);

      var menuBtn = document.createElement("button");
      menuBtn.className = "mistvideo-menu-trigger";
      menuBtn.textContent = MistVideo.translate("automatic");
      for (var m = 0; m < menuItems.length; m++) {
        if (menuItems[m].value === current) { menuBtn.textContent = menuItems[m].label; break; }
      }
      menuWrap.appendChild(menuBtn);

      var menu = createMenu(MistVideo, {
        type: "forceType",
        items: menuItems,
        selected: current,
        onChange: function(val) {
          for (var m = 0; m < menuItems.length; m++) {
            if (menuItems[m].value === val) { menuBtn.textContent = menuItems[m].label; break; }
          }
          MistVideo.options.forceType = (val === "" ? false : val);
          if ((!MistVideo.source) || (MistVideo.options.forceType != MistVideo.source.type)) {
            MistVideo.reload("Reloading to force new type.");
          }
        }
      });
      menuWrap.appendChild(menu);
      menuBtn.addEventListener("click", function(e){ e.stopPropagation(); menu.toggle(); });

      return container;
    },
    forceLanguage: function(){
      var container = document.createElement("div");
      container.className = "mistvideo-force-row";
      container.title = "Reload MistVideo with the selected language";
      var MistVideo = this;

      var label = document.createElement("span");
      label.className = "mistvideo-force-label";
      label.textContent = MistVideo.translate("language")+":";
      container.appendChild(label);

      var langNames = { en: "English", nl: "Nederlands", de: "Deutsch", fr: "Fran\u00e7ais", es: "Espa\u00f1ol" };
      var menuItems = [{ value: "", label: MistVideo.translate("automatic") }];
      for (var code in locales) {
        menuItems.push({ value: code, label: langNames[code] || code });
      }

      var current = "";
      if (typeof MistVideo.options.translations === "object" && MistVideo.options.translations) {
        for (var code in locales) {
          if (MistVideo.options.translations === locales[code]) { current = code; break; }
        }
      }

      var menuWrap = document.createElement("div");
      menuWrap.style.position = "relative";
      menuWrap.style.display = "inline-block";
      container.appendChild(menuWrap);

      var menuBtn = document.createElement("button");
      menuBtn.className = "mistvideo-menu-trigger";
      menuBtn.textContent = MistVideo.translate("automatic");
      for (var m = 0; m < menuItems.length; m++) {
        if (menuItems[m].value === current) { menuBtn.textContent = menuItems[m].label; break; }
      }
      menuWrap.appendChild(menuBtn);

      var menu = createMenu(MistVideo, {
        type: "forceLanguage",
        items: menuItems,
        selected: current,
        onChange: function(val) {
          for (var m = 0; m < menuItems.length; m++) {
            if (menuItems[m].value === val) { menuBtn.textContent = menuItems[m].label; break; }
          }
          MistVideo.options.translations = (val === "" ? false : locales[val]);
          MistVideo.reload("Reloading to apply language.");
        }
      });
      menuWrap.appendChild(menu);
      menuBtn.addEventListener("click", function(e){ e.stopPropagation(); menu.toggle(); });

      return container;
    },
    forceTheme: function(){
      var container = document.createElement("div");
      container.className = "mistvideo-force-row";
      container.title = "Apply a preset theme";
      var MistVideo = this;
      var currentTheme = "default";
      var currentMode = "auto";
      var lastTokenKeys = null;

      var label = document.createElement("span");
      label.className = "mistvideo-force-label";
      label.textContent = MistVideo.translate("theme")+":";
      container.appendChild(label);

      function effectiveMode() {
        if (currentMode !== "auto") return currentMode;
        return window.matchMedia && window.matchMedia("(prefers-color-scheme: light)").matches ? "light" : "dark";
      }

      function apply() {
        if (!MistVideo.container) return;
        // Clear previous overrides
        if (lastTokenKeys) {
          for (var i = 0; i < lastTokenKeys.length; i++) {
            MistVideo.container.style.removeProperty(lastTokenKeys[i]);
          }
          lastTokenKeys = null;
        }
        var mode = effectiveMode();
        if (currentTheme === "default" && mode === "dark") return;
        var tokens = resolveTheme(currentTheme, mode);
        if (!tokens) return;
        lastTokenKeys = [];
        for (var key in tokens) {
          var prop = key.indexOf("--") === 0 ? key : "--mist-" + key;
          MistVideo.container.style.setProperty(prop, tokens[key]);
          lastTokenKeys.push(prop);
        }
      }

      // Theme palette dropdown
      var themeItems = [];
      for (var name in MistThemes) {
        themeItems.push({ value: name, label: name });
      }

      var themeWrap = document.createElement("div");
      themeWrap.style.position = "relative";
      themeWrap.style.display = "inline-block";
      container.appendChild(themeWrap);

      var themeBtn = document.createElement("button");
      themeBtn.className = "mistvideo-menu-trigger";
      themeBtn.textContent = "default";
      themeWrap.appendChild(themeBtn);

      var themeMenu = createMenu(MistVideo, {
        type: "forceTheme",
        items: themeItems,
        selected: "default",
        onChange: function(val) {
          currentTheme = val;
          themeBtn.textContent = val;
          apply();
        }
      });
      themeWrap.appendChild(themeMenu);
      themeBtn.addEventListener("click", function(e){ e.stopPropagation(); themeMenu.toggle(); });

      // Mode dropdown
      var modeItems = [
        { value: "auto", label: "auto" },
        { value: "dark", label: "dark" },
        { value: "light", label: "light" }
      ];

      var modeWrap = document.createElement("div");
      modeWrap.style.position = "relative";
      modeWrap.style.display = "inline-block";
      modeWrap.style.marginLeft = "var(--mist-space-xs)";
      container.appendChild(modeWrap);

      var modeBtn = document.createElement("button");
      modeBtn.className = "mistvideo-menu-trigger";
      modeBtn.textContent = "auto";
      modeWrap.appendChild(modeBtn);

      var modeMenu = createMenu(MistVideo, {
        type: "forceMode",
        items: modeItems,
        selected: "auto",
        onChange: function(val) {
          currentMode = val;
          modeBtn.textContent = val;
          apply();
        }
      });
      modeWrap.appendChild(modeMenu);
      modeBtn.addEventListener("click", function(e){ e.stopPropagation(); modeMenu.toggle(); });

      // React to OS preference changes when in auto mode
      if (window.matchMedia) {
        window.matchMedia("(prefers-color-scheme: light)").addEventListener("change", function() {
          if (currentMode === "auto") apply();
        });
      }

      return container;
    },
    forceSource: function(){
      var container = document.createElement("label");
      container.title = "Reload MistVideo and use the selected source";
      var MistVideo = this;
      
      var s = document.createElement("span");
      container.appendChild(s);
      s.appendChild(document.createTextNode("Force source: "));
      
      var select = document.createElement("select");
      container.appendChild(select);
      var option = document.createElement("option");
      select.appendChild(option);
      option.value = "";
      option.appendChild(document.createTextNode("Automatic"));
      for (var i in MistVideo.info.source) {
        var source = MistVideo.info.source[i];
        var option = document.createElement("option");
        select.appendChild(option);
        option.value = i;
        option.appendChild(document.createTextNode(source.url+" ("+MistUtil.format.mime2human(source.type)+")"));
      }
      
      if (this.options.forceSource) { select.value = this.options.forceSource; }
      
      MistUtil.event.addListener(select,"change",function(){
        MistVideo.options.forceSource = (this.value == "" ? false : this.value);
        if (MistVideo.options.forceSource != MistVideo.source.index) { //only reload if there is a change
          MistVideo.reload("Reloading to force new source.");
        }
      });
      
      return container;
    }
  }
};

//MistSkins.dev.css = MistUtil.object.extend(MistSkins["default"].css);
MistSkins.dev.css = {skin: "/skins/dev.css"};

//add close button blueprint
MistSkins.dev.blueprints.closeSubmenu = function() {
  var MistVideo = this;
  var btn = document.createElement("button");
  btn.innerHTML = "&times;";
  btn.addEventListener("click", function(e) {
    e.stopPropagation();
    MistVideo.container.removeAttribute("data-show-submenu");
    var menus = MistVideo.container.querySelectorAll(".mistvideo-menu[data-open]");
    for (var i = 0; i < menus.length; i++) {
      menus[i].style.display = "none";
      menus[i].removeAttribute("data-open");
    }
  });
  return btn;
};

//override submenu blueprint to reparent onto .mistvideo and strip interfering classes
MistSkins.dev.blueprints.submenu = function() {
  var container = this.UI.buildStructure(this.skin.structure.submenu);
  var MistVideo = this;
  setTimeout(function() {
    if (container && container.parentNode && MistVideo.container) {
      MistVideo.container.appendChild(container);
      MistUtil.class.remove(container, 'inner_window');
    }
  }, 0);
  return container;
};

//prepend dev tools to settings window
MistSkins.dev.structure.submenu = MistUtil.object.extend({},MistSkins["default"].structure.submenu,true);
MistSkins.dev.structure.submenu.type = "draggable";
delete MistSkins.dev.structure.submenu.style.maxWidth;
delete MistSkins.dev.structure.submenu.style.minWidth;
MistSkins.dev.structure.submenu.children.unshift({
  type: "container",
  style: { flexShrink: 1 },
  classes: ["mistvideo-column"],
  children: [
    {type: "closeSubmenu"},
    {
      if: function(){
        return (this.playerName && this.source)
      },
      then: {
        type: "container",
        classes: ["mistvideo-description","mistvideo-displayCombo"],
        style: { display: "block" },
        children: [
          {type: "playername", style: { display: "inline" }},
          {type: "text", i18n: "is playing", style: {margin: "0 0.2em"}},
          {type: "mimetype"}
        ]
      }
    },
    {type:"log"},
    {type:"decodingIssues"},
    {
      type: "container",
      classes: ["mistvideo-column","mistvideo-devcontrols"],
      style: {"font-size":"0.9em"},
      children: [
        {
          type: "text",
          i18n: "player control"
        },{
          type: "container",
          classes: ["mistvideo-devbuttons"],
          style: {"flex-wrap": "wrap"},
          children: [
            {
              type: "button",
              title: "Build MistVideo again",
              i18n: "reload",
              label: "Reload",
              onclick: function(){
                this.reload("Dev-reload button clicked.");
              }
            },{
              type: "button",
              title: "Switch to the next available player and source combination",
              i18n: "next combo",
              label: "Next combo",
              onclick: function(){
                this.nextCombo();
              }
            }
          ]
        },
        {type:"forcePlayer"},
        {type:"forceType"},
        {type:"forceLanguage"},
        {type:"forceTheme"}//,
        //{type:"forceSource"}
      ]
    }
  ]
});

