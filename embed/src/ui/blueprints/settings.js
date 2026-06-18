import { MistUtil } from '../../core/util.js';
import { mistplayers } from '../../core/registry.js';
import { createMenu } from './menu.js';

export const settingsBlueprints = {
    playername: function(){
      if (!this.playerName || !(this.playerName in mistplayers)) { return; }
      
      var container = document.createElement("span");
      
      container.appendChild(document.createTextNode(mistplayers[this.playerName].name));
      
      return container;
    },
    mimetype: function(){
      if (!this.source) { return; }
      
      var a = document.createElement("a");
      a.href = this.source.url;
      a.target = "_blank";
      a.title = a.href+" ("+this.source.type+")";
      
      a.appendChild(document.createTextNode(MistUtil.format.mime2human(this.source.type)));
      
      return a;
    },
    logo: function(options){
      if ("element" in options) {
        return options.element;
      }
      if ("src" in options) {
        var img = document.createElement("img");
        img.src = options.src;
        return img;
      }
    },
    settings: function(){
      var MistVideo = this;

      var button = this.skin.icons.build("settings");
      button.setAttribute("role","button");
      button.setAttribute("tabindex","0");
      button.setAttribute("aria-label",MistVideo.translate("settings"));
      MistUtil.controlTooltip(button,MistVideo.translate("settings"));
      
      var touchmode = (typeof document.ontouchstart != "undefined");
      
      function closeSubmenu() {
        MistVideo.container.removeAttribute("data-show-submenu");
        // Close any open popup menus inside the submenu
        var openMenus = MistVideo.container.querySelectorAll(".mistvideo-menu[data-open]");
        for (var i = 0; i < openMenus.length; i++) {
          openMenus[i].style.display = "none";
          openMenus[i].removeAttribute("data-open");
        }
      }
      function openSubmenu() {
        MistVideo.container.setAttribute("data-show-submenu","");

        // Close on click outside
        function onDocClick(e) {
          if (!MistVideo.container.contains(e.target) || e.target === MistVideo.video) {
            closeSubmenu();
            document.removeEventListener("click", onDocClick, true);
          }
        }
        setTimeout(function(){ document.addEventListener("click", onDocClick, true); }, 0);
      }
      MistUtil.event.addListener(button,"click",function(e){
        e.stopPropagation();
        if (MistVideo.container.hasAttribute("data-show-submenu")) {
          closeSubmenu();
        }
        else {
          openSubmenu();
        }
      });
      return button;
    },
    loop: function(){
      if ((!this.api || !("loop" in this.api)) || (this.info.type == "live")) { return; }

      var MistVideo = this;
      var button = this.skin.icons.build("loop");
      button.setAttribute("role","button");
      button.setAttribute("tabindex","0");
      button.setAttribute("aria-label",MistVideo.translate("loop"));
      MistUtil.controlTooltip(button,MistVideo.translate("loop"));

      button.set = function(){
        if (MistVideo.api.loop) {
          MistUtil.class.remove(this,"off");
        }
        else {
          MistUtil.class.add(this,"off");
        }
      };

      MistUtil.event.addListener(button,"click",function(e){
        MistVideo.api.loop = !MistVideo.api.loop;
        this.set();
      });
      button.set();
      
      return button;
    },
    fullscreen: function(){
      if ((!("setSize" in this.player)) || (!this.info.hasVideo) || (this.source.type.split("/")[1] == "audio")) { return; }
      if (!this.fullscreen.supported) { return; }

      var MistVideo = this;
      var button = this.skin.icons.build("fullscreen");
      button.setAttribute("role","button");
      button.setAttribute("tabindex","0");
      button.setAttribute("aria-label",MistVideo.translate("fullscreen"));
      var fsTip = MistUtil.controlTooltip(button,MistVideo.translate("fullscreen")+" (f)");

      MistUtil.event.addListener(button, "click", function() { MistVideo.fullscreen.toggle(); });
      MistUtil.event.addListener(MistVideo.video, "dblclick", function() { MistVideo.fullscreen.toggle(); });

      var fsEvents = ['fullscreenchange','webkitfullscreenchange','mozfullscreenchange','MSFullscreenChange','fakefullscreenchange'];
      for (var i = 0; i < fsEvents.length; i++) {
        MistUtil.event.addListener(document, fsEvents[i], function() {
          if (MistVideo.fullscreen.active) {
            MistVideo.container.setAttribute("data-fullscreen","");
            button.setAttribute("aria-label",MistVideo.translate("exit fullscreen"));
            fsTip.textContent = MistVideo.translate("exit fullscreen")+" (f)";
            // Orientation lock on mobile
            if (screen.orientation && screen.orientation.lock) {
              try { screen.orientation.lock("landscape").catch(function(){}); } catch(e){}
            }
          } else if (MistVideo.container.hasAttribute("data-fullscreen")) {
            MistVideo.container.removeAttribute("data-fullscreen");
            button.setAttribute("aria-label",MistVideo.translate("fullscreen"));
            fsTip.textContent = MistVideo.translate("fullscreen")+" (f)";
            // Release orientation lock
            if (screen.orientation && screen.orientation.unlock) {
              try { screen.orientation.unlock(); } catch(e){}
            }
          }
          MistVideo.player.resizeAll();
        }, button);
      }

      return button;
    },
    "picture-in-picture": function(){
      if ((!("setSize" in this.player)) || (!this.info.hasVideo) || (this.source.type.split("/")[1] == "audio")) { return; }
      if (!this.pip.supported) { return; }

      var MistVideo = this;
      var button = this.skin.icons.build("pip");
      button.setAttribute("role","button");
      button.setAttribute("tabindex","0");
      button.setAttribute("aria-label",MistVideo.translate("pip"));
      MistUtil.controlTooltip(button,MistVideo.translate("pip")+" (i)");

      button.set = function(){
        if (MistVideo.pip.active) {
          MistUtil.class.remove(this, "off");
        } else {
          MistUtil.class.add(this, "off");
        }
      };

      MistUtil.event.addListener(button, "click", function(){
        MistVideo.pip.toggle().then(function(){ button.set(); });
      });
      button.set();

      return button;
    },
    tracks: function(){
      
      if ((!this.info) || (!this.video)) { return; }
      
      var MistVideo = this;
      var table = document.createElement("table");
      
      function build(tracks) {
        
        //empty table
        MistUtil.empty(table);
        
        tracks = MistUtil.tracks.parse(tracks);
        
        var selections = {};
        var checkboxes = {};
        
        function changeToTracks(type,value){
          if (value) { MistVideo.log("User selected "+type+" track with id "+value); }
          else {
            MistVideo.log("User selected automatic track selection for "+type);
            MistUtil.event.send("trackSetToAuto",type,MistVideo.video);
          }
          
          if (!MistVideo.options.setTracks) { MistVideo.options.setTracks = {}; }
          MistVideo.options.setTracks[type] = value;
          if ((value === true) && selections[type]) {
            MistUtil.event.send("change",null,selections[type]);
          }
          
          if ("setTrack" in MistVideo.player.api) {
            return MistVideo.player.api.setTrack(type,value);
          }
          else {
            //gather what tracks we should use
            var usetracks = {};
            for (var i in selections) {
              if ((i == "subtitle") || (selections[i].value == "")) { continue; } //subtitle tracks are handled separately
              usetracks[i] = selections[i].value;
            }
            if (value != ""){ usetracks[type] = value; }
            //use setTracks
            if ("setTracks" in MistVideo.player.api) {
              return MistVideo.player.api.setTracks(usetracks);
            }
            //use setSource
            if ("setSource" in MistVideo.player.api) {
              return MistVideo.player.api.setSource(
                MistUtil.http.url.addParam(MistVideo.source.url,usetracks)
              );
            }
          }
        }
        
        //sort the tracks to ["audio","video",..,"subtitle",..etc]
        var tracktypes = MistUtil.object.keys(tracks,function(keya,keyb){
          function order(value) {
            switch (value) {
              case "audio": return "aaaaaaa";
              case "video": return "aaaaaab";
              default:      return value;
            }
          }
          if (order(keya) > order(keyb)) { return 1; }
          if (order(keya) < order(keyb)) { return -1; }
          return 0;
        });
        function processTrack(t,type) {

          if (MistUtil.array.indexOf(["video","audio","subtitle"],type) <= -1) {
            //Do not display this track type
            return;
          }
          
          if (type == "subtitle") {
            if ((!("player" in MistVideo)) || (!("api" in MistVideo.player)) || (!("setWSSubtitle" in MistVideo.player.api) && !("setSubtitle" in MistVideo.player.api))) {
              //this player does not support adding subtitles, don't show track selection in the interface
              MistVideo.log("Subtitle selection was disabled as this player does not support it.");
              return;
            }
            
            var mime = "html5/text/vtt"
            if ("setWSSubtitle" in MistVideo.player.api) {
              mime = "html5/text/javascript";
            }
            
            //check if the VTT output is available
            var subtitleSource = false;
            for (var i in MistVideo.info.source) {
              var source = MistVideo.info.source[i];
              //this is a subtitle source, and it's the same protocol (HTTP/HTTPS) as the video source
              if ((source.type == mime) && (MistUtil.http.url.split(source.url).protocol == MistUtil.http.url.split(MistVideo.source.url).protocol.replace(/^ws/,"http"))) {
                subtitleSource = source.url.replace(/.srt$/,".vtt");
                break;
              }
            }
            
            if (!subtitleSource) {
              //if we can't find a subtitle output, don't show track selection in the interface
              MistVideo.log("Subtitle selection was disabled as a source could not be found.");
              return;
            }
            
            //also add the option to disable subtitles
            t[""] = { trackid: "", different: { none: MistVideo.translate("none") } };
            
          }
          
          var tr = document.createElement("tr");
          tr.title = MistVideo.translate("the current track")+" "+type+" track";
          table.appendChild(tr);
          
          if ("decodingIssues" in MistVideo.skin.blueprints) { //this is dev mode
            var cell = document.createElement("td");
            tr.appendChild(cell);
            if (type != "subtitle") {
              var checkbox = document.createElement("input");
              checkbox.setAttribute("type","checkbox");
              checkbox.setAttribute("checked","");
              checkbox.setAttribute("title","Whether or not to play "+type);
              checkbox.trackType = type;
              cell.appendChild(checkbox);
              checkboxes[type] = checkbox;
              
              if (MistVideo.options.setTracks && (MistVideo.options.setTracks[type])) {
                if (MistVideo.options.setTracks[type] == "none") {
                  checkbox.checked = false;
                }
                else {
                  checkbox.checked = true;
                }
              }
              
              MistUtil.event.addListener(checkbox,"change",function(){
                //make sure at least one checkbox is checked
                var n = 0;
                for (var i in checkboxes) {
                  if (checkboxes[i].checked) {
                    n++;
                  }
                }
                if (n == 0) {
                  for (var i in checkboxes) {
                    if (i == this.trackType) { continue; }
                    if (!checkboxes[i].checked) {
                      checkboxes[i].checked = true;
                      changeToTracks(i,true);
                      break;
                    }
                  }
                }
                
                var value = "none";
                if (this.checked) {
                  if (this.trackType in selections) {
                    value = selections[this.trackType].value;
                  }
                  else {
                    value = "auto";
                  }
                }
                else {
                  value = "none";
                }
                changeToTracks(this.trackType,(this.checked ? value : "none"));
              });
              
              MistUtil.event.addListener(MistVideo.video,"playerUpdate_trackChanged",function(e){
                
                if (e.message.type != type) { return; }
                
                if (e.message.value == "none") { this.checked = false; }
                else { this.checked = true; }
                
              },select);
            }
          }
          
          var header = document.createElement("td");
          tr.appendChild(header);
          header.appendChild(document.createTextNode(MistUtil.format.ucFirst(type)+":"));
          var cell = document.createElement("td");
          tr.appendChild(cell);
          var trackkeys = MistUtil.object.keys(t);
          
          //var determine the display info for the tracks
          function orderValues(trackinfoobj) {
            var order = {
              trackid:  0,
              language: 1,
              width:    2,
              bps:      3,
              fpks:     4,
              channels: 5,
              codec:    6,
              rate:     7
            };
            return MistUtil.object.values(trackinfoobj,function(keya,keyb,valuea,valueb){
              if (order[keya] > order[keyb]) { return 1; }
              if (order[keya] < order[keyb]) { return -1; }
              return 0;
            });
          }
          
          //there is more than one track of this type, and the player supports switching tracks
          if ((trackkeys.length > 1) && ("player" in MistVideo) && ("api" in MistVideo.player) && (("setTrack" in MistVideo.player.api) || ("setTracks" in MistVideo.player.api) || ("setSource" in MistVideo.player.api))) {
            // Hidden select for event compatibility
            var select = document.createElement("select");
            select.setAttribute("data-type",type);
            select.style.display = "none";
            selections[type] = select;
            select.trackType = type;
            cell.appendChild(select);

            // Build menu items
            var menuItems = [];
            if (type != "subtitle") {
              var option = document.createElement("option");
              select.appendChild(option);
              option.value = "";
              option.appendChild(document.createTextNode(MistVideo.translate("automatic")));
              menuItems.push({ value: "", label: MistVideo.translate("automatic") });
            }

            //add options to the select
            function n(str) {
              if (str == "") { return -1; }
              return Number(str);
            }
            var sortedKeys = MistUtil.object.keys(t,function(a,b){
              return n(a) - n(b);
            });
            for (var i in sortedKeys) {
              var track = t[sortedKeys[i]];
              var option = document.createElement("option");
              select.appendChild(option);
              option.value = ("idx" in track ? track.idx : track.trackid);
              var labelText;
              if (MistUtil.object.keys(track.different).length) {
                labelText = orderValues(track.different).join(" ");
              } else {
                labelText = MistVideo.translate("track")+" "+(Number(i)+1);
              }
              option.appendChild(document.createTextNode(labelText));
              menuItems.push({ value: option.value, label: labelText });
            }

            // Custom menu button + popup
            var menuWrap = document.createElement("div");
            menuWrap.style.position = "relative";
            cell.appendChild(menuWrap);

            var menuBtn = document.createElement("button");
            menuBtn.className = "mistvideo-menu-trigger";
            menuBtn.setAttribute("role","button");
            menuBtn.setAttribute("aria-haspopup","listbox");
            // Show default track info alongside "Automatic"
            if (type != "subtitle" && menuItems.length > 1) {
              var defaultTrack = menuItems[MistVideo.info.type == "live" ? menuItems.length - 1 : 1];
              menuBtn.textContent = MistVideo.translate("automatic") + " (" + defaultTrack.label + ")";
            }
            else {
              menuBtn.textContent = menuItems.length ? menuItems[0].label : "";
            }
            menuWrap.appendChild(menuBtn);

            function autoLabel(trackDesc) {
              return MistVideo.translate("automatic") + (trackDesc ? " (" + trackDesc + ")" : "");
            }
            var menu = createMenu(MistVideo, {
              type: type,
              items: menuItems,
              onChange: function(val) {
                select.value = val;
                if (val === "" && menuItems.length > 1) {
                  var def = menuItems[MistVideo.info.type == "live" ? menuItems.length - 1 : 1];
                  menuBtn.textContent = autoLabel(def.label);
                }
                else {
                  menuBtn.textContent = "";
                  for (var m = 0; m < menuItems.length; m++) {
                    if (menuItems[m].value === val) {
                      menuBtn.textContent = menuItems[m].label;
                      break;
                    }
                  }
                }
                MistUtil.event.send("change",val,select);
              }
            });
            menuWrap.appendChild(menu);

            menuBtn.addEventListener("click",function(e){
              e.stopPropagation();
              menu.toggle();
            });
            
            MistUtil.event.addListener(MistVideo.video,"playerUpdate_trackChanged",function(e){

              if ((e.message.type != type) || (e.message.trackid == "none")) { return; }

              var userChoseAuto = !MistVideo.options.setTracks || !MistVideo.options.setTracks[type];
              if (userChoseAuto) {
                var trackDesc = "";
                var tid = String(e.message.trackid);
                for (var m = 0; m < menuItems.length; m++) {
                  if (menuItems[m].value === tid) {
                    trackDesc = menuItems[m].label;
                    break;
                  }
                }
                menuBtn.textContent = autoLabel(trackDesc);
              }
              else {
                select.value = e.message.trackid;
                menu.setValue(e.message.trackid);
                for (var m = 0; m < menuItems.length; m++) {
                  if (menuItems[m].value === String(e.message.trackid)) {
                    menuBtn.textContent = menuItems[m].label;
                    break;
                  }
                }
              }
              MistVideo.log("Player selected "+type+" track with id "+e.message.trackid);

            },select);
            
            if (type == "subtitle") {
              MistUtil.event.addListener(select,"change",function(){
                try {
                  localStorage["mistSubtitleLanguage"] = t[this.value].lang;
                }
                catch (e) {}

                if ("setWSSubtitle" in MistVideo.player.api) {
                  MistVideo.player.api.setWSSubtitle(this.value == "" ? undefined : this.value);
                }
                else {
                  if (this.value != "") {
                    //gather metadata for this subtitle track here
                    var trackinfo = MistUtil.object.extend({},t[this.value]);
                    trackinfo.label = orderValues(trackinfo.describe).join(" ");
                    
                    trackinfo.src = MistUtil.http.url.addParam(subtitleSource,{track:this.value});
                    MistVideo.player.api.setSubtitle(trackinfo);
                  }
                  else {
                    MistVideo.player.api.setSubtitle();
                  }
                }
              });
              
              //load last used language if available
              if (("localStorage" in window) && (localStorage != null) && ("mistSubtitleLanguage" in localStorage)) {
                for (var i in t) {
                  if (t[i].lang == localStorage["mistSubtitleLanguage"]) {
                    select.value = i;
                    
                    //trigger onchange
                    var e = document.createEvent("Event");
                    e.initEvent("change");
                    select.dispatchEvent(e);
                    break;
                  }
                }
              }
            }
            else {
              MistUtil.event.addListener(select,"change",function(){
                if (this.trackType in checkboxes) { //this is dev mode
                  checkboxes[this.trackType].checked = true;
                }
                
                if (!changeToTracks(this.trackType,this.value)) {
                  //trackchange failed, reset select to old value
                }
                
              });
              
              //set to the track that plays by default
              /*
              if (MistVideo.info.type == "live") {
                //for live, the default track is the highest index
                select.value = MistUtil.object.keys(t).pop();
              }
              else {
                //for vod, the default track is the lowest index
                select.value = MistUtil.object.keys(t).shift();
              }
              */
              
            }
          }
          else {
            //show as text
            var span = document.createElement("span");
            span.className = "mistvideo-description";
            cell.appendChild(span);
            span.appendChild(document.createTextNode(orderValues(t[trackkeys[0]].same).join(" ")));
          }
        }

        for (var j in tracktypes) {
          var type = tracktypes[j];
          var t = tracks[type];
          processTrack(t,type);
        }
        
      }
      build(this.info.meta.tracks);
      MistUtil.event.addListener(MistVideo.video,"metaUpdate_tracks",function(e){
        
        //reconstruct track selection interface
        build(e.message.meta.tracks);
        
      },table);
      
      return table;
    },
};
