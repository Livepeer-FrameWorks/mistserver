import { MistUtil } from '../../core/util.js';

export var contextMenuBlueprints = {
  contextMenu: function(){
    var MistVideo = this;
    var container = document.createElement("div");
    container.className = "mistvideo-context-menu";
    container.setAttribute("role","menu");
    container.style.display = "none";

    var speeds = [0.25, 0.5, 0.75, 1, 1.25, 1.5, 2, 3, 5];
    var itemElements = [];

    function hide() {
      container.style.display = "none";
      itemElements = [];
    }

    function addItem(opts) {
      // opts: { label, check, shortcut, arrow, action, disabled, className }
      var el = document.createElement("div");
      el.className = "mistvideo-context-menu-item" + (opts.className ? " " + opts.className : "");
      el.setAttribute("role","menuitem");
      el.setAttribute("tabindex","0");
      if (opts.disabled) el.setAttribute("data-disabled","");

      if ("check" in opts) {
        var check = document.createElement("span");
        check.className = "mistvideo-context-menu-check";
        check.textContent = opts.check ? "\u2713" : "";
        el.appendChild(check);
      }

      var label = document.createElement("span");
      label.className = "mistvideo-context-menu-label";
      label.textContent = opts.label;
      el.appendChild(label);

      if (opts.shortcut) {
        var hint = document.createElement("span");
        hint.className = "mistvideo-context-menu-shortcut";
        hint.textContent = opts.shortcut;
        el.appendChild(hint);
      }

      if (opts.arrow) {
        var arrow = document.createElement("span");
        arrow.className = "mistvideo-context-menu-arrow";
        arrow.textContent = "\u25B8";
        el.appendChild(arrow);
      }

      if (opts.action && !opts.disabled) {
        el.addEventListener("click", function(e){
          e.stopPropagation();
          opts.action();
          if (!opts.keepOpen) hide();
        });
      }

      el.addEventListener("keydown", function(e){
        var idx = itemElements.indexOf(el);
        if (e.key === "ArrowDown" && idx < itemElements.length - 1) {
          itemElements[idx+1].focus();
          e.preventDefault();
        } else if (e.key === "ArrowUp" && idx > 0) {
          itemElements[idx-1].focus();
          e.preventDefault();
        } else if (e.key === "Enter" || e.key === " ") {
          el.click();
          e.preventDefault();
        } else if (e.key === "Escape") {
          hide();
          e.preventDefault();
        }
      });

      container.appendChild(el);
      itemElements.push(el);
      return el;
    }

    function addSeparator() {
      var sep = document.createElement("div");
      sep.className = "mistvideo-context-menu-separator";
      sep.setAttribute("role","separator");
      container.appendChild(sep);
    }

    function buildSpeedFlyout(parentItem) {
      var flyout = document.createElement("div");
      flyout.className = "mistvideo-context-menu-flyout";
      flyout.setAttribute("role","menu");

      var currentRate = MistVideo.api.playbackRate || 1;

      for (var i = 0; i < speeds.length; i++) {
        (function(speed){
          var item = document.createElement("div");
          item.className = "mistvideo-context-menu-item mistvideo-context-menu-flyout-item";
          item.setAttribute("role","menuitem");
          item.setAttribute("tabindex","0");

          var check = document.createElement("span");
          check.className = "mistvideo-context-menu-check";
          check.textContent = Math.abs(currentRate - speed) < 0.01 ? "\u2713" : "";
          item.appendChild(check);

          var label = document.createElement("span");
          label.className = "mistvideo-context-menu-label";
          label.textContent = speed === 1 ? MistVideo.translate("normal") : speed + "x";
          item.appendChild(label);

          item.addEventListener("click", function(e){
            e.stopPropagation();
            MistVideo.api.playbackRate = speed;
            hide();
          });

          flyout.appendChild(item);
        })(speeds[i]);
      }

      parentItem.appendChild(flyout);
      parentItem.addEventListener("mouseenter", function(){ flyout.style.display = ""; });
      parentItem.addEventListener("mouseleave", function(){ flyout.style.display = "none"; });
      flyout.style.display = "none";
    }

    function build() {
      MistUtil.empty(container);
      itemElements = [];

      var isLive = MistVideo.info && MistVideo.info.type === "live";
      var hasApi = MistVideo.player && MistVideo.player.api;
      var hadItems = false;

      // --- Playback group ---
      var playbackGroup = false;

      if (!isLive && hasApi && "loop" in MistVideo.player.api) {
        addItem({
          label: MistVideo.translate("loop"),
          check: MistVideo.api.loop,
          shortcut: "L",
          action: function(){
            MistVideo.api.loop = !MistVideo.api.loop;
          }
        });
        playbackGroup = true;
      }

      if (!isLive && hasApi && "playbackRate" in MistVideo.player.api) {
        var speedItem = addItem({
          label: MistVideo.translate("playback speed"),
          arrow: true,
          keepOpen: true,
          action: function(){} // flyout handles it
        });
        buildSpeedFlyout(speedItem);
        playbackGroup = true;
      }

      if (playbackGroup) hadItems = true;

      // --- Features group ---
      var featuresGroup = false;

      if (MistVideo.pip && MistVideo.pip.supported) {
        if (hadItems && !featuresGroup) addSeparator();
        addItem({
          label: MistVideo.translate("picture-in-picture"),
          shortcut: "I",
          action: function(){ MistVideo.pip.toggle(); }
        });
        featuresGroup = true;
      }

      var castBtn = MistVideo.container.querySelector("google-cast-launcher");
      if (castBtn) {
        if (hadItems && !featuresGroup) addSeparator();
        addItem({
          label: "Chromecast",
          action: function(){ castBtn.click(); }
        });
        featuresGroup = true;
      }

      var airplayBtn = MistVideo.container.querySelector(".mistvideo-airplay");
      if (airplayBtn) {
        if (hadItems && !featuresGroup) addSeparator();
        addItem({
          label: "AirPlay",
          action: function(){ airplayBtn.click(); }
        });
        featuresGroup = true;
      }

      if (featuresGroup) hadItems = true;

      // --- Actions group ---
      if (MistVideo.source && MistVideo.source.url) {
        if (hadItems) addSeparator();
        addItem({
          label: MistVideo.translate("copy video url"),
          action: function(){
            if (navigator.clipboard && navigator.clipboard.writeText) {
              navigator.clipboard.writeText(MistVideo.source.url);
            }
          }
        });
        hadItems = true;
      }

      // --- Branding ---
      if (hadItems) addSeparator();
      var brand = document.createElement("div");
      brand.className = "mistvideo-context-menu-brand";
      brand.textContent = "MistPlayer";
      brand.setAttribute("role","none");
      container.appendChild(brand);
    }

    function show(e) {
      e.preventDefault();
      e.stopPropagation();

      build();
      container.style.display = "";

      // Position relative to the player container
      var rect = MistVideo.container.getBoundingClientRect();
      var x = e.clientX - rect.left;
      var y = e.clientY - rect.top;

      // Clamp to stay inside container
      var menuW = container.offsetWidth;
      var menuH = container.offsetHeight;
      if (x + menuW > rect.width) x = rect.width - menuW;
      if (y + menuH > rect.height) y = rect.height - menuH;
      if (x < 0) x = 0;
      if (y < 0) y = 0;

      container.style.left = x + "px";
      container.style.top = y + "px";

      if (itemElements.length) itemElements[0].focus();
    }

    // Attach to video element
    MistUtil.event.addListener(MistVideo.options.target,"playerUpdate_videoElement",function(){
      if (MistVideo.video) {
        MistUtil.event.addListener(MistVideo.video,"contextmenu",show,container);
      }
    },container);
    // Also attach immediately if video already exists
    if (MistVideo.video) {
      MistUtil.event.addListener(MistVideo.video,"contextmenu",show,container);
    }

    // Close on click-away
    document.addEventListener("click",function(e){
      if (container.style.display !== "none" && !container.contains(e.target)) {
        hide();
      }
    });

    // Close on Escape
    document.addEventListener("keydown",function(e){
      if (e.key === "Escape" && container.style.display !== "none") {
        hide();
      }
    });

    return container;
  }
};
