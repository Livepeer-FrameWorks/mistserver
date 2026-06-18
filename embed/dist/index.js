// src/core/util.js
var MistUtil = {
  format: {
    time: function(secs, options) {
      if (isNaN(secs) || !isFinite(secs)) {
        return secs;
      }
      if (!options) {
        options = {};
      }
      var ago = secs < 0 ? " ago" : "";
      secs = Math.abs(secs);
      var days = Math.floor(secs / 86400);
      secs = secs - days * 86400;
      var hours = Math.floor(secs / 3600);
      secs = secs - hours * 3600;
      var mins = Math.floor(secs / 60);
      var ms = Math.round(secs % 1 * 1e3);
      secs = Math.floor(secs - mins * 60);
      var str = [];
      if (days) {
        days = days + " day" + (days > 1 ? "s" : "") + ", ";
      }
      if (hours || days) {
        str.push(hours);
        str.push(("0" + mins).slice(-2));
      } else {
        str.push(mins);
      }
      str.push(("0" + Math.floor(secs)).slice(-2));
      if (options.ms) {
        str[str.length - 1] += "." + ("000" + ms).slice(-3);
      }
      return (days ? days : "") + str.join(":") + ago;
    },
    ago: function(date, range, translate) {
      if (isNaN(date.getTime())) {
        return "";
      }
      var ago = range ? range : (/* @__PURE__ */ new Date()).getTime() - date.getTime();
      var out = "";
      var negative = ago < 0;
      if (negative) {
        ago *= -1;
      }
      if (ago < 1e3) {
        out = translate ? translate("live") : "live";
      } else if (ago < 6e4) {
        var n = Math.round(ago / 1e3);
        if (translate) {
          out = translate(negative ? "in n sec" : "n sec ago").replace("{n}", n);
        } else {
          out = n + " sec";
          if (negative) {
            out = "in " + out;
          } else {
            out += " ago";
          }
        }
      } else if (!range && (/* @__PURE__ */ new Date()).toLocaleDateString() == date.toLocaleDateString() || range < 864e5) {
        out = date.toLocaleTimeString(void 0, {
          hour: "numeric",
          minute: "2-digit",
          second: "2-digit"
        });
      } else if (ago < 5184e5) {
        out = date.toLocaleString(void 0, {
          weekday: "short",
          hour: "numeric",
          minute: "2-digit",
          second: "2-digit"
        });
      } else if (!range && (/* @__PURE__ */ new Date()).getFullYear() == date.getFullYear() || range < 316224e5) {
        out = date.toLocaleString(void 0, {
          month: "short",
          day: "numeric",
          weekday: "short",
          hour: "numeric",
          minute: "2-digit",
          second: "2-digit"
        });
      } else {
        out = date.toLocaleString(void 0, {
          year: "numeric",
          month: "short",
          day: "numeric",
          hour: "numeric",
          minute: "2-digit",
          second: "2-digit"
        });
      }
      return out;
    },
    ucFirst: function(string) {
      return string.charAt(0).toUpperCase() + string.slice(1);
    },
    number: function(num) {
      if (isNaN(Number(num)) || Number(num) == 0) {
        return num;
      }
      var sig = Math.max(3, Math.ceil(Math.log(num) / Math.LN10));
      var mult = Math.pow(10, sig - Math.floor(Math.log(num) / Math.LN10) - 1);
      num = Math.round(num * mult) / mult;
      if (num >= 1e4) {
        var seperator = "\u2009";
        var parts = num.toString().split(".");
        var regex = /(\d+)(\d{3})/;
        while (regex.test(parts[0])) {
          parts[0] = parts[0].replace(regex, "$1" + seperator + "$2");
        }
        num = parts.join(".");
      }
      return num;
    },
    bytes: function(val, bits) {
      if (isNaN(Number(val))) {
        return val;
      }
      var suffix = bits ? ["bits", "Kb", "Mb", "Gb", "Tb", "Pb"] : ["bytes", "KB", "MB", "GB", "TB", "PB"];
      var unit;
      if (val == 0) {
        unit = suffix[0];
      } else {
        var exponent = Math.floor(Math.log(Math.abs(val)) / Math.log(1024));
        if (exponent < 0) {
          unit = suffix[0];
        } else {
          val = val / Math.pow(1024, exponent);
          unit = suffix[exponent];
        }
      }
      return this.number(val) + unit;
    },
    bits: function(val) {
      return this.bytes(val, true);
    },
    mime2human: function(mime) {
      switch (mime) {
        case "html5/video/webm": {
          return "WebM";
          break;
        }
        case "html5/application/vnd.apple.mpegurl": {
          return "HLS (TS)";
          break;
        }
        case "html5/application/vnd.apple.mpegurl;version=7": {
          return "HLS (CMAF)";
          break;
        }
        case "flash/10": {
          return "Flash (RTMP)";
          break;
        }
        case "flash/11": {
          return "Flash (HDS)";
          break;
        }
        case "flash/7": {
          return "Flash (Progressive)";
          break;
        }
        case "html5/video/mpeg": {
          return "TS";
          break;
        }
        case "html5/application/vnd.ms-sstr+xml":
        case "html5/application/vnd.ms-ss": {
          return "Smooth Streaming";
          break;
        }
        case "dash/video/mp4": {
          return "DASH";
          break;
        }
        case "webrtc": {
          return "WebRTC (WS)";
          break;
        }
        case "whep": {
          return "WebRTC (WHEP)";
          break;
        }
        case "silverlight": {
          return "Smooth streaming (Silverlight)";
          break;
        }
        case "html5/text/vtt": {
          return "VTT subtitles";
          break;
        }
        case "html5/text/plain": {
          return "SRT subtitles";
          break;
        }
        default: {
          return mime.replace("html5/", "").replace("video/", "").replace("audio/", "").toLocaleUpperCase();
        }
      }
    },
    offer2human: function(str) {
      var arr = str.split("\r\n");
      var out = [];
      var group = [];
      for (var i2 in arr) {
        var line = arr[i2];
        var stripped = line.slice(2);
        switch (line.slice(0, 2)) {
          case "m=": {
            out.push(group.join("\r\n"));
            group = [];
          }
        }
        group.push(line);
      }
      return out;
    }
  },
  class: {
    //reroute classList functionalities if not supported; also avoid indexOf
    add: function(DOMelement, item) {
      if ("classList" in DOMelement) {
        DOMelement.classList.add(item);
      } else {
        var classes = this.get(DOMelement);
        classes.push(item);
        this.set(DOMelement, classes);
      }
    },
    remove: function(DOMelement, item) {
      if ("classList" in DOMelement) {
        DOMelement.classList.remove(item);
      } else {
        var classes = this.get(DOMelement);
        for (var i2 = classes.length - 1; i2 >= 0; i2--) {
          if (classes[i2] == item) {
            classes.splice(i2);
          }
        }
        this.set(DOMelement, classes);
      }
    },
    get: function(DOMelement) {
      var classes;
      var className = DOMelement.getAttribute("class");
      if (!className || className == "") {
        classes = [];
      } else {
        classes = className.split(" ");
      }
      return classes;
    },
    set: function(DOMelement, classes) {
      DOMelement.setAttribute("class", classes.join(" "));
    },
    has: function(DOMelement, hasClass) {
      return DOMelement.className.split(" ").indexOf(hasClass) >= 0;
    }
  },
  object: {
    //extend object1 with object2
    extend: function(object1, object2, deep) {
      for (var i2 in object2) {
        if (deep && typeof object2[i2] == "object" && !("nodeType" in object2[i2])) {
          if (!(i2 in object1)) {
            if (MistUtil.array.is(object2[i2])) {
              object1[i2] = [];
            } else {
              object1[i2] = {};
            }
          }
          this.extend(object1[i2], object2[i2], true);
        } else {
          object1[i2] = object2[i2];
        }
      }
      return object1;
    },
    //replace Object.keys
    //if sorting: sort the keys alphabetically or use passed sorting function
    //sorting gets these arguments: keya,keyb,valuea,valueb
    keys: function(obj, sorting) {
      var keys = [];
      for (var i2 in obj) {
        keys.push(i2);
      }
      if (sorting) {
        if (typeof sorting != "function") {
          sorting = function(a, b) {
            return a.localeCompare(b);
          };
        }
        keys.sort(function(keya, keyb) {
          return sorting(keya, keyb, obj[keya], obj[keyb]);
        });
      }
      return keys;
    },
    //replace Object.values
    //if sorting: sort the keys alphabetically or use passed sorting function
    //sorting gets these arguments: keya,keyb,valuea,valueb
    values: function(obj, sorting) {
      var keys = this.keys(obj, sorting);
      var values = [];
      for (var i2 in keys) {
        values.push(obj[keys[i2]]);
      }
      return values;
    }
  },
  array: {
    //replace [].indexOf
    indexOf: function(array, entry) {
      if (!(array instanceof Array)) {
        throw "Tried to use indexOf on something that is not an array";
      }
      if ("indexOf" in array) {
        return array.indexOf(entry);
      }
      for (var i2; i2 < array.length; i2++) {
        if (array[i2] == entry) {
          return i2;
        }
      }
      return -1;
    },
    //replace isArray
    is: function(array) {
      if ("isArray" in Array) {
        return Array.isArray(array);
      }
      return Object.prototype.toString.call(array) === "[object Array]";
    },
    multiSort: function(array, sortby) {
      var sortfunc = function(a, b) {
        if (isNaN(a) || isNaN(b)) {
          return a.localeCompare(b);
        }
        return a > b ? 1 : a < b ? -1 : 0;
      };
      if (!sortby.length) {
        return array.sort(sortfunc);
      }
      function getValue(key, a) {
        function parseIt(item, key2, sortvalue) {
          if (!(key2 in item)) {
            throw "Invalid sorting rule: " + JSON.stringify([key2, sortvalue]) + '. "' + key2 + '" is not a key of ' + JSON.stringify(item);
          }
          if (typeof sortvalue == "number") {
            if (key2 in item) {
              return item[key2] * sortvalue;
            }
          }
          var i2 = sortvalue.indexOf(item[key2]);
          return i2 >= 0 ? i2 : sortvalue.length;
        }
        if (typeof key == "function") {
          return key(a);
        }
        if (typeof key == "object") {
          if (key instanceof Array) {
            return parseIt(a, key[0], key[1]);
          }
          for (var j in key) {
            return parseIt(a, j, key[j]);
          }
        }
        if (key in a) {
          return a[key];
        }
        throw "Invalid sorting rule: " + key + ". This should be a function, object or key of " + JSON.stringify(a) + ".";
      }
      array.sort(function(a, b) {
        var output = 0;
        for (var i2 in sortby) {
          var key = sortby[i2];
          output = sortfunc(getValue(key, a), getValue(key, b));
          if (output != 0) {
            break;
          }
        }
        return output;
      });
      return array;
    }
  },
  createUnique: function() {
    var i2 = "uid" + Math.random().toString().replace("0.", "");
    if (document.querySelector("." + i2)) {
      return createUnique();
    }
    return i2;
  },
  http: {
    getpost: function(type, url, data, callback, errorCallback) {
      var xhr = new XMLHttpRequest();
      xhr.open(type, url, true);
      if (type == "POST") {
        xhr.setRequestHeader("Content-type", "application/x-www-form-urlencoded");
      }
      if (errorCallback) {
        xhr.timeout = 8e3;
      }
      xhr.onload = function() {
        var status = xhr.status;
        if (status >= 200 && status < 300) {
          callback(xhr.response);
        } else if (errorCallback) {
          errorCallback(xhr);
        }
      };
      if (errorCallback) {
        xhr.onerror = function() {
          errorCallback(xhr);
        };
        xhr.ontimeout = xhr.onerror;
      }
      if (type == "POST") {
        var poststr;
        var post = [];
        for (var i2 in data) {
          post.push(i2 + "=" + encodeURIComponent(data[i2]));
        }
        if (post.length) {
          poststr = post.join("&");
        }
        xhr.send(poststr);
      } else {
        xhr.send();
      }
    },
    get: function(url, callback, errorCallback) {
      this.getpost("GET", url, null, callback, errorCallback);
    },
    post: function(url, data, callback, errorCallback) {
      this.getpost("POST", url, data, callback, errorCallback);
    },
    url: {
      addParam: function(url, params) {
        var spliturl = url.split("?");
        var ret = [spliturl.shift()];
        var splitparams = [];
        if (spliturl.length) {
          splitparams = spliturl[0].split("&");
        }
        for (var i2 in params) {
          splitparams.push(i2 + "=" + params[i2]);
        }
        if (splitparams.length) {
          ret.push(splitparams.join("&"));
        }
        return ret.join("?");
      },
      append: function(url, append) {
        var a = document.createElement("a");
        a.href = url;
        if (append[0] == "?") {
          if (a.search == "") {
            a.search = append;
          } else {
            a.search += "&" + append.slice(1);
          }
        } else if (append[0] == "&") {
          if (a.search == "") {
            a.search = "?" + append.slice(1);
          } else {
            a.search += append;
          }
        } else {
          a.href += append;
        }
        return a.href;
      },
      split: function(url) {
        var a = document.createElement("a");
        a.href = url;
        return {
          protocol: a.protocol,
          host: a.hostname,
          hash: a.hash,
          port: a.port,
          path: a.pathname.replace(/\/*$/, "")
        };
      },
      sanitizeHost: function(host) {
        var split = MistUtil.http.url.split(host);
        var out = split.protocol + "//" + split.host + (split.port && split.port != "" ? ":" + split.port : "") + (split.hash && split.hash != "" ? "#" + split.hash : "") + (split.path ? split.path.charAt(0) == "/" ? split.path : "/" + split.path : "");
        return out;
      }
    }
  },
  css: {
    cache: {},
    load: function(url, colors, callback) {
      var style = document.createElement("style");
      style.type = "text/css";
      style.setAttribute("data-source", url);
      if (callback) {
        style.callback = callback;
      }
      var cache = this.cache;
      function onCSSLoad(d) {
        var css = MistUtil.css.applyColors(d, colors);
        if ("callback" in style) {
          style.callback(css);
        } else {
          style.textContent = css;
        }
      }
      if (url in cache) {
        if (cache[url] instanceof Array) {
          cache[url].push(onCSSLoad);
        } else {
          onCSSLoad(cache[url]);
        }
      } else {
        let retry = function() {
          MistUtil.http.get(url, function(d) {
            for (var i2 in cache[url]) {
              cache[url][i2](d);
            }
            cache[url] = d;
          }, function() {
            if (attempts > 0) {
              attempts--;
              setTimeout(retry, 2e3);
            } else {
              var d = "/*Failed to load*/";
              for (var i2 in cache[url]) {
                cache[url][i2](d);
              }
              cache[url] = d;
            }
          });
        };
        cache[url] = [onCSSLoad];
        var attempts = 3;
        retry();
      }
      return style;
    },
    applyColors: function(css, colors) {
      return css.replace(/\$([^\s^;^}]*)/g, function(str, variable) {
        var index = variable.split(".");
        var val = colors;
        for (var j in index) {
          val = val[index[j]];
        }
        return val;
      });
    },
    generateCustomProperties: function(colors, tokens) {
      var map = {
        fill: "--mist-color-icon-fill",
        semiFill: "--mist-color-text-muted",
        stroke: "--mist-color-icon-stroke",
        strokeWidth: "--mist-icon-stroke-width",
        background: "--mist-color-surface",
        progressBackground: "--mist-color-progress-track",
        accent: "--mist-color-accent"
      };
      var props = {};
      if (colors) {
        for (var key in colors) {
          if (key in map) {
            props[map[key]] = colors[key];
          }
        }
      }
      if (tokens) {
        for (var key in tokens) {
          var name = key.indexOf("--") === 0 ? key : "--mist-" + key;
          props[name] = tokens[key];
        }
      }
      return props;
    },
    applyCustomProperties: function(element, colors, tokens) {
      var props = this.generateCustomProperties(colors, tokens);
      for (var name in props) {
        element.style.setProperty(name, props[name]);
      }
    },
    createStyle: function(css, prepend, applyToChildren) {
      var style = document.createElement("style");
      style.type = "text/css";
      if (css) {
        if (prepend) {
          css = this.prependClass(css, prepend, applyToChildren);
        }
        style.textContent = css;
      }
      return style;
    },
    prependClass: function(css, prepend, applyToChildren) {
      var style = false;
      if (typeof css != "string") {
        style = css;
        if (!("unprepended" in style)) {
          style.unprepended = style.textContent;
        }
        css = style.unprepended;
      }
      css = css.replace(/\/\*.*?\*\//g, "");
      var atDecls = [];
      css = css.replace(/@[^{;]*;/g, function(m2) {
        atDecls.push(m2);
        return "##DECL_" + (atDecls.length - 1) + "##";
      });
      var save = [];
      var m;
      while (m = css.match(/@[^}]*}/)) {
        css = css.replace(m[0], "@@#@@");
        var block = m[0];
        var replacecount = 1;
        while (replacecount < block.match(/{/g).length) {
          var match = css.match(/@@#@@([^}]*})/);
          if (!match) break;
          css = css.replace(match[0], "@@#@@");
          block += match[1];
          replacecount++;
        }
        css = css.replace("@@#@@", "##BLOCK_" + save.length + "##");
        save.push(block);
      }
      for (var i2 = 0; i2 < save.length; i2++) {
        css = css.replace("##BLOCK_" + i2 + "##", "@@BLOCK_" + i2 + "@@");
      }
      for (var i2 = 0; i2 < atDecls.length; i2++) {
        css = css.replace("##DECL_" + i2 + "##", "@@DECL_" + i2 + "@@");
      }
      css = css.replace(/[^@]*?{[^]*?}/g, function(match2) {
        var split = match2.split("{");
        var selectors = split[0].split(",");
        var properties = "{" + split.slice(1).join("}");
        for (var i3 in selectors) {
          selectors[i3] = selectors[i3].trim();
          var str = "." + prepend + selectors[i3];
          if (applyToChildren) {
            str += ",\n." + prepend + " " + selectors[i3];
          }
          selectors[i3] = str;
        }
        return "\n" + selectors + " " + properties;
      });
      for (var i2 = 0; i2 < save.length; i2++) {
        css = css.replace("@@BLOCK_" + i2 + "@@", save[i2]);
      }
      for (var i2 = 0; i2 < atDecls.length; i2++) {
        css = css.replace("@@DECL_" + i2 + "@@", atDecls[i2]);
      }
      if (style) {
        style.textContent = css;
        return;
      }
      return css;
    }
  },
  empty: function(DOMelement) {
    while (DOMelement.lastChild) {
      if (DOMelement.lastChild.lastChild) {
        this.empty(DOMelement.lastChild);
      }
      if ("attachedListeners" in DOMelement.lastChild) {
        for (var i2 in DOMelement.lastChild.attachedListeners) {
          MistUtil.event.removeListener(DOMelement.lastChild.attachedListeners[i2]);
        }
      }
      DOMelement.removeChild(DOMelement.lastChild);
    }
  },
  event: {
    send: function(type, message, target) {
      try {
        var event = new Event(type, {
          bubbles: true,
          cancelable: true
        });
        event.message = message;
        target.dispatchEvent(event);
        return event;
      } catch (e) {
        try {
          var event = document.createEvent("Event");
          event.initEvent(type, true, true);
          event.message = message;
          target.dispatchEvent(event);
          return event;
        } catch (e2) {
          return false;
        }
      }
      return true;
    },
    addListener: function(DOMelement, type, callback, storeOnElement) {
      DOMelement.addEventListener(type, callback);
      if (!storeOnElement) {
        storeOnElement = DOMelement;
      }
      if (!("attachedListeners" in storeOnElement)) {
        storeOnElement.attachedListeners = [];
      }
      var output = {
        element: DOMelement,
        type,
        callback
      };
      storeOnElement.attachedListeners.push(output);
      return output;
    },
    removeListener: function(data) {
      data.element.removeEventListener(data.type, data.callback);
    }
  },
  scripts: {
    list: {},
    insert: function(src, onevent, MistVideo2) {
      var scripts = this;
      if (MistVideo2) {
        MistVideo2.errorListeners.push({
          src,
          onevent
        });
      }
      if (src in this.list) {
        this.list[src].subscribers.push(onevent.onerror);
        if ("onload" in onevent) {
          if (this.list[src].tag.hasLoaded) {
            onevent.onload();
          } else {
            MistUtil.event.addListener(this.list[src].tag, "load", onevent.onload);
          }
        }
        return;
      }
      var scripttag = document.createElement("script");
      scripttag.hasLoaded = false;
      scripttag.setAttribute("src", src);
      scripttag.setAttribute("crossorigin", "anonymous");
      document.head.appendChild(scripttag);
      scripttag.onerror = function(e) {
        onevent.onerror(e);
      };
      scripttag.onload = function(e) {
        this.hasLoaded = true;
        if (!MistVideo2.destroyed) {
          onevent.onload(e);
        }
      };
      scripttag.addEventListener("error", function(e) {
        onevent.onerror(e);
      });
      var oldonerror = false;
      if (window.onerror) {
        oldonerror = window.onerror;
      }
      window.onerror = function(message, source, line, column, error) {
        if (oldonerror) {
          oldonerror.apply(this, arguments);
        }
        if (source == src) {
          onevent.onerror(error);
          for (var i2 in scripts.list[src].subscribers) {
            scripts.list[src].subscribers[i2](error);
          }
        }
      };
      this.list[src] = {
        subscribers: [onevent.onerror],
        tag: scripttag
      };
      return scripttag;
    }
  },
  tracks: {
    parse: function(metaTracks) {
      var output = {};
      for (var i2 in metaTracks) {
        var track = MistUtil.object.extend({}, metaTracks[i2]);
        if (track.type == "meta") {
          track.type = track.codec;
          track.codec = "meta";
        }
        if (!(track.type in output)) {
          output[track.type] = {};
        }
        output[track.type]["idx" in track ? track.idx : track.trackid] = track;
        var name = {};
        for (var j in track) {
          switch (j) {
            case "width":
              name[j] = track.width + "\xD7" + track.height;
              break;
            case "bps":
              if (track.codec == "meta") {
                continue;
              }
              if (track.bps > 0) {
                var val;
                if (track.bps > 1024 * 1024 / 8) {
                  val = Math.round(track.bps / 1024 / 1024 * 8) + "mbps";
                } else {
                  val = Math.round(track.bps / 1024 * 8) + "kbps";
                }
                name[j] = val;
              }
              break;
            case "fpks":
              if (track.fpks > 0) {
                name[j] = track.fpks / 1e3 + "fps";
              }
              break;
            case "channels":
              if (track.channels > 0) {
                name[j] = track.channels == 1 ? "Mono" : track.channels == 2 ? "Stereo" : "Surround (" + track.channels + "ch)";
              }
              break;
            case "rate":
              name[j] = Math.round(track.rate * 1e-3) + "Khz";
              break;
            case "language":
              if (track[j] != "Undetermined") {
                name[j] = track[j];
              }
              break;
            case "codec":
              if (track.codec == "meta") {
                continue;
              }
              name[j] = track[j];
              break;
          }
        }
        track.describe = name;
      }
      for (var type in output) {
        var equal = false;
        for (var i2 in output[type]) {
          if (!equal) {
            equal = MistUtil.object.extend({}, output[type][i2].describe);
            continue;
          }
          if (MistUtil.object.keys(output[type]).length > 1) {
            for (var j in output[type][i2].describe) {
              if (equal[j] != output[type][i2].describe[j]) {
                delete equal[j];
              }
            }
          }
        }
        for (var i2 in output[type]) {
          var different = {};
          var same = {};
          for (var j in output[type][i2].describe) {
            if (!(j in equal)) {
              different[j] = output[type][i2].describe[j];
            } else {
              same[j] = output[type][i2].describe[j];
            }
          }
          output[type][i2].different = different;
          output[type][i2].same = same;
          var d = MistUtil.object.values(different);
          output[type][i2].displayName = d.length ? d.join(", ") : MistUtil.object.values(output[type][i2].describe).join(" ");
        }
        var names = {};
        for (var i2 in output[type]) {
          if (output[type][i2].displayName in names) {
            var n = 1;
            for (var i2 in output[type]) {
              output[type][i2].different.trackid = n + ")";
              output[type][i2].displayName = "Track " + n + " (" + output[type][i2].displayName + ")";
              n++;
            }
            break;
          }
          names[output[type][i2].displayName] = 1;
        }
      }
      return output;
    },
    translateCodec: function(track) {
      function bin2hex(index) {
        return ("0" + track.init.charCodeAt(index).toString(16)).slice(-2);
      }
      switch (track.codec) {
        case "AAC":
          return "mp4a.40.2";
        case "MP3":
          return "mp3";
        //return "mp4a.40.34";
        case "AC3":
          return "ec-3";
        case "H264":
          return "avc1." + bin2hex(1) + bin2hex(2) + bin2hex(3);
        case "HEVC":
          return "hev1." + bin2hex(1) + bin2hex(6) + bin2hex(7) + bin2hex(8) + bin2hex(9) + bin2hex(10) + bin2hex(11) + bin2hex(12);
        default:
          return track.codec.toLowerCase();
      }
    }
  },
  isTouchDevice: function() {
    return "ontouchstart" in window || navigator.msMaxTouchPoints > 0;
  },
  getPos: function(element, cursorLocation) {
    var pos0 = element.getBoundingClientRect().left - (parseInt(element.borderLeftWidth, 10) || 0);
    var width = element.getBoundingClientRect().width;
    var perc = Math.max(0, (cursorLocation.clientX - pos0) / width);
    perc = Math.min(perc, 1);
    return perc;
  },
  createGraph: function(data, options) {
    if (!options) {
      options = {};
    }
    var ns = "http://www.w3.org/2000/svg";
    var svg = document.createElementNS(ns, "svg");
    svg.setAttributeNS(null, "height", "100%");
    svg.setAttributeNS(null, "width", "100%");
    svg.setAttributeNS(null, "class", "mist icon graph");
    svg.setAttributeNS(null, "preserveAspectRatio", "none");
    var x_correction = data.x[0];
    var lasty = data.y[0];
    if (options.differentiate) {
      for (var i2 = 1; i2 < data.y.length; i2++) {
        var diff = data.y[i2] - lasty;
        lasty = data.y[i2];
        data.y[i2] = diff;
      }
    }
    var path = [];
    var area = {
      x: {
        min: data.x[0] - x_correction,
        max: data.x[0] - x_correction
      },
      y: {
        min: data.y[0] * -1,
        max: data.y[0] * -1
      }
    };
    function updateMinMax(x, y) {
      if (arguments.length) {
        area.x.min = Math.min(area.x.min, x);
        area.x.max = Math.max(area.x.max, x);
        area.y.min = Math.min(area.y.min, y * -1);
        area.y.max = Math.max(area.y.max, y * -1);
      } else {
        var d = path[0].split(",");
        area = {
          x: {
            min: d[0],
            max: d[0]
          },
          y: {
            min: d[1],
            max: d[1]
          }
        };
        for (var i3 = 1; i3 < path.length; i3++) {
          var d = path[i3].split(",");
          updateMinMax(d[0], d[1] * -1);
        }
      }
    }
    path.push([data.x[0] - x_correction, data.y[0] * -1].join(","));
    for (var i2 = 1; i2 < data.y.length; i2++) {
      updateMinMax(data.x[i2] - x_correction, data.y[i2] * -1);
      path.push("L " + [data.x[i2] - x_correction, data.y[i2] * -1].join(","));
    }
    var defs = document.createElementNS(ns, "defs");
    svg.appendChild(defs);
    var gradient = document.createElementNS(ns, "linearGradient");
    defs.appendChild(gradient);
    gradient.setAttributeNS(null, "id", MistUtil.createUnique());
    gradient.setAttributeNS(null, "gradientUnits", "userSpaceOnUse");
    gradient.innerHTML += '<stop offset="0" stop-color="green"/>';
    gradient.innerHTML += '<stop offset="0.33" stop-color="yellow"/>';
    gradient.innerHTML += '<stop offset="0.66" stop-color="orange"/>';
    gradient.innerHTML += '<stop offset="1" stop-color="red"/>';
    function updateViewBox() {
      if ("x" in options) {
        if ("min" in options.x) {
          area.x.min = options.x.min;
        }
        if ("max" in options.x) {
          area.x.max = options.x.max;
        }
        if ("count" in options.x) {
          area.x.min = area.x.max - options.x.count;
        }
      }
      if ("y" in options) {
        if ("min" in options.y) {
          area.y.min = options.y.max * -1;
        }
        if ("max" in options.y) {
          area.y.max = options.y.min * -1;
        }
      }
      svg.setAttributeNS(null, "viewBox", [area.x.min, area.y.min, area.x.max - area.x.min, area.y.max - area.y.min].join(" "));
      gradient.setAttributeNS(null, "x1", 0);
      gradient.setAttributeNS(null, "x2", 0);
      if (options.reverseGradient) {
        gradient.setAttributeNS(null, "y1", area.y.max);
        gradient.setAttributeNS(null, "y2", area.y.min);
      } else {
        gradient.setAttributeNS(null, "y1", area.y.min);
        gradient.setAttributeNS(null, "y2", area.y.max);
      }
    }
    updateViewBox();
    var line = document.createElementNS(ns, "path");
    svg.appendChild(line);
    line.setAttributeNS(null, "stroke-width", "0.1");
    line.setAttributeNS(null, "fill", "none");
    line.setAttributeNS(null, "stroke", "url(#" + gradient.getAttribute("id") + ")");
    line.setAttributeNS(null, "d", "M" + path.join(" L"));
    line.addData = function(newData) {
      if (isNaN(newData.y)) {
        return;
      }
      if (options.differentiate) {
        var diff2 = newData.y - lasty;
        lasty = newData.y;
        newData.y = diff2;
      }
      path.push([newData.x - x_correction, newData.y * -1].join(","));
      if (options.x && options.x.count) {
        if (path.length > options.x.count) {
          path.shift();
          updateMinMax();
        }
      }
      updateMinMax(newData.x - x_correction, newData.y * -1);
      this.setAttributeNS(null, "d", "M" + path.join(" L"));
      updateViewBox();
    };
    svg.addData = function(newData) {
      line.addData(newData);
    };
    return svg;
  },
  getBrowser: function() {
    var ua = window.navigator.userAgent;
    if (ua.indexOf("MSIE ") >= 0 || ua.indexOf("Trident/") >= 0) {
      return "ie";
    }
    if (ua.indexOf("Edge/") >= 0) {
      return "edge";
    }
    if (ua.indexOf("Opera") >= 0 || ua.indexOf("OPR") >= 0) {
      return "opera";
    }
    if (ua.indexOf("Chrome") >= 0) {
      return "chrome";
    }
    if (ua.indexOf("Safari") >= 0) {
      return "safari";
    }
    if (ua.indexOf("Firefox") >= 0) {
      return "firefox";
    }
    return false;
  },
  getAndroid: function() {
    var match = navigator.userAgent.toLowerCase().match(/android\s([\d\.]*)/i);
    return match ? match[1] : false;
  },
  sources: {
    find: function(sources, matchObj) {
      outer:
        for (var i2 in sources) {
          for (var j in matchObj) {
            if (j == "protocol") {
              if (sources[i2].url.slice(0, matchObj.protocol.length) != matchObj.protocol) {
                continue outer;
              }
            } else {
              if (sources[i2][j] != matchObj[j]) {
                continue outer;
              }
            }
          }
          return sources[i2];
        }
      return false;
    }
  },
  controlTooltip: function(element, label) {
    var tip = document.createElement("div");
    tip.className = "mistvideo-control-tooltip";
    tip.textContent = label;
    tip.style.cssText = "position:absolute;bottom:100%;left:50%;transform:translateX(-50%);pointer-events:none;opacity:0;transition:opacity 0.15s ease;white-space:nowrap;z-index:10;padding:4px 8px;font-size:12px;";
    element.style.position = "relative";
    element.appendChild(tip);
    element.addEventListener("mouseenter", function() {
      tip.style.opacity = "1";
    });
    element.addEventListener("mouseleave", function() {
      tip.style.opacity = "0";
    });
    element.addEventListener("focus", function() {
      tip.style.opacity = "1";
    });
    element.addEventListener("blur", function() {
      tip.style.opacity = "0";
    });
    element._tooltip = tip;
    return tip;
  }
};

// src/core/registry.js
var mistplayers = {};
var nextPriority = 1;
function registerWrapper(name, definition) {
  definition.shortname = name;
  if (!("priority" in definition)) {
    definition.priority = nextPriority++;
  }
  mistplayers[name] = definition;
}

// src/ui/blueprints/container.js
var containerBlueprints = {
  container: function() {
    var container = document.createElement("div");
    return container;
  },
  video: function() {
    var MistVideo2 = this;
    if (MistVideo2.options.rotate) {
      switch (MistVideo2.options.rotate) {
        case -1:
        case 1:
        case 2: {
          MistVideo2.video.setAttribute("data-mist-rotate", MistVideo2.options.rotate);
          break;
        }
      }
    }
    MistVideo2.video.hideTimer = false;
    MistVideo2.video.hideCursor = function() {
      if (this.hideTimer) {
        clearTimeout(this.hideTimer);
      }
      this.hideTimer = MistVideo2.timers.start(function() {
        MistVideo2.container.setAttribute("data-hidecursor", "");
        var controlsContainer = MistVideo2.container.querySelector(".mistvideo-controls");
        if (controlsContainer) {
          controlsContainer.parentNode.setAttribute("data-hidecursor", "");
        }
      }, 3e3);
    };
    MistUtil.event.addListener(MistVideo2.video, "mousemove", function() {
      MistVideo2.container.removeAttribute("data-hidecursor");
      var controlsContainer = MistVideo2.container.querySelector(".mistvideo-controls");
      if (controlsContainer) {
        controlsContainer.parentNode.removeAttribute("data-hidecursor");
      }
      MistVideo2.video.hideCursor();
    });
    MistUtil.event.addListener(MistVideo2.video, "mouseout", function() {
      if (MistVideo2.video.hideTimer) {
        MistVideo2.timers.stop(MistVideo2.video.hideTimer);
      }
    });
    if (MistUtil.isTouchDevice() && MistVideo2.api) {
      var lastTap = 0;
      var lastSide = null;
      var tapTimer = null;
      MistUtil.event.addListener(MistVideo2.video, "touchend", function(e) {
        if (!MistVideo2.api || !isFinite(MistVideo2.api.duration)) {
          return;
        }
        if (MistVideo2.info && MistVideo2.info.type == "live") {
          return;
        }
        var now = Date.now();
        var rect = MistVideo2.video.getBoundingClientRect();
        var touch = e.changedTouches[0];
        var x = touch.clientX - rect.left;
        var side = x < rect.width * 0.4 ? "left" : x > rect.width * 0.6 ? "right" : null;
        if (side && side === lastSide && now - lastTap < 300) {
          e.preventDefault();
          if (tapTimer) {
            clearTimeout(tapTimer);
            tapTimer = null;
          }
          var delta = side === "right" ? 10 : -10;
          MistVideo2.api.currentTime = Math.max(0, Math.min(MistVideo2.api.duration, MistVideo2.api.currentTime + delta));
          var overlay = document.createElement("div");
          overlay.className = "mistvideo-doubletap-overlay";
          overlay.setAttribute("data-side", side);
          overlay.textContent = (delta > 0 ? "+" : "") + delta + "s";
          MistVideo2.container.appendChild(overlay);
          setTimeout(function() {
            if (overlay.parentNode) overlay.parentNode.removeChild(overlay);
          }, 600);
          lastTap = 0;
          lastSide = null;
        } else {
          lastTap = now;
          lastSide = side;
          tapTimer = setTimeout(function() {
            lastTap = 0;
            lastSide = null;
            tapTimer = null;
          }, 300);
        }
      });
    }
    if (MistVideo2.options.autoplay) {
      var canplay = MistUtil.event.addListener(MistVideo2.video, "canplay", function() {
        if (MistVideo2.api && MistVideo2.api.paused) {
          let autoplayFailed = function() {
            MistVideo2.log("Autoplay failed even with muted video. Unmuting and showing play button.");
            MistVideo2.timers.start(function() {
              if (MistVideo2.api.paused) {
                MistVideo2.api.pause();
                if (MistVideo2.monitor) {
                  MistVideo2.monitor.destroy();
                }
              }
            }, 5e3);
            if (MistVideo2.reporting) {
              MistVideo2.reporting.stats.d.autoplay = "failed";
            }
            MistVideo2.api.muted = false;
            var largePlayButton = MistVideo2.skin.icons.build("largeplay", 150);
            MistUtil.class.add(largePlayButton, "mistvideo-pointer");
            MistVideo2.container.appendChild(largePlayButton);
            MistUtil.event.addListener(largePlayButton, "click", function() {
              if (MistVideo2.api.paused) {
                MistVideo2.api.play();
              }
            });
            var f = function() {
              MistVideo2.container.removeChild(largePlayButton);
              MistVideo2.video.removeEventListener("play", f);
            };
            MistUtil.event.addListener(MistVideo2.video, "play", f);
          }, autoplayMuted = function() {
            MistVideo2.log("Autoplay worked! Video will be unmuted on mouseover if the page has been interacted with.");
            if (MistVideo2.reporting) {
              MistVideo2.reporting.stats.d.autoplay = "muted";
            }
            var largeMutedButton = MistVideo2.skin.icons.build("muted", 100);
            MistUtil.class.add(largeMutedButton, "mistvideo-pointer");
            MistVideo2.container.appendChild(largeMutedButton);
            MistUtil.event.addListener(largeMutedButton, "click", function() {
              MistVideo2.api.muted = false;
              MistVideo2.container.removeChild(largeMutedButton);
            });
            var interacted = false;
            var i2 = function() {
              interacted = true;
              document.body.removeEventListener("click", i2);
            };
            MistUtil.event.addListener(document.body, "click", i2, MistVideo2.video);
            var f = function() {
              if (interacted) {
                MistVideo2.api.muted = false;
                MistVideo2.video.removeEventListener("mouseenter", f);
                MistVideo2.log("Re-enabled sound");
              }
            };
            MistUtil.event.addListener(MistVideo2.video, "mouseenter", f);
            var fu = function() {
              if (!MistVideo2.api.muted) {
                if (largeMutedButton.parentNode) {
                  MistVideo2.container.removeChild(largeMutedButton);
                }
                MistVideo2.video.removeEventListener("volumechange", fu);
                document.body.removeEventListener("click", i2);
                MistVideo2.video.removeEventListener("mouseenter", f);
              }
            };
            MistUtil.event.addListener(MistVideo2.video, "volumechange", fu);
          };
          if (!MistVideo2.info.hasAudio) {
            MistVideo2.api.muted = true;
          }
          var wasMuted = MistVideo2.api.muted;
          if (MistUtil.getBrowser() == "safari") {
            MistVideo2.log("Muting before autoplay because this is safari");
            MistVideo2.api.muted = true;
          }
          var promise = MistVideo2.api.play();
          if (promise) {
            promise.then(function() {
              if (MistVideo2.api.muted != wasMuted) {
                autoplayMuted();
              } else {
                MistVideo2.log("Autoplay worked! muted:" + MistVideo2.api.muted);
              }
            }).catch(function(e) {
              if (MistVideo2.destroyed) {
                return;
              }
              if (MistVideo2.info.hasVideo && !MistVideo2.api.muted) {
                MistVideo2.log("Autoplay failed. Retrying with muted audio..");
                MistVideo2.api.muted = true;
                var promise2 = MistVideo2.api.play();
                if (promise2) {
                  promise2.then(function() {
                    if (MistVideo2.reporting) {
                      MistVideo2.reporting.stats.d.autoplay = "success";
                    }
                  }).then(function() {
                    if (MistVideo2.destroyed) {
                      return;
                    }
                    autoplayMuted();
                  }).catch(function() {
                    if (MistVideo2.destroyed) {
                      return;
                    }
                    autoplayFailed();
                  });
                }
              } else {
                autoplayFailed();
              }
            });
          }
        } else if (MistVideo2.reporting) {
          MistVideo2.reporting.stats.d.autoplay = "success";
        }
        MistUtil.event.removeListener(canplay);
      });
    }
    return this.video;
  },
  videocontainer: function() {
    return this.UI.buildStructure(this.skin.structure.videocontainer);
  },
  secondaryVideo: function(o) {
    if (!o) {
      o = {};
    }
    if (!o.options) {
      o.options = {};
    }
    var MistVideo2 = this;
    if (!("secondary" in MistVideo2)) {
      MistVideo2.secondary = [];
    }
    var options = MistUtil.object.extend({}, MistVideo2.options);
    options = MistUtil.object.extend(options, o.options);
    MistVideo2.secondary.push(options);
    var pointer = {
      primary: MistVideo2,
      secondary: false
    };
    options.target = document.createElement("div");
    delete options.container;
    var mvo = {};
    options.MistVideoObject = mvo;
    MistUtil.event.addListener(options.target, "initialized", function() {
      var mv = mvo.reference;
      options.MistVideo = mv;
      pointer.secondary = mv;
      mv.player.api.muted = true;
      mv.player.api.loop = false;
      var controlContainers = options.target.querySelectorAll(".mistvideo-controls");
      for (var i2 = 0; i2 < controlContainers.length; i2++) {
        MistUtil.event.addListener(controlContainers[i2], "click", function(e) {
          e.stopPropagation();
        });
      }
      MistUtil.event.addListener(MistVideo2.video, "play", function() {
        if (mv.player.api.paused) {
          mv.player.api.play();
        }
      }, options.target);
      MistUtil.event.addListener(MistVideo2.video, "pause", function(e) {
        if (!mv.player.api.paused) {
          mv.player.api.pause();
        }
      }, options.target);
      MistUtil.event.addListener(MistVideo2.video, "seeking", function() {
        mv.player.api.currentTime = this.currentTime;
      }, options.target);
      MistUtil.event.addListener(MistVideo2.video, "timeupdate", function() {
        if (mv.player.api.pausedesync) {
          return;
        }
        var desync = this.currentTime - mv.player.api.currentTime;
        var adesync = Math.abs(desync);
        if (adesync > 30) {
          mv.player.api.pausedesync = true;
          mv.player.api.currentTime = this.currentTime;
          mv.log("Re-syncing with main video by seeking (desync: " + desync + "s)");
        } else if (adesync > 0.01) {
          var rate = 0.1;
          if (adesync < 1) {
            rate = 0.05;
          }
          rate = 1 + rate * Math.sign(desync);
          if (rate != mv.player.api.playbackRate) {
            mv.log("Re-syncing by changing the playback rate (desync: " + Math.round(desync * 1e3) + "ms, rate: " + rate + ")");
          }
          mv.player.api.playbackRate = rate;
        } else if (mv.player.api.playbackRate != 1) {
          mv.player.api.playbackRate = 1;
          mv.log("Sync with main video achieved (desync: " + Math.round(desync * 1e3) + "ms)");
        }
      }, options.target);
      MistUtil.event.addListener(mv.video, "seeked", function() {
        mv.player.api.pausedesync = false;
      });
    });
    options.skin = MistUtil.object.extend({}, MistVideo2.skin, true);
    options.skin.structure.main = MistUtil.object.extend({}, MistVideo2.skin.structure.secondaryVideo(pointer));
    mistPlay(MistVideo2.stream, options);
    return options.target;
  },
  switchVideo: function(options) {
    var container = document.createElement("div");
    container.appendChild(this.skin.icons.build("switchvideo"));
    MistUtil.event.addListener(container, "click", function() {
      var primary = options.containers.primary;
      var secondary = options.containers.secondary;
      function findVideo(startAt, matchTarget) {
        if (startAt.video.currentTarget == matchTarget) {
          return startAt.video;
        }
        if (startAt.secondary) {
          for (var i2 = 0; i2 < startAt.secondary.length; i2++) {
            var result = findVideo(startAt.secondary[i2].MistVideo, matchTarget);
            if (result) {
              return result;
            }
          }
        }
        return false;
      }
      var pv = findVideo(primary, primary.options.target);
      var sv = findVideo(primary, secondary.options.target);
      var playit = !pv.paused;
      var place = document.createElement("div");
      sv.parentElement.insertBefore(place, sv);
      pv.parentElement.insertBefore(sv, pv);
      place.parentElement.insertBefore(pv, place);
      place.parentElement.removeChild(place);
      if (playit) {
        try {
          pv.play();
          sv.play();
        } catch (e) {
        }
      }
      var tmp = {
        width: pv.style.width,
        height: pv.style.height,
        currentTarget: pv.currentTarget
      };
      pv.currentTarget = sv.currentTarget;
      sv.currentTarget = tmp.currentTarget;
      primary.player.resizeAll();
    });
    return container;
  }
};

// src/ui/blueprints/controls-bar.js
var controlsBarBlueprints = {
  controls: function() {
    if (this.options.controls && this.options.controls != "stock") {
      MistUtil.class.add(this.container, "hasControls");
      var container = this.UI.buildStructure(this.skin.structure.controls);
      if (MistUtil.isTouchDevice() && this.size && this.size.width > 300) {
        container.style.zoom = 1.5;
      }
      return container;
    }
  },
  submenu: function() {
    return this.UI.buildStructure(this.skin.structure.submenu);
  },
  hoverWindow: function(options) {
    var structure = {
      type: "container",
      classes: "classes" in options ? options.classes : [],
      children: "children" in options ? options.children : []
    };
    structure.classes.push("hover_window_container");
    if (!("classes" in options.window)) {
      options.window.classes = [];
    }
    options.window.classes.push("inner_window");
    options.window.classes.push("mistvideo-container");
    options.window = {
      type: "container",
      classes: ["outer_window"],
      children: [options.window]
    };
    if (!("classes" in options.button)) {
      options.button.classes = [];
    }
    options.button.classes.push("pointer");
    switch (options.mode) {
      case "left":
        structure.classes.push("horizontal");
        structure.children = [options.window, options.button];
        break;
      case "right":
        structure.classes.push("horizontal");
        structure.children = [options.button, options.window];
        break;
      case "top":
        structure.classes.push("vertical");
        structure.children = [options.button, options.window];
        break;
      case "bottom":
        structure.classes.push("vertical");
        structure.children = [options.window, options.button];
        break;
      case "pos":
        structure.children = [options.button, options.window];
        if (!("classes" in options.window)) {
          options.window.classes = [];
        }
        break;
      default:
        throw "Unsupported mode for structure type hoverWindow";
        break;
    }
    if ("transition" in options) {
      if (!("css" in structure)) {
        structure.css = [];
      }
      structure.css.push(
        ".hover_window_container:hover > .outer_window:not([data-hidecursor]) > .inner_window { " + options.transition.show + " }\n.hover_window_container > .outer_window { " + options.transition.viewport + " }\n.hover_window_container > .outer_window > .inner_window { " + options.transition.hide + " }"
      );
    }
    structure.classes.push(options.mode);
    return this.UI.buildStructure(structure);
  },
  draggable: function(options) {
    var container = this.skin.blueprints.container(options);
    var MistVideo2 = this;
    var button = this.skin.icons.build("fullscreen", 16);
    MistUtil.class.remove(button, "fullscreen");
    MistUtil.class.add(button, "draggable-icon");
    container.appendChild(button);
    button.style.alignSelf = "flex-end";
    button.style.position = "absolute";
    button.style.cursor = "move";
    var offset = {};
    var move = function(e) {
      container.style.left = e.clientX - offset.x + "px";
      container.style.top = e.clientY - offset.y + "px";
    };
    var stop = function(e) {
      window.removeEventListener("mousemove", move);
      window.removeEventListener("click", stop);
      MistUtil.event.addListener(button, "click", start);
    };
    var start = function(e) {
      e.stopPropagation();
      button.removeEventListener("click", start);
      offset.x = MistVideo2.container.getBoundingClientRect().left - (container.getBoundingClientRect().left - e.clientX);
      offset.y = MistVideo2.container.getBoundingClientRect().top - (container.getBoundingClientRect().top - e.clientY);
      container.style.position = "absolute";
      container.style.right = "auto";
      container.style.bottom = "auto";
      MistVideo2.container.appendChild(container);
      move(e);
      MistUtil.event.addListener(window, "mousemove", move, container);
      MistUtil.event.addListener(window, "click", stop, container);
    };
    MistUtil.event.addListener(button, "click", start);
    return container;
  }
};

// src/ui/blueprints/thumbnail.js
var ThumbnailPreview = {
  parseVTT: function(text) {
    var cues = [];
    var blocks = text.replace(/\r\n/g, "\n").split("\n\n");
    for (var i2 = 0; i2 < blocks.length; i2++) {
      var lines = blocks[i2].trim().split("\n");
      for (var j = 0; j < lines.length; j++) {
        var match = lines[j].match(/^(\d{2}:\d{2}[:\.][\d.]+)\s+-->\s+(\d{2}:\d{2}[:\.][\d.]+)/);
        if (match) {
          var start = ThumbnailPreview.parseTime(match[1]);
          var end = ThumbnailPreview.parseTime(match[2]);
          var url = "";
          if (j + 1 < lines.length) {
            url = lines[j + 1].trim();
          }
          if (url) {
            cues.push({ start, end, url });
          }
          break;
        }
      }
    }
    return cues;
  },
  parseTime: function(str) {
    var parts = str.replace(",", ".").split(":");
    var secs = 0;
    if (parts.length === 3) {
      secs = parseFloat(parts[0]) * 3600 + parseFloat(parts[1]) * 60 + parseFloat(parts[2]);
    } else if (parts.length === 2) {
      secs = parseFloat(parts[0]) * 60 + parseFloat(parts[1]);
    }
    return secs;
  },
  findCue: function(cues, time) {
    for (var i2 = 0; i2 < cues.length; i2++) {
      if (time >= cues[i2].start && time < cues[i2].end) {
        return cues[i2];
      }
    }
    return null;
  },
  parseFragment: function(url) {
    var hash = url.indexOf("#");
    if (hash === -1) return { src: url, x: 0, y: 0, w: 0, h: 0, isSprite: false };
    var src = url.substring(0, hash);
    var frag = url.substring(hash + 1);
    var match = frag.match(/xywh=(\d+),(\d+),(\d+),(\d+)/);
    if (!match) return { src, x: 0, y: 0, w: 0, h: 0, isSprite: false };
    return {
      src,
      x: parseInt(match[1]),
      y: parseInt(match[2]),
      w: parseInt(match[3]),
      h: parseInt(match[4]),
      isSprite: true
    };
  },
  resolveUrl: function(url, baseUrl) {
    if (!baseUrl || /^https?:\/\//.test(url) || /^\/\//.test(url)) return url;
    var base = baseUrl.substring(0, baseUrl.lastIndexOf("/") + 1);
    return base + url;
  },
  createPreview: function(cue, baseUrl) {
    var info = ThumbnailPreview.parseFragment(cue.url);
    var src = ThumbnailPreview.resolveUrl(info.src, baseUrl);
    var container = document.createElement("div");
    container.className = "mistvideo-thumb-preview";
    if (info.isSprite) {
      container.style.width = info.w + "px";
      container.style.height = info.h + "px";
      container.style.backgroundImage = "url('" + src + "')";
      container.style.backgroundPosition = "-" + info.x + "px -" + info.y + "px";
      container.style.backgroundRepeat = "no-repeat";
    } else {
      var img = document.createElement("img");
      img.src = src;
      img.style.maxWidth = "160px";
      img.style.maxHeight = "90px";
      img.style.display = "block";
      container.appendChild(img);
    }
    return container;
  }
};

// src/ui/blueprints/playback.js
var LS_VOLUME_KEY = "mistVolume";
var playbackBlueprints = {
  progress: function() {
    var margincontainer = document.createElement("div");
    margincontainer.setAttribute("role", "slider");
    margincontainer.setAttribute("aria-label", this.translate("seek bar"));
    margincontainer.setAttribute("aria-valuemin", "0");
    margincontainer.setAttribute("aria-valuemax", "100");
    margincontainer.setAttribute("aria-valuenow", "0");
    margincontainer.setAttribute("tabindex", "0");
    var container = document.createElement("div");
    margincontainer.appendChild(container);
    container.kids = {};
    container.kids.bar = document.createElement("div");
    container.kids.bar.className = "bar";
    container.appendChild(container.kids.bar);
    var video = this.video;
    var MistVideo2 = this;
    var first = Infinity;
    if (MistVideo2.info && MistVideo2.info.meta && MistVideo2.info.meta.tracks) {
      for (var i2 in MistVideo2.info.meta.tracks) {
        if (MistVideo2.info.meta.tracks[i2].firstms * 1e-3 < first) {
          first = MistVideo2.info.meta.tracks[i2].firstms * 1e-3;
        }
      }
    }
    if (first == Infinity) {
      first = 0;
    }
    function firsts() {
      if (MistVideo2.api.duration < first) {
        return 0;
      }
      return first;
    }
    function getBufferWindow() {
      var buffer_window = MistVideo2.info.meta.buffer_window;
      if (typeof buffer_window == "undefined") {
        if (MistVideo2.api.buffered && MistVideo2.api.buffered.length) {
          buffer_window = (MistVideo2.api.duration - MistVideo2.api.buffered.start(0)) * 1e3;
        } else {
          buffer_window = 6e4;
        }
      }
      return buffer_window *= 1e-3;
    }
    container.updateBar = function(currentTime) {
      if (this.kids.bar) {
        if (!isFinite(MistVideo2.api.duration)) {
          this.kids.bar.style.display = "none";
          return;
        } else {
          this.kids.bar.style.display = "";
        }
        var w = Math.min(1, Math.max(0, this.time2perc(currentTime)));
        this.kids.bar.style.width = w * 100 + "%";
        margincontainer.setAttribute("aria-valuenow", Math.round(w * 100));
      }
    };
    container.time2perc = function(time) {
      if (!isFinite(MistVideo2.api.duration)) {
        return 0;
      }
      var result = 0;
      if (MistVideo2.info.type == "live") {
        var buffer_window = getBufferWindow();
        result = (time - MistVideo2.api.duration + buffer_window) / buffer_window;
      } else {
        result = (time - firsts()) / (MistVideo2.api.duration - firsts());
      }
      return Math.min(1, Math.max(0, result));
    };
    container.buildBuffer = function(start, end) {
      var buffer = document.createElement("div");
      buffer.className = "buffer";
      buffer.style.left = this.time2perc(start) * 100 + "%";
      buffer.style.width = (this.time2perc(end) - this.time2perc(start)) * 100 + "%";
      return buffer;
    };
    container.updateBuffers = function(buffers) {
      var old = this.querySelectorAll(".buffer");
      for (var i3 = 0; i3 < old.length; i3++) {
        this.removeChild(old[i3]);
      }
      if (buffers) {
        for (var i3 = 0; i3 < buffers.length; i3++) {
          this.appendChild(this.buildBuffer(
            buffers.start(i3),
            buffers.end(i3)
          ));
        }
      }
    };
    var lastBufferUpdate = 0;
    var bufferTimer = false;
    MistVideo2.playerState.on("buffered", function(buffers) {
      function updateBuffers() {
        if ((/* @__PURE__ */ new Date()).getTime() - lastBufferUpdate > 1e3) {
          container.updateBuffers(buffers);
          lastBufferUpdate = (/* @__PURE__ */ new Date()).getTime();
        } else if (!bufferTimer) {
          bufferTimer = MistVideo2.timers.start(function() {
            updateBuffers();
            bufferTimer = false;
          }, 1e3);
        }
      }
      if (buffers) updateBuffers();
    });
    var lastBarUpdate = 0;
    var barTimer = false;
    MistVideo2.playerState.on("currentTime", function(time) {
      function updateBar() {
        if ((/* @__PURE__ */ new Date()).getTime() - lastBarUpdate > 200 && !dragging) {
          container.updateBar(time);
          lastBarUpdate = (/* @__PURE__ */ new Date()).getTime();
        } else if (!barTimer) {
          barTimer = MistVideo2.timers.start(function() {
            updateBar();
            barTimer = false;
          }, 1e3);
        }
      }
      updateBar();
    });
    MistVideo2.playerState.on("seeking", function(isSeeking) {
      if (isSeeking) {
        container.updateBar(MistVideo2.api.currentTime);
      }
    });
    container.getPos = function(e) {
      var perc = isNaN(e) ? MistUtil.getPos(this, e) : e;
      if (MistVideo2.info.type == "live") {
        var bufferWindow = getBufferWindow();
        return (perc - 1) * bufferWindow + MistVideo2.api.duration;
      } else {
        if (!isFinite(MistVideo2.api.duration)) {
          return false;
        }
        return perc * (MistVideo2.api.duration - firsts()) + firsts();
      }
    };
    container.seek = function(e) {
      var pos = this.getPos(e);
      MistVideo2.api.currentTime = pos;
    };
    MistUtil.event.addListener(margincontainer, "mouseup", function(e) {
      if (e.which != 1) {
        return;
      }
      container.seek(e);
    });
    var tooltip = MistVideo2.UI.buildStructure({ type: "tooltip" });
    tooltip.style.opacity = 0;
    container.appendChild(tooltip);
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
    if (MistVideo2.options.thumbnails) {
      loadThumbnails(MistVideo2.options.thumbnails);
    }
    MistUtil.event.addListener(margincontainer, "mouseout", function() {
      if (!dragging) {
        tooltip.style.opacity = 0;
      }
    });
    container.moveTooltip = function(e) {
      var secs = this.getPos(e);
      if (secs === false) {
        tooltip.style.opacity = 0;
        return;
      }
      tooltip.setDisplay(secs);
      tooltip.style.opacity = 1;
      var perc = MistUtil.getPos(this, e);
      var pos = { bottom: 20 };
      if (perc > 0.5) {
        pos.right = (1 - perc) * 100 + "%";
        tooltip.triangle.setMode("bottom", "right");
      } else {
        pos.left = perc * 100 + "%";
        tooltip.triangle.setMode("bottom", "left");
      }
      tooltip.setPos(pos);
    };
    var realtime = document.createElement("span");
    realtime.setAttribute("class", "mistvideo-realtime");
    var realtimetext = document.createTextNode("");
    realtime.appendChild(realtimetext);
    tooltip.setDisplay = function(secs) {
      MistUtil.empty(thumbContainer);
      if (thumbCues && thumbCues.length) {
        var cue = ThumbnailPreview.findCue(thumbCues, secs);
        if (cue) {
          var preview = ThumbnailPreview.createPreview(cue, thumbBaseUrl);
          thumbContainer.appendChild(preview);
        }
      }
      if (MistVideo2.options.useDateTime && MistVideo2.info && MistVideo2.info.unixoffset) {
        var range = container.getPos(1) - container.getPos(0);
        var ago = (/* @__PURE__ */ new Date()).getTime() * 1e-3 - (MistVideo2.info.unixoffset * 1e-3 + container.getPos(1));
        var scale = Math.max(range, ago);
        var str = "";
        var t = MistVideo2.translate.bind(MistVideo2);
        if (MistVideo2.info.type == "live") {
          if (scale < 60) {
            str = MistUtil.format.ago(new Date(MistVideo2.info.unixoffset + secs * 1e3), null, t);
          } else {
            var secsago = (/* @__PURE__ */ new Date()).getTime() * 1e-3 - (MistVideo2.info.unixoffset * 1e-3 + secs);
            if (secsago < 48 * 3600) {
              str += " - " + MistUtil.format.time(secsago);
            }
          }
        } else {
          str += MistUtil.format.time(secs);
        }
        if (scale >= 60) {
          realtimetext.nodeValue = " " + MistVideo2.translate("at") + " " + MistUtil.format.ago(new Date(MistVideo2.info.unixoffset + secs * 1e3), scale * 1e3);
          var f = document.createDocumentFragment();
          f.appendChild(document.createTextNode(str));
          f.appendChild(realtime);
          tooltip.setHtml(f);
        } else {
          realtimetext.nodeValue = "";
          tooltip.setText(str);
        }
      } else {
        tooltip.setText(MistUtil.format.time(secs));
      }
    };
    MistUtil.event.addListener(margincontainer, "mousemove", function(e) {
      container.moveTooltip(e);
    });
    var dragging = false;
    MistUtil.event.addListener(margincontainer, "mousedown", function(e) {
      if (e.which != 1) {
        return;
      }
      dragging = true;
      container.updateBar(container.getPos(e));
      var moveListener = MistUtil.event.addListener(document, "mousemove", function(e2) {
        container.updateBar(container.getPos(e2));
        container.moveTooltip(e2);
      }, container);
      var upListener = MistUtil.event.addListener(document, "mouseup", function(e2) {
        if (e2.which != 1) {
          return;
        }
        dragging = false;
        MistUtil.event.removeListener(moveListener);
        MistUtil.event.removeListener(upListener);
        tooltip.style.opacity = 0;
        if (!e2.composedPath() || MistUtil.array.indexOf(e2.composedPath(), margincontainer) < 0) {
          container.seek(e2);
        }
      }, container);
    });
    return margincontainer;
  },
  play: function() {
    var MistVideo2 = this;
    var button = document.createElement("div");
    button.setAttribute("role", "button");
    button.setAttribute("tabindex", "0");
    button.setAttribute("aria-label", MistVideo2.translate("play"));
    button.appendChild(this.skin.icons.build("play"));
    button.appendChild(this.skin.icons.build("pause"));
    var playTip = MistUtil.controlTooltip(button, MistVideo2.translate("play") + " (k)");
    button.setState = function(state) {
      this.setAttribute("data-state", state);
      var label = MistVideo2.translate(state == "playing" ? "pause" : "play");
      this.setAttribute("aria-label", label);
      playTip.textContent = label + " (k)";
    };
    button.setState("paused");
    MistVideo2.playerState.on("playing", function(isPlaying) {
      button.setState(isPlaying ? "playing" : "paused");
      if (isPlaying) MistVideo2.options.autoplay = true;
    });
    MistVideo2.playerState.on("ended", function(isEnded) {
      if (isEnded) button.setState("paused");
    });
    MistUtil.event.addListener(button, "click", function() {
      if (MistVideo2.api.error) {
        MistVideo2.api.load();
      }
      if (MistVideo2.api.paused) {
        MistVideo2.api.play();
      } else {
        MistVideo2.api.pause();
        MistVideo2.options.autoplay = false;
      }
    });
    if (MistVideo2.api) {
      MistUtil.event.addListener(MistVideo2.video, "click", function() {
        if (MistVideo2.container.hasAttribute("data-show-submenu")) {
          MistVideo2.container.removeAttribute("data-show-submenu");
          return;
        }
        if (MistVideo2.api.paused) {
          MistVideo2.api.play();
        } else if (!MistUtil.isTouchDevice()) {
          MistVideo2.api.pause();
          MistVideo2.options.autoplay = false;
        }
      }, button);
    }
    return button;
  },
  speaker: function() {
    if (!this.api || !("muted" in this.api)) {
      return false;
    }
    var hasaudio = false;
    var tracks = this.info.meta.tracks;
    for (var i2 in tracks) {
      if (tracks[i2].type == "audio") {
        hasaudio = true;
        break;
      }
    }
    if (!hasaudio) {
      return false;
    }
    var MistVideo2 = this;
    var button = this.skin.icons.build("speaker");
    button.setAttribute("role", "button");
    button.setAttribute("tabindex", "0");
    button.setAttribute("aria-label", MistVideo2.translate("mute"));
    var speakerTip = MistUtil.controlTooltip(button, MistVideo2.translate("mute") + " (m)");
    function updateSpeakerState(vol, isMuted) {
      if (vol && !isMuted) {
        MistUtil.class.remove(button, "off");
        button.setAttribute("aria-label", MistVideo2.translate("mute"));
        speakerTip.textContent = MistVideo2.translate("mute") + " (m)";
      } else {
        MistUtil.class.add(button, "off");
        button.setAttribute("aria-label", MistVideo2.translate("unmute"));
        speakerTip.textContent = MistVideo2.translate("unmute") + " (m)";
      }
    }
    MistVideo2.playerState.on("muted", function(isMuted) {
      updateSpeakerState(MistVideo2.playerState.get("volume"), isMuted);
    });
    MistVideo2.playerState.on("volume", function(vol) {
      updateSpeakerState(vol, MistVideo2.playerState.get("muted"));
    });
    MistUtil.event.addListener(button, "click", function(e) {
      MistVideo2.api.muted = !MistVideo2.api.muted;
    });
    button.addEventListener("wheel", function(e) {
      e.preventDefault();
      var delta = e.deltaY > 0 ? -0.05 : 0.05;
      MistVideo2.api.muted = false;
      if (MistVideo2.gain) {
        var currentGain = MistVideo2.gain.value;
        if (MistVideo2.api.volume >= 1 && delta > 0) {
          MistVideo2.gain.init();
          MistVideo2.gain.value = Math.min(2, currentGain + delta);
          return;
        } else if (currentGain > 1 && delta < 0) {
          MistVideo2.gain.value = Math.max(1, currentGain + delta);
          return;
        }
      }
      var newVol = Math.max(0, Math.min(1, MistVideo2.api.volume + delta));
      MistVideo2.api.volume = newVol;
      if (MistVideo2.gain && MistVideo2.gain.value > 1) {
        MistVideo2.gain.value = 1;
      }
      try {
        localStorage[LS_VOLUME_KEY] = newVol;
      } catch (ex) {
      }
    }, { passive: false });
    return button;
  },
  volume: function(options) {
    if (!this.api || !("volume" in this.api)) {
      return false;
    }
    var hasaudio = false;
    var tracks = this.info.meta.tracks;
    for (var i2 in tracks) {
      if (tracks[i2].type == "audio") {
        hasaudio = true;
        break;
      }
    }
    if (!hasaudio) {
      return false;
    }
    var container = document.createElement("div");
    var button = this.skin.icons.build("volume", "size" in options ? options.size : false);
    button.setAttribute("role", "slider");
    button.setAttribute("aria-label", this.translate("volume"));
    button.setAttribute("aria-valuemin", "0");
    button.setAttribute("aria-valuemax", "100");
    button.setAttribute("aria-valuenow", "100");
    button.setAttribute("tabindex", "0");
    container.appendChild(button);
    var MistVideo2 = this;
    button.mode = "mode" in options ? options.mode : "vertical";
    if (button.mode == "vertical") {
      button.style.transform = "rotate(90deg)";
    }
    button.margin = {
      start: 0.15,
      end: 0.1
    };
    var video = this.video;
    button.set = function(perc) {
      perc = 100 - 100 * Math.pow(1 - perc / 100, 2);
      if (perc != 100 && perc != 0) {
        perc = this.addPadding(perc / 100) * 100;
      }
      var sliders = button.querySelectorAll(".slider");
      for (var i3 = 0; i3 < sliders.length; i3++) {
        sliders[i3].setAttribute(button.mode == "vertical" ? "height" : "width", perc + "%");
      }
    };
    function updateVolume() {
      var isMuted = MistVideo2.playerState.get("muted");
      var vol = MistVideo2.playerState.get("volume");
      var display = isMuted ? 0 : vol * 100;
      button.set(display);
      button.setAttribute("aria-valuenow", Math.round(display));
    }
    MistVideo2.playerState.on("volume", updateVolume);
    MistVideo2.playerState.on("muted", updateVolume);
    var initevent = MistUtil.event.addListener(video, "loadedmetadata", function() {
      if ("localStorage" in window && localStorage != null && LS_VOLUME_KEY in localStorage) {
        MistVideo2.api.volume = localStorage[LS_VOLUME_KEY];
      }
      MistUtil.event.removeListener(initevent);
    });
    button.addPadding = function(actual) {
      return actual * (1 - (this.margin.start + this.margin.end)) + this.margin.start;
    };
    button.removePadding = function(padded) {
      var val = (padded - this.margin.start) / (1 - (this.margin.start + this.margin.end));
      val = Math.max(val, 0);
      val = Math.min(val, 1);
      return val;
    };
    button.getPos = function(e) {
      return this.addPadding(MistUtil.getPos(this, e));
    };
    button.setVolume = function(e) {
      MistVideo2.api.muted = false;
      var val = this.removePadding(MistUtil.getPos(this, e));
      val = 1 - Math.pow(1 - val, 0.5);
      MistVideo2.api.volume = val;
      if (MistVideo2.gain && MistVideo2.gain.value > 1) {
        MistVideo2.gain.value = 1;
      }
      try {
        localStorage[LS_VOLUME_KEY] = MistVideo2.api.volume;
      } catch (e2) {
      }
    };
    MistUtil.event.addListener(button, "mouseup", function(e) {
      if (e.which != 1) {
        return;
      }
      button.setVolume(e);
    });
    var tooltip = MistVideo2.UI.buildStructure({ type: "tooltip" });
    tooltip.style.opacity = 0;
    tooltip.triangle.setMode("bottom", "right");
    container.style.position = "relative";
    container.appendChild(tooltip);
    MistUtil.event.addListener(button, "mouseover", function() {
      tooltip.style.opacity = 1;
    });
    MistUtil.event.addListener(button, "mouseout", function() {
      if (!dragging) {
        tooltip.style.opacity = 0;
      }
    });
    button.moveTooltip = function(e) {
      tooltip.style.opacity = 1;
      var pos = MistUtil.getPos(this, e);
      var displayVol = Math.round(this.removePadding(pos) * 100);
      if (MistVideo2.gain && MistVideo2.gain.value > 1) {
        displayVol = Math.round(MistVideo2.gain.value * 100);
      }
      tooltip.setText(displayVol + "%");
      tooltip.setPos({
        bottom: 46,
        right: 100 * (1 - pos) + "%"
      });
    };
    MistUtil.event.addListener(button, "mousemove", function(e) {
      button.moveTooltip(e);
    });
    var dragging = false;
    var volumeGroup = container.closest ? null : null;
    function getVolumeGroup() {
      if (!volumeGroup) {
        var el = container.parentElement;
        while (el) {
          if (MistUtil.class.has(el, "mistvideo-volume_group")) {
            volumeGroup = el;
            break;
          }
          el = el.parentElement;
        }
      }
      return volumeGroup;
    }
    MistUtil.event.addListener(button, "mousedown", function(e) {
      if (e.which != 1) {
        return;
      }
      dragging = true;
      var vg = getVolumeGroup();
      if (vg) MistUtil.class.add(vg, "dragging");
      button.setVolume(e);
      tooltip.style.opacity = 1;
      var rafId = null;
      var lastEvent = null;
      function onDragMove(ev) {
        lastEvent = ev;
        if (!rafId) {
          rafId = requestAnimationFrame(function() {
            rafId = null;
            if (lastEvent) {
              button.setVolume(lastEvent);
              button.moveTooltip(lastEvent);
            }
          });
        }
      }
      var moveListener = MistUtil.event.addListener(document, "mousemove", onDragMove, button);
      var upListener = MistUtil.event.addListener(document, "mouseup", function(e2) {
        if (e2.which != 1) {
          return;
        }
        dragging = false;
        if (vg) MistUtil.class.remove(vg, "dragging");
        if (rafId) {
          cancelAnimationFrame(rafId);
          rafId = null;
        }
        MistUtil.event.removeListener(moveListener);
        MistUtil.event.removeListener(upListener);
        tooltip.style.opacity = 0;
        if (!e2.composedPath || !e2.composedPath() || MistUtil.array.indexOf(e2.composedPath(), button) < 0) {
          button.setVolume(e2);
        }
      }, button);
    });
    container.addEventListener("wheel", function(e) {
      e.preventDefault();
      var delta = e.deltaY > 0 ? -0.05 : 0.05;
      MistVideo2.api.muted = false;
      if (MistVideo2.gain) {
        var currentGain = MistVideo2.gain.value;
        if (MistVideo2.api.volume >= 1 && delta > 0) {
          MistVideo2.gain.init();
          MistVideo2.gain.value = Math.min(2, currentGain + delta);
          return;
        } else if (currentGain > 1 && delta < 0) {
          MistVideo2.gain.value = Math.max(1, currentGain + delta);
          return;
        }
      }
      var newVol = Math.max(0, Math.min(1, MistVideo2.api.volume + delta));
      MistVideo2.api.volume = newVol;
      if (MistVideo2.gain && MistVideo2.gain.value > 1) {
        MistVideo2.gain.value = 1;
      }
      try {
        localStorage[LS_VOLUME_KEY] = newVol;
      } catch (ex) {
      }
    }, { passive: false });
    return container;
  },
  currentTime: function() {
    var MistVideo2 = this;
    var container = document.createElement("div");
    var text = document.createTextNode("");
    var realtime = document.createElement("span");
    realtime.setAttribute("class", "mistvideo-realtime");
    container.appendChild(text);
    container.appendChild(realtime);
    var realtimetext = document.createTextNode("");
    realtime.appendChild(realtimetext);
    var formatTime = MistUtil.format.time;
    var tr = MistVideo2.translate.bind(MistVideo2);
    container.set = function() {
      var v = MistVideo2.api.currentTime;
      var t;
      if (MistVideo2.options.useDateTime && MistVideo2.info && MistVideo2.info.unixoffset) {
        var d = new Date(MistVideo2.info.unixoffset + v * 1e3);
        var ago = /* @__PURE__ */ new Date() - d;
        realtimetext.nodeValue = "";
        if (MistVideo2.info.type == "live") {
          if (ago < 6e4) {
            t = MistUtil.format.ago(d, null, tr);
          } else if (ago < 12 * 36e5) {
            t = "- " + MistUtil.format.time(ago * 1e-3);
          } else {
            t = MistUtil.format.ago(d, null, tr);
          }
        } else {
          t = formatTime(v);
          if (ago > 6e4 && MistVideo2.size.width >= 600) {
            realtimetext.nodeValue = " (" + MistVideo2.translate("at") + " " + MistUtil.format.ago(d, null, tr) + ")";
          }
        }
        container.setAttribute("title", MistUtil.format.ago(d, 3456e7));
      } else {
        t = formatTime(v);
        container.setAttribute("title", t);
      }
      text.nodeValue = t;
    };
    container.set();
    MistVideo2.playerState.on("currentTime", function() {
      container.set();
    });
    MistVideo2.playerState.on("seeking", function(isSeeking) {
      if (isSeeking) container.set();
    });
    return container;
  },
  totalTime: function() {
    var MistVideo2 = this;
    var container = document.createElement("div");
    var text = document.createTextNode("");
    container.appendChild(text);
    if (MistVideo2.info.type == "live") {
      text.nodeValue = MistVideo2.translate("live");
      container.className = "live";
    } else {
      container.set = function(duration) {
        if (isNaN(duration) || !isFinite(duration)) {
          this.style.display = "none";
          return;
        }
        this.style.display = "";
        if (MistVideo2.options.useDateTime && MistVideo2.info && (MistVideo2.info.type == "live" && MistVideo2.info.unixoffset)) {
          var t = new Date(duration * 1e3 + MistVideo2.info.unixoffset);
          var tr = MistVideo2.translate.bind(MistVideo2);
          text.nodeValue = MistUtil.format.ago(t, null, tr);
          container.setAttribute("title", MistUtil.format.ago(t, 3456e7));
        } else {
          text.nodeValue = MistUtil.format.time(duration);
          container.setAttribute("title", text.nodeValue);
        }
      };
      MistVideo2.playerState.on("duration", function(dur) {
        container.set(dur);
      });
    }
    return container;
  },
  seekBackward: function() {
    if (!this.api || this.info.type == "live") {
      return;
    }
    var MistVideo2 = this;
    var button = this.skin.icons.build("seekbackward");
    button.setAttribute("role", "button");
    button.setAttribute("tabindex", "0");
    button.setAttribute("aria-label", MistVideo2.translate("seek backward"));
    MistUtil.controlTooltip(button, MistVideo2.translate("-10s"));
    MistUtil.event.addListener(button, "click", function() {
      MistVideo2.api.currentTime = Math.max(0, MistVideo2.api.currentTime - 10);
    });
    return button;
  },
  seekForward: function() {
    if (!this.api || this.info.type == "live") {
      return;
    }
    var MistVideo2 = this;
    var button = this.skin.icons.build("seekforward");
    button.setAttribute("role", "button");
    button.setAttribute("tabindex", "0");
    button.setAttribute("aria-label", MistVideo2.translate("seek forward"));
    MistUtil.controlTooltip(button, MistVideo2.translate("+10s"));
    MistUtil.event.addListener(button, "click", function() {
      MistVideo2.api.currentTime = Math.min(MistVideo2.api.duration, MistVideo2.api.currentTime + 10);
    });
    return button;
  }
};

// src/ui/blueprints/menu.js
function createMenu(MistVideo2, options) {
  var container = document.createElement("div");
  container.className = "mistvideo-menu";
  container.setAttribute("role", "listbox");
  container.setAttribute("aria-label", options.type || "menu");
  container.setAttribute("tabindex", "-1");
  var currentValue = options.selected || "";
  var itemElements = [];
  function buildItems() {
    MistUtil.empty(container);
    itemElements = [];
    for (var i2 = 0; i2 < options.items.length; i2++) {
      var item = options.items[i2];
      var el = document.createElement("div");
      el.className = "mistvideo-menu-item";
      el.setAttribute("role", "option");
      el.setAttribute("tabindex", "0");
      el.setAttribute("data-value", item.value);
      if (item.value === currentValue) {
        el.setAttribute("aria-selected", "true");
        el.setAttribute("data-selected", "");
      }
      var indicator = document.createElement("span");
      indicator.className = "mistvideo-menu-indicator";
      el.appendChild(indicator);
      var label = document.createElement("span");
      label.className = "mistvideo-menu-label";
      label.textContent = item.label;
      el.appendChild(label);
      el._value = item.value;
      itemElements.push(el);
      container.appendChild(el);
      (function(el2, val) {
        el2.addEventListener("click", function(e) {
          e.stopPropagation();
          setValue(val);
          if (options.onChange) options.onChange(val);
          hide();
        });
        el2.addEventListener("keydown", function(e) {
          var idx = itemElements.indexOf(el2);
          if (e.key === "ArrowDown" && idx < itemElements.length - 1) {
            itemElements[idx + 1].focus();
            e.preventDefault();
          } else if (e.key === "ArrowUp" && idx > 0) {
            itemElements[idx - 1].focus();
            e.preventDefault();
          } else if (e.key === "Enter" || e.key === " ") {
            el2.click();
            e.preventDefault();
          } else if (e.key === "Escape") {
            hide();
            e.preventDefault();
          }
        });
      })(el, item.value);
    }
  }
  function setValue(val) {
    currentValue = val;
    for (var i2 = 0; i2 < itemElements.length; i2++) {
      if (itemElements[i2]._value === val) {
        itemElements[i2].setAttribute("aria-selected", "true");
        itemElements[i2].setAttribute("data-selected", "");
      } else {
        itemElements[i2].removeAttribute("aria-selected");
        itemElements[i2].removeAttribute("data-selected");
      }
    }
  }
  function show() {
    var allMenus = MistVideo2.container.querySelectorAll(".mistvideo-menu[data-open]");
    for (var i2 = 0; i2 < allMenus.length; i2++) {
      if (allMenus[i2] !== container) {
        allMenus[i2].style.display = "none";
        allMenus[i2].removeAttribute("data-open");
      }
    }
    container.style.display = "";
    container.setAttribute("data-open", "");
    for (var i2 = 0; i2 < itemElements.length; i2++) {
      if (itemElements[i2].hasAttribute("data-selected")) {
        itemElements[i2].focus();
        return;
      }
    }
    if (itemElements.length) itemElements[0].focus();
  }
  function hide() {
    container.style.display = "none";
    container.removeAttribute("data-open");
  }
  container.style.display = "none";
  buildItems();
  container.setValue = setValue;
  container.getValue = function() {
    return currentValue;
  };
  container.show = show;
  container.hide = hide;
  container.toggle = function() {
    container.style.display === "none" ? show() : hide();
  };
  container.setItems = function(items) {
    options.items = items;
    buildItems();
  };
  document.addEventListener("click", function(e) {
    if (!container.contains(e.target) && container.style.display !== "none") {
      hide();
    }
  });
  return container;
}

// src/ui/blueprints/settings.js
var settingsBlueprints = {
  playername: function() {
    if (!this.playerName || !(this.playerName in mistplayers)) {
      return;
    }
    var container = document.createElement("span");
    container.appendChild(document.createTextNode(mistplayers[this.playerName].name));
    return container;
  },
  mimetype: function() {
    if (!this.source) {
      return;
    }
    var a = document.createElement("a");
    a.href = this.source.url;
    a.target = "_blank";
    a.title = a.href + " (" + this.source.type + ")";
    a.appendChild(document.createTextNode(MistUtil.format.mime2human(this.source.type)));
    return a;
  },
  logo: function(options) {
    if ("element" in options) {
      return options.element;
    }
    if ("src" in options) {
      var img = document.createElement("img");
      img.src = options.src;
      return img;
    }
  },
  settings: function() {
    var MistVideo2 = this;
    var button = this.skin.icons.build("settings");
    button.setAttribute("role", "button");
    button.setAttribute("tabindex", "0");
    button.setAttribute("aria-label", MistVideo2.translate("settings"));
    MistUtil.controlTooltip(button, MistVideo2.translate("settings"));
    var touchmode = typeof document.ontouchstart != "undefined";
    function closeSubmenu() {
      MistVideo2.container.removeAttribute("data-show-submenu");
      var openMenus = MistVideo2.container.querySelectorAll(".mistvideo-menu[data-open]");
      for (var i2 = 0; i2 < openMenus.length; i2++) {
        openMenus[i2].style.display = "none";
        openMenus[i2].removeAttribute("data-open");
      }
    }
    function openSubmenu() {
      MistVideo2.container.setAttribute("data-show-submenu", "");
      function onDocClick(e) {
        if (!MistVideo2.container.contains(e.target) || e.target === MistVideo2.video) {
          closeSubmenu();
          document.removeEventListener("click", onDocClick, true);
        }
      }
      setTimeout(function() {
        document.addEventListener("click", onDocClick, true);
      }, 0);
    }
    MistUtil.event.addListener(button, "click", function(e) {
      e.stopPropagation();
      if (MistVideo2.container.hasAttribute("data-show-submenu")) {
        closeSubmenu();
      } else {
        openSubmenu();
      }
    });
    return button;
  },
  loop: function() {
    if (!this.api || !("loop" in this.api) || this.info.type == "live") {
      return;
    }
    var MistVideo2 = this;
    var button = this.skin.icons.build("loop");
    button.setAttribute("role", "button");
    button.setAttribute("tabindex", "0");
    button.setAttribute("aria-label", MistVideo2.translate("loop"));
    MistUtil.controlTooltip(button, MistVideo2.translate("loop"));
    button.set = function() {
      if (MistVideo2.api.loop) {
        MistUtil.class.remove(this, "off");
      } else {
        MistUtil.class.add(this, "off");
      }
    };
    MistUtil.event.addListener(button, "click", function(e) {
      MistVideo2.api.loop = !MistVideo2.api.loop;
      this.set();
    });
    button.set();
    return button;
  },
  fullscreen: function() {
    if (!("setSize" in this.player) || !this.info.hasVideo || this.source.type.split("/")[1] == "audio") {
      return;
    }
    if (!this.fullscreen.supported) {
      return;
    }
    var MistVideo2 = this;
    var button = this.skin.icons.build("fullscreen");
    button.setAttribute("role", "button");
    button.setAttribute("tabindex", "0");
    button.setAttribute("aria-label", MistVideo2.translate("fullscreen"));
    var fsTip = MistUtil.controlTooltip(button, MistVideo2.translate("fullscreen") + " (f)");
    MistUtil.event.addListener(button, "click", function() {
      MistVideo2.fullscreen.toggle();
    });
    MistUtil.event.addListener(MistVideo2.video, "dblclick", function() {
      MistVideo2.fullscreen.toggle();
    });
    var fsEvents = ["fullscreenchange", "webkitfullscreenchange", "mozfullscreenchange", "MSFullscreenChange", "fakefullscreenchange"];
    for (var i2 = 0; i2 < fsEvents.length; i2++) {
      MistUtil.event.addListener(document, fsEvents[i2], function() {
        if (MistVideo2.fullscreen.active) {
          MistVideo2.container.setAttribute("data-fullscreen", "");
          button.setAttribute("aria-label", MistVideo2.translate("exit fullscreen"));
          fsTip.textContent = MistVideo2.translate("exit fullscreen") + " (f)";
          if (screen.orientation && screen.orientation.lock) {
            try {
              screen.orientation.lock("landscape").catch(function() {
              });
            } catch (e) {
            }
          }
        } else if (MistVideo2.container.hasAttribute("data-fullscreen")) {
          MistVideo2.container.removeAttribute("data-fullscreen");
          button.setAttribute("aria-label", MistVideo2.translate("fullscreen"));
          fsTip.textContent = MistVideo2.translate("fullscreen") + " (f)";
          if (screen.orientation && screen.orientation.unlock) {
            try {
              screen.orientation.unlock();
            } catch (e) {
            }
          }
        }
        MistVideo2.player.resizeAll();
      }, button);
    }
    return button;
  },
  "picture-in-picture": function() {
    if (!("setSize" in this.player) || !this.info.hasVideo || this.source.type.split("/")[1] == "audio") {
      return;
    }
    if (!this.pip.supported) {
      return;
    }
    var MistVideo2 = this;
    var button = this.skin.icons.build("pip");
    button.setAttribute("role", "button");
    button.setAttribute("tabindex", "0");
    button.setAttribute("aria-label", MistVideo2.translate("pip"));
    MistUtil.controlTooltip(button, MistVideo2.translate("pip") + " (i)");
    button.set = function() {
      if (MistVideo2.pip.active) {
        MistUtil.class.remove(this, "off");
      } else {
        MistUtil.class.add(this, "off");
      }
    };
    MistUtil.event.addListener(button, "click", function() {
      MistVideo2.pip.toggle().then(function() {
        button.set();
      });
    });
    button.set();
    return button;
  },
  tracks: function() {
    if (!this.info || !this.video) {
      return;
    }
    var MistVideo2 = this;
    var table = document.createElement("table");
    function build(tracks) {
      MistUtil.empty(table);
      tracks = MistUtil.tracks.parse(tracks);
      var selections = {};
      var checkboxes = {};
      function changeToTracks(type2, value) {
        if (value) {
          MistVideo2.log("User selected " + type2 + " track with id " + value);
        } else {
          MistVideo2.log("User selected automatic track selection for " + type2);
          MistUtil.event.send("trackSetToAuto", type2, MistVideo2.video);
        }
        if (!MistVideo2.options.setTracks) {
          MistVideo2.options.setTracks = {};
        }
        MistVideo2.options.setTracks[type2] = value;
        if (value === true && selections[type2]) {
          MistUtil.event.send("change", null, selections[type2]);
        }
        if ("setTrack" in MistVideo2.player.api) {
          return MistVideo2.player.api.setTrack(type2, value);
        } else {
          var usetracks = {};
          for (var i2 in selections) {
            if (i2 == "subtitle" || selections[i2].value == "") {
              continue;
            }
            usetracks[i2] = selections[i2].value;
          }
          if (value != "") {
            usetracks[type2] = value;
          }
          if ("setTracks" in MistVideo2.player.api) {
            return MistVideo2.player.api.setTracks(usetracks);
          }
          if ("setSource" in MistVideo2.player.api) {
            return MistVideo2.player.api.setSource(
              MistUtil.http.url.addParam(MistVideo2.source.url, usetracks)
            );
          }
        }
      }
      var tracktypes = MistUtil.object.keys(tracks, function(keya, keyb) {
        function order(value) {
          switch (value) {
            case "audio":
              return "aaaaaaa";
            case "video":
              return "aaaaaab";
            default:
              return value;
          }
        }
        if (order(keya) > order(keyb)) {
          return 1;
        }
        if (order(keya) < order(keyb)) {
          return -1;
        }
        return 0;
      });
      function processTrack(t2, type2) {
        if (MistUtil.array.indexOf(["video", "audio", "subtitle"], type2) <= -1) {
          return;
        }
        if (type2 == "subtitle") {
          if (!("player" in MistVideo2) || !("api" in MistVideo2.player) || !("setWSSubtitle" in MistVideo2.player.api) && !("setSubtitle" in MistVideo2.player.api)) {
            MistVideo2.log("Subtitle selection was disabled as this player does not support it.");
            return;
          }
          var mime = "html5/text/vtt";
          if ("setWSSubtitle" in MistVideo2.player.api) {
            mime = "html5/text/javascript";
          }
          var subtitleSource = false;
          for (var i2 in MistVideo2.info.source) {
            var source = MistVideo2.info.source[i2];
            if (source.type == mime && MistUtil.http.url.split(source.url).protocol == MistUtil.http.url.split(MistVideo2.source.url).protocol.replace(/^ws/, "http")) {
              subtitleSource = source.url.replace(/.srt$/, ".vtt");
              break;
            }
          }
          if (!subtitleSource) {
            MistVideo2.log("Subtitle selection was disabled as a source could not be found.");
            return;
          }
          t2[""] = { trackid: "", different: { none: MistVideo2.translate("none") } };
        }
        var tr = document.createElement("tr");
        tr.title = MistVideo2.translate("the current track") + " " + type2 + " track";
        table.appendChild(tr);
        if ("decodingIssues" in MistVideo2.skin.blueprints) {
          var cell = document.createElement("td");
          tr.appendChild(cell);
          if (type2 != "subtitle") {
            var checkbox = document.createElement("input");
            checkbox.setAttribute("type", "checkbox");
            checkbox.setAttribute("checked", "");
            checkbox.setAttribute("title", "Whether or not to play " + type2);
            checkbox.trackType = type2;
            cell.appendChild(checkbox);
            checkboxes[type2] = checkbox;
            if (MistVideo2.options.setTracks && MistVideo2.options.setTracks[type2]) {
              if (MistVideo2.options.setTracks[type2] == "none") {
                checkbox.checked = false;
              } else {
                checkbox.checked = true;
              }
            }
            MistUtil.event.addListener(checkbox, "change", function() {
              var n = 0;
              for (var i3 in checkboxes) {
                if (checkboxes[i3].checked) {
                  n++;
                }
              }
              if (n == 0) {
                for (var i3 in checkboxes) {
                  if (i3 == this.trackType) {
                    continue;
                  }
                  if (!checkboxes[i3].checked) {
                    checkboxes[i3].checked = true;
                    changeToTracks(i3, true);
                    break;
                  }
                }
              }
              var value = "none";
              if (this.checked) {
                if (this.trackType in selections) {
                  value = selections[this.trackType].value;
                } else {
                  value = "auto";
                }
              } else {
                value = "none";
              }
              changeToTracks(this.trackType, this.checked ? value : "none");
            });
            MistUtil.event.addListener(MistVideo2.video, "playerUpdate_trackChanged", function(e2) {
              if (e2.message.type != type2) {
                return;
              }
              if (e2.message.value == "none") {
                this.checked = false;
              } else {
                this.checked = true;
              }
            }, select);
          }
        }
        var header = document.createElement("td");
        tr.appendChild(header);
        header.appendChild(document.createTextNode(MistUtil.format.ucFirst(type2) + ":"));
        var cell = document.createElement("td");
        tr.appendChild(cell);
        var trackkeys = MistUtil.object.keys(t2);
        function orderValues(trackinfoobj) {
          var order = {
            trackid: 0,
            language: 1,
            width: 2,
            bps: 3,
            fpks: 4,
            channels: 5,
            codec: 6,
            rate: 7
          };
          return MistUtil.object.values(trackinfoobj, function(keya, keyb, valuea, valueb) {
            if (order[keya] > order[keyb]) {
              return 1;
            }
            if (order[keya] < order[keyb]) {
              return -1;
            }
            return 0;
          });
        }
        if (trackkeys.length > 1 && "player" in MistVideo2 && "api" in MistVideo2.player && ("setTrack" in MistVideo2.player.api || "setTracks" in MistVideo2.player.api || "setSource" in MistVideo2.player.api)) {
          let n = function(str) {
            if (str == "") {
              return -1;
            }
            return Number(str);
          }, autoLabel = function(trackDesc) {
            return MistVideo2.translate("automatic") + (trackDesc ? " (" + trackDesc + ")" : "");
          };
          var select = document.createElement("select");
          select.setAttribute("data-type", type2);
          select.style.display = "none";
          selections[type2] = select;
          select.trackType = type2;
          cell.appendChild(select);
          var menuItems = [];
          if (type2 != "subtitle") {
            var option = document.createElement("option");
            select.appendChild(option);
            option.value = "";
            option.appendChild(document.createTextNode(MistVideo2.translate("automatic")));
            menuItems.push({ value: "", label: MistVideo2.translate("automatic") });
          }
          var sortedKeys = MistUtil.object.keys(t2, function(a, b) {
            return n(a) - n(b);
          });
          for (var i2 in sortedKeys) {
            var track = t2[sortedKeys[i2]];
            var option = document.createElement("option");
            select.appendChild(option);
            option.value = "idx" in track ? track.idx : track.trackid;
            var labelText;
            if (MistUtil.object.keys(track.different).length) {
              labelText = orderValues(track.different).join(" ");
            } else {
              labelText = MistVideo2.translate("track") + " " + (Number(i2) + 1);
            }
            option.appendChild(document.createTextNode(labelText));
            menuItems.push({ value: option.value, label: labelText });
          }
          var menuWrap = document.createElement("div");
          menuWrap.style.position = "relative";
          cell.appendChild(menuWrap);
          var menuBtn = document.createElement("button");
          menuBtn.className = "mistvideo-menu-trigger";
          menuBtn.setAttribute("role", "button");
          menuBtn.setAttribute("aria-haspopup", "listbox");
          if (type2 != "subtitle" && menuItems.length > 1) {
            var defaultTrack = menuItems[MistVideo2.info.type == "live" ? menuItems.length - 1 : 1];
            menuBtn.textContent = MistVideo2.translate("automatic") + " (" + defaultTrack.label + ")";
          } else {
            menuBtn.textContent = menuItems.length ? menuItems[0].label : "";
          }
          menuWrap.appendChild(menuBtn);
          var menu = createMenu(MistVideo2, {
            type: type2,
            items: menuItems,
            onChange: function(val) {
              select.value = val;
              if (val === "" && menuItems.length > 1) {
                var def = menuItems[MistVideo2.info.type == "live" ? menuItems.length - 1 : 1];
                menuBtn.textContent = autoLabel(def.label);
              } else {
                menuBtn.textContent = "";
                for (var m = 0; m < menuItems.length; m++) {
                  if (menuItems[m].value === val) {
                    menuBtn.textContent = menuItems[m].label;
                    break;
                  }
                }
              }
              MistUtil.event.send("change", val, select);
            }
          });
          menuWrap.appendChild(menu);
          menuBtn.addEventListener("click", function(e2) {
            e2.stopPropagation();
            menu.toggle();
          });
          MistUtil.event.addListener(MistVideo2.video, "playerUpdate_trackChanged", function(e2) {
            if (e2.message.type != type2 || e2.message.trackid == "none") {
              return;
            }
            var userChoseAuto = !MistVideo2.options.setTracks || !MistVideo2.options.setTracks[type2];
            if (userChoseAuto) {
              var trackDesc = "";
              var tid = String(e2.message.trackid);
              for (var m = 0; m < menuItems.length; m++) {
                if (menuItems[m].value === tid) {
                  trackDesc = menuItems[m].label;
                  break;
                }
              }
              menuBtn.textContent = autoLabel(trackDesc);
            } else {
              select.value = e2.message.trackid;
              menu.setValue(e2.message.trackid);
              for (var m = 0; m < menuItems.length; m++) {
                if (menuItems[m].value === String(e2.message.trackid)) {
                  menuBtn.textContent = menuItems[m].label;
                  break;
                }
              }
            }
            MistVideo2.log("Player selected " + type2 + " track with id " + e2.message.trackid);
          }, select);
          if (type2 == "subtitle") {
            MistUtil.event.addListener(select, "change", function() {
              try {
                localStorage["mistSubtitleLanguage"] = t2[this.value].lang;
              } catch (e2) {
              }
              if ("setWSSubtitle" in MistVideo2.player.api) {
                MistVideo2.player.api.setWSSubtitle(this.value == "" ? void 0 : this.value);
              } else {
                if (this.value != "") {
                  var trackinfo = MistUtil.object.extend({}, t2[this.value]);
                  trackinfo.label = orderValues(trackinfo.describe).join(" ");
                  trackinfo.src = MistUtil.http.url.addParam(subtitleSource, { track: this.value });
                  MistVideo2.player.api.setSubtitle(trackinfo);
                } else {
                  MistVideo2.player.api.setSubtitle();
                }
              }
            });
            if ("localStorage" in window && localStorage != null && "mistSubtitleLanguage" in localStorage) {
              for (var i2 in t2) {
                if (t2[i2].lang == localStorage["mistSubtitleLanguage"]) {
                  select.value = i2;
                  var e = document.createEvent("Event");
                  e.initEvent("change");
                  select.dispatchEvent(e);
                  break;
                }
              }
            }
          } else {
            MistUtil.event.addListener(select, "change", function() {
              if (this.trackType in checkboxes) {
                checkboxes[this.trackType].checked = true;
              }
              if (!changeToTracks(this.trackType, this.value)) {
              }
            });
          }
        } else {
          var span = document.createElement("span");
          span.className = "mistvideo-description";
          cell.appendChild(span);
          span.appendChild(document.createTextNode(orderValues(t2[trackkeys[0]].same).join(" ")));
        }
      }
      for (var j in tracktypes) {
        var type = tracktypes[j];
        var t = tracks[type];
        processTrack(t, type);
      }
    }
    build(this.info.meta.tracks);
    MistUtil.event.addListener(MistVideo2.video, "metaUpdate_tracks", function(e) {
      build(e.message.meta.tracks);
    }, table);
    return table;
  }
};

// src/ui/blueprints/overlay.js
var overlayBlueprints = {
  text: function(options) {
    var container = document.createElement("span");
    var str = options.i18n ? this.translate(options.i18n, options.text || options.i18n) : options.text || "";
    container.appendChild(document.createTextNode(str));
    return container;
  },
  placeholder: function() {
    var placeholder = document.createElement("div");
    var MistVideo2 = this;
    if (this.options.fillSpace && !this.player) {
      var w, h;
      placeholder.setSize = function() {
        placeholder.style.width = w + "px";
        placeholder.style.height = h + "px";
      };
      var onInserted = function() {
        if (MistVideo2.destroyed) return;
        if (placeholder.parentNode) {
          w = window.innerWidth;
          var aspect = 16 / 9;
          h = w / aspect;
          if (MistVideo2.options.poster) {
            var img = new Image();
            img.src = MistVideo2.options.poster;
            img.onload = function() {
              aspect = img.naturalWidth / img.naturalHeight;
              h = w / aspect;
              placeholder.setSize();
            };
          }
          placeholder.setSize();
          if (MistVideo2.container.clientWidth < w) {
            w = MistVideo2.container.clientWidth;
            h = w / aspect;
          }
          placeholder.setSize();
          return;
        } else if (MistVideo2.options.target.children.length > 0) {
          return;
        }
        setTimeout(onInserted, 100);
      };
      onInserted();
      MistUtil.event.addListener(window, "resize", function() {
        onInserted();
      }, placeholder);
    } else {
      var size = this.calcSize();
      placeholder.style.width = size.width + "px";
      placeholder.style.height = size.height + "px";
    }
    if (this.options.poster) placeholder.style.background = "url('" + this.options.poster + "') no-repeat 50%/contain";
    return placeholder;
  },
  timeout: function(options) {
    if (false in options) {
      return;
    }
    var delay = "delay" in options ? options.delay : 5;
    var icon = this.skin.icons.build("timeout", false, { delay });
    icon.timeout = this.timers.start(function() {
      options.function();
    }, delay * 1e3);
    return icon;
  },
  polling: function() {
    var div = document.createElement("div");
    var icon = this.skin.icons.build("loading");
    div.appendChild(icon);
    return div;
  },
  loading: function() {
    var MistVideo2 = this;
    var icon = this.skin.icons.build("loading", 50);
    icon.setAttribute("aria-live", "polite");
    icon.setAttribute("role", "status");
    if (MistVideo2.api) {
      let addIcon = function(e) {
        MistVideo2.container.setAttribute("data-loading", e.type);
        checkIfOk();
      }, removeIcon = function() {
        MistVideo2.container.removeAttribute("data-loading");
        if (timer) {
          MistVideo2.timers.stop(timer);
        }
        timer = false;
      }, checkIfOk = function() {
        if (!timer) {
          timer = MistVideo2.timers.start(function() {
            timer = false;
            if (MistVideo2.monitor.vars && MistVideo2.monitor.vars.score >= 0.999) {
              removeIcon();
            } else {
              checkIfOk();
            }
          }, 1e3);
        }
      };
      var timer = false;
      var events = ["waiting", "seeking", "stalled"];
      for (var i2 in events) {
        MistUtil.event.addListener(MistVideo2.video, events[i2], function(e) {
          if (!MistVideo2.api.paused && "container" in MistVideo2) {
            addIcon(e);
          }
        }, icon);
      }
      var events = ["seeked", "playing", "canplay", "paused", "ended"];
      for (var i2 in events) {
        MistUtil.event.addListener(MistVideo2.video, events[i2], function(e) {
          if ("container" in MistVideo2) {
            removeIcon();
          }
        }, icon);
      }
      MistUtil.event.addListener(MistVideo2.video, "progress", function(e) {
        if ("container" in MistVideo2 && "monitor" in MistVideo2 && "vars" in MistVideo2.monitor && "score" in MistVideo2.monitor.vars && MistVideo2.monitor.vars.score > 0.99) {
          removeIcon();
        }
      }, icon);
    }
    return icon;
  },
  error: function() {
    var MistVideo2 = this;
    var container = document.createElement("div");
    container.setAttribute("aria-live", "polite");
    container.setAttribute("role", "alert");
    container.message = function(message, details, options) {
      MistUtil.empty(this);
      var message_container = document.createElement("div");
      message_container.className = "message";
      this.appendChild(message_container);
      if (!options.polling && !options.passive && !options.hideTitle) {
        var header = document.createElement("h3");
        message_container.appendChild(header);
        header.appendChild(document.createTextNode(MistVideo2.translate(MistVideo2.casting ? "chromecast encountered a problem" : "player encountered a problem")));
      }
      var p11 = document.createElement("p");
      message_container.appendChild(p11);
      message_container.update = function(message2) {
        MistUtil.empty(p11);
        p11.innerHTML = message2;
      };
      if (message) {
        if (MistVideo2.info.on_error) {
          message = MistVideo2.info.on_error.replace(/\<error>/, message);
        }
        message_container.update(message);
        var d = document.createElement("p");
        d.className = "details mistvideo-description";
        message_container.appendChild(d);
        if (details) {
          d.appendChild(document.createTextNode(details));
        } else if ("decodingIssues" in MistVideo2.skin.blueprints) {
          if ("player" in MistVideo2 && "api" in MistVideo2.player && MistVideo2.video) {
            details = [];
            if (typeof MistVideo2.state != "undefined") {
              details.push(["Stream state:", MistVideo2.state]);
            }
            if (typeof MistVideo2.player.api.currentTime != "undefined") {
              details.push(["Current video time:", MistUtil.format.time(MistVideo2.player.api.currentTime)]);
            }
            if ("video" in MistVideo2 && "getVideoPlaybackQuality" in MistVideo2.video) {
              var data = MistVideo2.video.getVideoPlaybackQuality();
              if ("droppedVideoFrames" in data && "totalVideoFrames" in data && data.totalVideoFrames) {
                details.push(["Frames dropped/total:", MistUtil.format.number(data.droppedVideoFrames) + "/" + MistUtil.format.number(data.totalVideoFrames)]);
              }
              if ("corruptedVideoFrames" in data && data.corruptedVideoFrames) {
                details.push(["Corrupted frames:", MistUtil.format.number(data.corruptedVideoFrames)]);
              }
            }
            var networkstates = {
              0: ["NETWORK EMPTY:", "not yet initialized"],
              1: ["NETWORK IDLE:", "resource selected, but not in use"],
              2: ["NETWORK LOADING:", "data is being downloaded"],
              3: ["NETWORK NO SOURCE:", "could not locate source"]
            };
            details.push(networkstates[MistVideo2.video.networkState]);
            var readystates = {
              0: ["HAVE NOTHING:", "no information about ready state"],
              1: ["HAVE METADATA:", "metadata has been loaded"],
              2: ["HAVE CURRENT DATA:", "data for the current playback position is available, but not for the next frame"],
              3: ["HAVE FUTURE DATA:", "data for current and next frame is available"],
              4: ["HAVE ENOUGH DATA:", "can start playing"]
            };
            details.push(readystates[MistVideo2.video.readyState]);
            if (!options.passive) {
              var table = document.createElement("table");
              for (var i3 in details) {
                var tr = document.createElement("tr");
                table.appendChild(tr);
                for (var j in details[i3]) {
                  var td = document.createElement("td");
                  tr.appendChild(td);
                  td.appendChild(document.createTextNode(details[i3][j]));
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
          var s = MistVideo2.UI.buildStructure({ type: "forcePlayer" });
          if (s) {
            c.appendChild(s);
          }
          var s = MistVideo2.UI.buildStructure({ type: "forceType" });
          if (s) {
            c.appendChild(s);
          }
        }
      }
      return message_container;
    };
    var countdown = false;
    var showingError = false;
    var since = false;
    var message_global;
    var ignoreThese = {};
    this.showError = function(message, options) {
      if (!options) {
        options = {
          softReload: !!(MistVideo2.player && MistVideo2.player.api && MistVideo2.player.api.load),
          reload: true,
          nextCombo: !!MistVideo2.info,
          polling: false,
          passive: false
        };
      }
      var identifyer = options.type ? options.type : message;
      if (identifyer in ignoreThese) {
        return;
      }
      if (options.reload === true) {
        if (MistVideo2.options.reloadDelay && !isNaN(Number(MistVideo2.options.reloadDelay))) {
          options.reload = Number(MistVideo2.options.reloadDelay);
        } else {
          options.reload = 10;
        }
      }
      if (options.passive) {
        if (showingError === true) {
          return;
        }
        if (showingError) {
          message_global.update(message);
          since = (/* @__PURE__ */ new Date()).getTime();
          return;
        }
        container.setAttribute("data-passive", "");
      } else {
        container.removeAttribute("data-passive");
      }
      if (showingError) {
        container.clear();
      }
      showingError = options.passive ? "passive" : true;
      since = (/* @__PURE__ */ new Date()).getTime();
      var event;
      if (!MistVideo2.casting) {
        event = this.log(message, "error");
      }
      var message_container = container.message(message, false, options);
      message_global = message_container;
      var button_container = document.createElement("div");
      button_container.className = "mistvideo-buttoncontainer";
      message_container.appendChild(button_container);
      MistUtil.empty(button_container);
      if (MistVideo2.casting && !options.passive) {
        var obj = {
          type: "button",
          label: MistVideo2.translate("stop casting"),
          onclick: function() {
            MistVideo2.detachFromCast();
          }
        };
        if (!isNaN(options.softReload + "")) {
          obj.delay = options.softReload;
        }
        button_container.appendChild(MistVideo2.UI.buildStructure(obj));
      }
      if (options.softReload && !MistVideo2.casting) {
        var obj = {
          type: "button",
          label: MistVideo2.translate("reload video"),
          onclick: function() {
            MistVideo2.player.api.load();
          }
        };
        if (!isNaN(options.softReload + "")) {
          obj.delay = options.softReload;
        }
        button_container.appendChild(MistVideo2.UI.buildStructure(obj));
      }
      if (options.reload) {
        var obj = {
          type: "button",
          label: MistVideo2.translate("reload player"),
          onclick: function() {
            MistVideo2.reload("Reloading because reload button was clicked.");
          }
        };
        if (!isNaN(options.reload + "")) {
          obj.delay = options.reload;
        }
        button_container.appendChild(MistVideo2.UI.buildStructure(obj));
      }
      if (options.nextCombo) {
        var obj = {
          type: "button",
          label: MistVideo2.translate("next source"),
          onclick: function() {
            MistVideo2.nextCombo();
          }
        };
        if (!isNaN(options.nextCombo + "")) {
          obj.delay = options.nextCombo;
        }
        button_container.appendChild(MistVideo2.UI.buildStructure(obj));
      }
      if (options.ignore) {
        var obj = {
          type: "button",
          label: MistVideo2.translate("ignore"),
          onclick: function() {
            this.clearError();
            ignoreThese[identifyer] = true;
          }
        };
        if (!isNaN(options.ignore + "")) {
          obj.delay = options.ignore;
        }
        button_container.appendChild(MistVideo2.UI.buildStructure(obj));
      }
      if (options.polling) {
        button_container.appendChild(MistVideo2.UI.buildStructure({ type: "polling" }));
      }
      MistUtil.class.add(container, "show");
      if ("container" in MistVideo2) {
        MistVideo2.container.removeAttribute("data-loading");
      }
      if (event && event.defaultPrevented) {
        MistVideo2.log("Error event was defaultPrevented, not showing.");
        container.clear();
      }
    };
    container.clear = function() {
      var countdowns = container.querySelectorAll("svg.icon.timeout");
      for (var i3 = 0; i3 < countdowns.length; i3++) {
        MistVideo2.timers.stop(countdowns[i3].timeout);
      }
      MistUtil.empty(container);
      MistUtil.class.remove(container, "show");
      showingError = false;
    };
    this.clearError = container.clear;
    if ("video" in MistVideo2) {
      var events = ["timeupdate", "playing", "canplay"];
      for (var i2 in events) {
        MistUtil.event.addListener(MistVideo2.video, events[i2], function(e) {
          if (!showingError) {
            return;
          }
          if (e.type == "timeupdate") {
            if (MistVideo2.player.api.currentTime == 0) {
              return;
            }
            if ((/* @__PURE__ */ new Date()).getTime() - since < 2e3) {
              return;
            }
          }
          MistVideo2.log("Removing error window because of " + e.type + " event");
          container.clear();
        }, container);
      }
    }
    return container;
  },
  tooltip: function() {
    var container = document.createElement("div");
    var mode = "text";
    var textNode = document.createTextNode("");
    container.appendChild(textNode);
    container.setText = function(text) {
      textNode.nodeValue = text;
      if (mode != "text") {
        container.removeChild(htmlNode);
        container.appendChild(textNode);
        mode = "text";
      }
    };
    var htmlNode = document.createElement("div");
    htmlNode.empty = function() {
      htmlNode.innerText = "";
      for (var i2 = htmlNode.children.length - 1; i2 >= 0; i2--) {
        htmlNode.removeChild(htmlNode.children[i2]);
      }
    };
    container.setHtml = function(ele) {
      htmlNode.empty();
      htmlNode.appendChild(ele);
      if (mode != "html") {
        container.removeChild(textNode);
        container.appendChild(htmlNode);
        mode = "html";
      }
    };
    var triangle = document.createElement("div");
    container.triangle = triangle;
    triangle.className = "triangle";
    container.appendChild(triangle);
    triangle.setMode = function(primary, secondary) {
      if (!primary) {
        primary = "bottom";
      }
      if (!secondary) {
        secondary = "left";
      }
      var sides = ["bottom", "top", "right", "left"];
      for (var i2 in sides) {
        this.style[sides[i2]] = "";
        var cap = MistUtil.format.ucFirst(sides[i2]);
        this.style["border" + cap] = "";
        this.style["border" + cap + "Color"] = "";
      }
      var opposite = {
        top: "bottom",
        bottom: "top",
        left: "right",
        right: "left"
      };
      this.style[primary] = "-10px";
      this.style["border" + MistUtil.format.ucFirst(opposite[primary])] = "none";
      this.style["border" + MistUtil.format.ucFirst(primary) + "Color"] = "transparent";
      this.style[secondary] = 0;
      this.style["border" + MistUtil.format.ucFirst(opposite[secondary])] = "none";
    };
    container.setPos = function(pos) {
      var set = {
        left: "auto",
        right: "auto",
        top: "auto",
        bottom: "auto"
      };
      MistUtil.object.extend(set, pos);
      for (var i2 in set) {
        if (!isNaN(set[i2])) {
          set[i2] += "px";
        }
        this.style[i2] = set[i2];
      }
    };
    return container;
  },
  button: function(options) {
    var button = document.createElement("button");
    var MistVideo2 = this;
    if (options.onclick) {
      MistUtil.event.addListener(button, "click", function() {
        options.onclick.call(MistVideo2, arguments);
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
    var labelStr = options.i18n ? this.translate(options.i18n, options.label || options.i18n) : options.label || "";
    button.appendChild(document.createTextNode(labelStr));
    return button;
  },
  videobackground: function(options) {
    if (!options) {
      options = {};
    }
    if (!options.delay) {
      options.delay = 5;
    }
    var ele = document.createElement("div");
    var MistVideo2 = this;
    if (MistVideo2.options.rotate) {
      switch (MistVideo2.options.rotate) {
        case -1:
        case 1:
        case 2: {
          ele.setAttribute("data-mist-rotate", MistVideo2.options.rotate);
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
      if (options.alwaysDisplay || MistVideo2.video.videoWidth / MistVideo2.video.videoHeight != ele.clientWidth / ele.clientHeight) {
        canvasses[index].removeAttribute("data-front");
        index++;
        if (index >= canvasses.length) {
          index = 0;
        }
        var c2 = canvasses[index];
        var ctx = c2._context;
        c2.width = MistVideo2.video.videoWidth;
        c2.height = MistVideo2.video.videoHeight;
        ctx.drawImage(MistVideo2.video, 0, 0);
        c2.setAttribute("data-front", "");
      }
      if (!MistVideo2.player.api.paused) {
        MistVideo2.timers.start(function() {
          draw();
        }, options.delay * 1e3);
      } else {
        drawing = false;
      }
    }
    MistUtil.event.addListener(MistVideo2.video, "playing", function() {
      if (!drawing) {
        draw();
        drawing = true;
      }
    });
    return ele;
  }
};

// src/ui/blueprints/media.js
var mediaBlueprints = {
  subtitles: function(options) {
    if (!("WebSocket" in window)) {
      return false;
    }
    var MistVideo2 = this;
    if (!MistVideo2.api || !("currentTime" in MistVideo2.api)) {
      return false;
    }
    if (!("metaTrackSubscriptions" in MistVideo2)) {
      return false;
    }
    function clearFormatting(str) {
      str = str.replace(/\<\/?[bui]\>/gi, "");
      str = str.replace(/{\/?[bui]}/gi, "");
      str = str.replace(/{\\a\d+}/gi, "");
      str = str.replace(/\<\/?font[^>]*?\>/gi, "");
      return str;
    }
    var container = document.createElement("div");
    container.addEventListener("click", function(e) {
      if (this.hasAttribute("data-position")) this.removeAttribute("data-position");
      else this.setAttribute("data-position", "top");
      e.stopPropagation();
    });
    var c = document.createElement("span");
    container.appendChild(c);
    var textNode = document.createTextNode("");
    c.appendChild(textNode);
    var timer = false;
    function displayMessage(message) {
      textNode.nodeValue = clearFormatting(message.data);
      if (timer) {
        MistVideo2.timers.stop(timer);
        timer = null;
      }
      function setTimer(delay) {
        timer = MistVideo2.timers.start(function() {
          if (MistVideo2.api.paused) {
            var playing = MistUtil.event.addListener(MistVideo2.video, "playing", function() {
              setTimer(message.time + ("duration" in message ? message.duration : 5e3) - MistVideo2.api.currentTime * 1e3);
              MistUtil.event.removeListener(playing);
            });
            return;
          }
          textNode.nodeValue = "";
        }, delay);
      }
      setTimer("duration" in message ? message.duration : 5e3);
    }
    MistUtil.event.addListener(MistVideo2.video, "seeked", function() {
      textNode.nodeValue = "";
      if (timer) {
        MistVideo2.timers.stop(timer);
      }
      timer = null;
    });
    if (!("setWSSubtitle" in MistVideo2.api)) {
      var trackid = false;
      MistVideo2.player.api.setWSSubtitle = function(id) {
        if (id == trackid) {
          return;
        }
        if (typeof id != "undefined") {
          MistVideo2.metaTrackSubscriptions.add(id, displayMessage);
        }
        if (id != trackid) {
          MistVideo2.metaTrackSubscriptions.remove(trackid, displayMessage);
        }
        trackid = id == "undefined" ? false : id;
      };
    }
    return container;
  },
  chromecast: function() {
    var MistVideo2 = this;
    if (!"".indexOf || MistVideo2.options.host.indexOf("localhost") > -1 || MistVideo2.options.host.indexOf("::1") > -1) {
      return;
    }
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
      loop: MistVideo2.player.api ? MistVideo2.player.api.loop : MistVideo2.options.loop
    };
    MistVideo2.casting = false;
    function onCastLoad(tries) {
      if (!window.chrome || !window.chrome.cast || !window.chrome.cast.isAvailable || tries > 5) {
        if (ele.parentNode) {
          ele.parentNode.removeChild(ele);
        }
        MistVideo2.log("Chromecast is not supported");
        return;
      }
      if (!window.cast) {
        if (!tries) {
          tries = 0;
        }
        MistVideo2.log("Casting api loaded but cast function not yet available, retrying..");
        MistVideo2.timers.start(function() {
          onCastLoad(tries++);
        }, 200);
        return;
      }
      MistVideo2.log("Chromecast API loaded");
      if (!window.loadedCastApi || window.loadedCastApi == "loading") {
        window.loadedCastApi = true;
      }
      var cast_ele = document.createElement("google-cast-launcher");
      cast_ele.setAttribute("role", "button");
      cast_ele.setAttribute("tabindex", "0");
      cast_ele.setAttribute("aria-label", MistVideo2.translate("chromecast"));
      ele.appendChild(cast_ele);
      cast.framework.CastContext.getInstance().setOptions({
        receiverApplicationId: "E5F1558C",
        autoJoinPolicy: chrome.cast.AutoJoinPolicy.ORIGIN_SCOPED
      });
      function detachFromCast(keepSession) {
        MistUtil.class.remove(cast_ele, "active");
        MistUtil.class.remove(MistVideo2.container, "casting");
        if (casting_ele.parentNode) {
          casting_ele.parentNode.removeChild(casting_ele);
        }
        if (!keepSession && cast.framework.CastContext.getInstance().getCurrentSession()) {
          cast.framework.CastContext.getInstance().getCurrentSession().endSession(true);
        }
        if (api) {
          MistVideo2.player.api = api;
          api.currentTime = remoteData.currentTime;
          api.play();
          MistVideo2.reload = reload;
          MistVideo2.nextCombo = nextCombo;
          MistVideo2.unload = unload;
        } else {
          MistVideo2.player.api.play();
        }
        if (MistVideo2.player.api && MistVideo2.player.api.setTracks && MistUtil.object.keys(selectedTracks).length) {
          MistVideo2.player.api.setTracks(selectedTracks);
        }
        MistVideo2.casting = false;
        MistVideo2.log("Detached chromecast session");
      }
      MistVideo2.detachFromCast = detachFromCast;
      cast_ele.addEventListener("click", function(e) {
        e.stopPropagation();
        if (MistUtil.class.has(cast_ele, "active")) {
          detachFromCast();
        } else {
          let loadStream = function() {
            cast.framework.CastContext.getInstance().getCurrentSession().addMessageListener("urn:x-cast:mistcaster", function(ns, message) {
              if (MistVideo2.destroyed) {
                detachFromCast();
              }
              message = JSON.parse(message);
              if (message.type) {
                switch (message.type) {
                  case "log":
                  case "error": {
                    MistVideo2.log("[Chromecast] " + message.message, message.type);
                    break;
                  }
                  case "showError": {
                    MistVideo2.showError.apply(MistVideo2, message.args);
                    break;
                  }
                  case "event": {
                    switch (message.event) {
                      case "timeupdate": {
                        remoteData.currentTime = message.currentTime;
                        MistUtil.event.send(message.event, "chromecast", MistVideo2.video);
                        break;
                      }
                      case "progress": {
                        remoteData.buffer = message.buffer;
                        MistUtil.event.send(message.event, "chromecast", MistVideo2.video);
                        break;
                      }
                      case "pause":
                      case "paused":
                      case "ended":
                      case "play":
                      case "playing": {
                        remoteData.paused = message.paused;
                        MistUtil.event.send(message.event, "chromecast", MistVideo2.video);
                        break;
                      }
                      case "volumechange": {
                        remoteData.volume = message.volume;
                        remoteData.muted = message.muted;
                        MistUtil.event.send(message.event, "chromecast", MistVideo2.video);
                        break;
                      }
                      default: {
                        MistUtil.event.send(message.event, "chromecast", MistVideo2.video);
                      }
                    }
                    break;
                  }
                  case "detach": {
                    if (message.n == MistVideo2.n) {
                      detachFromCast(true);
                    }
                    break;
                  }
                  default: {
                    console.log("Unknown chromecast message type", message);
                  }
                }
              }
            });
            var d = {
              type: "load",
              n: MistVideo2.n,
              //indentify which player instance we are
              options: {
                host: MistVideo2.options.host,
                loop: MistVideo2.options.loop,
                //will be overwritten with the current value if there is a player api
                poster: MistVideo2.options.poster,
                //should be an absolute url, because the location will be different
                streaminfo: MistVideo2.options.streaminfo,
                urlappend: MistVideo2.options.urlappend,
                forcePriority: MistVideo2.options.forcePriority,
                setTracks: MistVideo2.options.setTracks,
                //when the track selection is changed through the UI, the selected track is saved in the options, so this passes on the currently enforced tracks
                controls: false,
                skin: "default"
                //TODO: right now the skin can't really be transferred because there are functions in there that won't be in the JSON. At some point we should fix this, probably by having the Mist backend include a custom skin definition with the player code.
              },
              stream: MistVideo2.stream
            };
            if (MistVideo2.info && MistVideo2.info.type != "live") {
              d.time = MistVideo2.player.api.currentTime;
            }
            if (MistVideo2.options.skin == "dev") {
              d.options.skin = MistVideo2.options.skin;
            }
            if (MistVideo2.player && MistVideo2.player.api) {
              d.volume = MistVideo2.player.api.volume;
              d.muted = MistVideo2.player.api.muted;
              d.options.loop = MistVideo2.player.api.loop;
            }
            MistCast.send(d);
            api = MistVideo2.player.api;
            reload = MistVideo2.reload;
            nextCombo = MistVideo2.nextCombo;
            unload = MistVideo2.unload;
            selectedTracks = MistVideo2.options.setTracks ? MistVideo2.options.setTracks : {};
            MistVideo2.player.api = new Proxy(api, {
              get: function(target, key, receiver) {
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
                    return new function() {
                      this.length = remoteData.buffer.length;
                      this.start = function(i2) {
                        return remoteData.buffer[i2][0];
                      };
                      this.end = function(i2) {
                        return remoteData.buffer[i2][1];
                      };
                    }();
                  }
                  case "setTracks":
                  case "play":
                  case "pause": {
                    return function() {
                      var a = [];
                      for (var i2 = 0; i2 < arguments.length; i2++) {
                        a.push(arguments[i2]);
                      }
                      if (key == "setTracks") {
                        MistUtil.object.extend(selectedTracks, arguments[0]);
                      }
                      MistCast.send({ type: "cmd", cmd: key, args: a });
                    };
                  }
                }
                return property;
              },
              set: function(target, key, value) {
                if (key == "loop") {
                  remoteData[key] = value;
                }
                MistCast.send({ type: "set", cmd: key, value });
                return true;
              }
            });
            api.pause();
            MistVideo2.reload = function() {
              MistCast.send({ type: "reload" });
            };
            MistVideo2.nextCombo = function() {
              MistCast.send({ type: "nextCombo" });
            };
            MistVideo2.unload = function() {
              detachFromCast();
              return unload.apply(this, arguments);
            };
            var d = cast.framework.CastContext.getInstance().getCurrentSession().getCastDevice();
            if (d && d.friendlyName) {
              casting_ele.innerHTML = MistVideo2.translate("casting to") + " " + d.friendlyName;
            }
            var on_session_end = function() {
              if (cast.framework.CastContext.getInstance().getSessionState() == "SESSION_ENDED") {
                if (MistUtil.class.has(cast_ele, "active")) {
                  detachFromCast();
                }
                cast.framework.CastContext.getInstance().removeEventListener(cast.framework.CastContextEventType.SESSION_STATE_CHANGED, on_session_end);
              }
            };
            cast.framework.CastContext.getInstance().addEventListener(cast.framework.CastContextEventType.SESSION_STATE_CHANGED, on_session_end);
            MistVideo2.log("Attached chromecast session");
          };
          MistUtil.class.add(cast_ele, "active");
          casting_ele.innerHTML = MistVideo2.translate("select cast device");
          MistVideo2.container.appendChild(casting_ele);
          MistUtil.class.add(MistVideo2.container, "casting");
          MistVideo2.log("chromecast: pausing player");
          MistVideo2.player.api.pause();
          MistVideo2.container.setAttribute("data-loading", "waiting for cast");
          MistVideo2.casting = true;
          if (!window.MistCast) {
            window.MistCast = {
              send: function(obj) {
                cast.framework.CastContext.getInstance().getCurrentSession().sendMessage("urn:x-cast:mistcaster", JSON.stringify(obj));
              }
            };
          }
          if (cast.framework.CastContext.getInstance().getCurrentSession()) {
            loadStream();
          } else {
            cast.framework.CastContext.getInstance().requestSession().then(function() {
              if (!cast.framework.CastContext.getInstance().getCurrentSession()) {
                throw "Could not connect to the cast device";
              }
              loadStream();
            }, function(e2) {
              MistVideo2.log("Chromecast session ended: " + e2);
              detachFromCast();
            });
          }
        }
      }, true);
    }
    if ((!window.chrome || !window.chrome.cast) && !window.loadedCastApi) {
      window["__onGCastApiAvailable"] = function(loaded, errorInfo) {
        if (!loaded) {
          MistVideo2.log("Error while loading chromecast API: " + errorInfo);
        }
        onCastLoad();
      };
      window.loadedCastApi = "loading";
      var script = document.createElement("script");
      script.setAttribute("src", "//www.gstatic.com/cv/js/sender/v1/cast_sender.js?loadCastFramework=1");
      document.head.appendChild(script);
      MistVideo2.log("Appending chromecast script");
    } else {
      if (window.loadedCastApi == "loading") {
        MistVideo2.log("Not appending chromecast script - still loading");
        MistVideo2.timers.start(function() {
          onCastLoad();
        }, 200);
      } else {
        MistVideo2.log("Not appending chromecast script - already loaded");
        onCastLoad();
      }
    }
    return ele;
  },
  airplay: function() {
    if (!window.WebKitPlaybackTargetAvailabilityEvent) {
      return;
    }
    if (!this.video) {
      return;
    }
    var MistVideo2 = this;
    var ele = document.createElement("div");
    ele.style.display = "none";
    var button = this.skin.icons.build("airplay");
    button.setAttribute("role", "button");
    button.setAttribute("tabindex", "0");
    button.setAttribute("aria-label", MistVideo2.translate("airplay", "AirPlay"));
    MistUtil.controlTooltip(button, MistVideo2.translate("airplay", "AirPlay"));
    ele.appendChild(button);
    MistVideo2.video.addEventListener("webkitplaybacktargetavailabilitychanged", function(e) {
      if (e.availability === "available") {
        ele.style.display = "";
      } else {
        ele.style.display = "none";
      }
    });
    MistUtil.event.addListener(button, "click", function() {
      MistVideo2.video.webkitShowPlaybackTargetPicker();
    });
    return ele;
  },
  keyControls: function() {
    var MistVideo2 = this;
    if (!MistVideo2.options.keyControls) return;
    if (!MistVideo2.api) return;
    var api = MistVideo2.api;
    var defaultKeyMap = {
      togglePlay: ["k", "K", " "],
      seekForward: ["l", "L", "ArrowRight"],
      seekBackward: ["j", "J", "ArrowLeft"],
      volumeUp: ["ArrowUp", "+", "="],
      volumeDown: ["ArrowDown", "-", "_"],
      toggleFullscreen: ["f", "F"],
      toggleMute: ["m", "M"],
      togglePip: ["i", "I"],
      cycleSubtitle: ["c", "C"],
      speedUp: [">", "."],
      speedDown: ["<", ","],
      seekToStart: ["Home"],
      seekToEnd: ["End"],
      seekPercent: ["0", "1", "2", "3", "4", "5", "6", "7", "8", "9"]
    };
    var keyMap = {};
    for (var action in defaultKeyMap) {
      if (MistVideo2.options.keyMap && action in MistVideo2.options.keyMap) {
        keyMap[action] = MistVideo2.options.keyMap[action];
      } else {
        keyMap[action] = defaultKeyMap[action];
      }
    }
    function matchesAction(key, action2) {
      var keys = keyMap[action2];
      if (!keys) return false;
      for (var i2 = 0; i2 < keys.length; i2++) {
        if (keys[i2] === key) return true;
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
    function show(text, icon) {
      if (iconCon.children.length) iconCon.removeChild(iconCon.children[0]);
      if (icon) {
        var i2 = MistVideo2.skin.icons.build(icon, 80);
        iconCon.appendChild(i2);
      }
      textEle.nodeValue = text;
      ele.setAttribute("data-show", "");
      setTimeout(function() {
        ele.removeAttribute("data-show");
      }, 500);
    }
    function getDisplayVolume() {
      return Math.round((1 - Math.pow(1 - api.volume, 2)) * 100) + "%";
    }
    function snapRate(rate) {
      var speeds = [0.25, 0.5, 0.75, 1, 1.5, 2, 3, 5];
      var index = speeds.length - 1;
      for (var i2 in speeds) {
        var speed = speeds[i2];
        if (rate < speed) {
          if (i2 > 0 && speed - rate > rate - speeds[i2 - 1]) {
            index = i2 - 1;
          } else {
            index = i2;
          }
          break;
        }
      }
      return { index, rate: speeds[index], speeds };
    }
    var keepTimer = false;
    function showControls(keep) {
      if (MistVideo2.container) {
        MistVideo2.container.setAttribute("data-controls", "show");
        if (keepTimer) {
          MistVideo2.timers.stop(keepTimer);
        }
        if (!keep) {
          MistVideo2.timers.start(function() {
            MistVideo2.container.removeAttribute("data-controls");
          }, 3e3);
        }
      }
    }
    var holdingspace = false;
    MistUtil.event.addListener(MistVideo2.options.keyControls == "focus" ? MistVideo2.container : document.body, "keydown", function(e) {
      if (MistVideo2.destroyed) {
        return;
      }
      if (e.target.matches('input,textarea,select,[contenteditable]:not([contenteditable="false"])')) {
        return;
      }
      if (matchesAction(e.key, "togglePlay")) {
        if (e.key === " ") {
          e.preventDefault();
          if (holdingspace) {
            return;
          }
          holdingspace = true;
          var speeding = false;
          var timer = false;
          var keyup = MistUtil.event.addListener(document.body, "keyup", function(e2) {
            if (e2.key == " ") {
              holdingspace = false;
              MistUtil.event.removeListener(keyup);
              if (timer) MistVideo2.timers.stop(timer);
              if (speeding) {
                api.playbackRate = speeding;
                show(MistVideo2.translate("speed") + ": " + Math.round(speeding * 10) / 10 + "x", "play");
                speeding = false;
              } else {
                if (api.paused) {
                  api.play();
                  show("", "play");
                } else {
                  api.pause();
                  show("", "pause");
                }
              }
              e2.preventDefault();
            }
          }, ele);
          if (!(MistVideo2.info && MistVideo2.info.type == "live")) {
            timer = MistVideo2.timers.start(function() {
              speeding = api.playbackRate;
              api.playbackRate = 2 * speeding;
              if (api.paused) api.play();
              show(MistVideo2.translate("speed doubled"), "forward");
            }, 200);
          }
        } else {
          if (api.paused) {
            api.play();
            show("", "play");
          } else {
            api.pause();
            show("", "pause");
          }
          e.preventDefault();
        }
      } else if (matchesAction(e.key, "toggleFullscreen")) {
        var button = MistVideo2.container.querySelector(".mistvideo-fullscreen");
        if (button) {
          var wasFullscreen = MistVideo2.container.hasAttribute("data-fullscreen");
          MistUtil.event.send("click", null, button);
          show((wasFullscreen ? "Exit" : "Enable") + " Full Screen");
        }
        e.preventDefault();
      } else if (matchesAction(e.key, "toggleMute")) {
        api.muted = !api.muted;
        if (!api.muted && api.volume == 0) api.volume = 0.29;
        if (api.muted) {
          show(MistVideo2.translate("muted"), "muted");
        } else {
          show(MistVideo2.translate("volume") + ": " + getDisplayVolume(), "unmuted");
        }
        e.preventDefault();
      } else if (matchesAction(e.key, "volumeUp")) {
        var current = 1 - Math.pow(1 - api.volume, 2);
        var target = Math.min(1, Math.round((current + 0.1) * 10) / 10);
        api.volume = Math.min(1, 1 - Math.pow(1 - target, 0.5));
        if (api.muted) api.muted = false;
        show(MistVideo2.translate("volume") + ": " + getDisplayVolume(), "unmuted");
        e.preventDefault();
      } else if (matchesAction(e.key, "volumeDown")) {
        var current = 1 - Math.pow(1 - api.volume, 2);
        var target = Math.round((current - 0.1) * 10) / 10;
        api.volume = Math.max(0, 1 - Math.pow(1 - target, 0.5));
        show(MistVideo2.translate("volume") + ": " + getDisplayVolume() + (api.muted ? "\n" + MistVideo2.translate("(muted)") : ""), api.muted || api.volume <= 0 ? "muted" : "unmuted");
        e.preventDefault();
      } else if (matchesAction(e.key, "cycleSubtitle")) {
        var select = MistVideo2.container.querySelector('.mistvideo-tracks select[data-type="subtitle"]');
        if (select) {
          var current = select.querySelector('option[value="' + select.value + '"]');
          var index = Array.from(select.children).indexOf(current);
          if (!e.shiftKey) {
            index++;
            if (index >= select.children.length) index = 0;
          } else {
            index--;
            if (index < 0) index = select.children.length - 1;
          }
          var next = select.children[index];
          select.value = next.value;
          show(select.value == "" ? "Hide subtitles" : "Subtitle: " + next.innerHTML);
          MistUtil.event.send("change", select.value, select);
          e.preventDefault();
        }
      } else if (matchesAction(e.key, "seekBackward")) {
        if (MistVideo2.info && MistVideo2.info.type == "live") {
          return;
        }
        api.currentTime -= 10;
        show(MistVideo2.translate("seek backward seconds"), "left");
        showControls();
        e.preventDefault();
      } else if (matchesAction(e.key, "seekForward")) {
        if (MistVideo2.info && MistVideo2.info.type == "live") {
          return;
        }
        api.currentTime += 10;
        show(MistVideo2.translate("seek forward seconds"), "right");
        showControls();
        e.preventDefault();
      } else if (matchesAction(e.key, "seekToStart")) {
        api.currentTime = 0;
        show(MistVideo2.translate("to start"), "left");
        showControls();
        e.preventDefault();
      } else if (matchesAction(e.key, "seekToEnd")) {
        api.currentTime = api.duration - (MistVideo2.info && MistVideo2.info.type == "live" ? 0 : 3);
        show(MistVideo2.translate("to end"), "right");
        showControls();
        e.preventDefault();
      } else if (matchesAction(e.key, "speedUp")) {
        if (e.key === "." && api.paused && !e.shiftKey) {
          api.currentTime += 1 / 30;
          show(MistVideo2.translate("frame forward"), "forward");
          showControls();
        } else {
          if (MistVideo2.info && MistVideo2.info.type == "live") {
            return;
          }
          var snap = snapRate(api.playbackRate);
          var set = api.playbackRate;
          if (snap.index + 1 < snap.speeds.length) {
            set = snap.speeds[snap.index + 1];
            api.playbackRate = set;
            if (api.paused) api.play();
          }
          show(MistVideo2.translate("speed") + ": " + set + "x", "forward");
        }
        e.preventDefault();
      } else if (matchesAction(e.key, "speedDown")) {
        if (e.key === "," && api.paused && !e.shiftKey) {
          api.currentTime -= 1 / 30;
          show(MistVideo2.translate("frame backward"), "backward");
          showControls();
        } else {
          if (MistVideo2.info && MistVideo2.info.type == "live") {
            return;
          }
          var snap = snapRate(api.playbackRate);
          var set = api.playbackRate;
          if (snap.index > 0) {
            set = snap.speeds[snap.index - 1];
            api.playbackRate = set;
          }
          show(MistVideo2.translate("speed") + ": " + set + "x", "backward");
        }
        e.preventDefault();
      } else if (matchesAction(e.key, "togglePip")) {
        var button = MistVideo2.container.querySelector(".mistvideo-picture-in-picture");
        if (button) {
          MistUtil.event.send("click", null, button);
          e.preventDefault();
        }
      } else if (matchesAction(e.key, "seekPercent")) {
        if (MistVideo2.info && MistVideo2.info.type == "live") {
          return;
        }
        if (!isFinite(api.duration)) {
          return;
        }
        var perc = parseInt(e.key) / 10;
        api.currentTime = perc * api.duration;
        show(Math.round(perc * 100) + "%");
        showControls();
        e.preventDefault();
      }
    }, ele);
    return ele;
  }
};

// src/ui/blueprints/context-menu.js
var contextMenuBlueprints = {
  contextMenu: function() {
    var MistVideo2 = this;
    var container = document.createElement("div");
    container.className = "mistvideo-context-menu";
    container.setAttribute("role", "menu");
    container.style.display = "none";
    var speeds = [0.25, 0.5, 0.75, 1, 1.25, 1.5, 2, 3, 5];
    var itemElements = [];
    function hide() {
      container.style.display = "none";
      itemElements = [];
    }
    function addItem(opts) {
      var el = document.createElement("div");
      el.className = "mistvideo-context-menu-item" + (opts.className ? " " + opts.className : "");
      el.setAttribute("role", "menuitem");
      el.setAttribute("tabindex", "0");
      if (opts.disabled) el.setAttribute("data-disabled", "");
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
        el.addEventListener("click", function(e) {
          e.stopPropagation();
          opts.action();
          if (!opts.keepOpen) hide();
        });
      }
      el.addEventListener("keydown", function(e) {
        var idx = itemElements.indexOf(el);
        if (e.key === "ArrowDown" && idx < itemElements.length - 1) {
          itemElements[idx + 1].focus();
          e.preventDefault();
        } else if (e.key === "ArrowUp" && idx > 0) {
          itemElements[idx - 1].focus();
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
      sep.setAttribute("role", "separator");
      container.appendChild(sep);
    }
    function buildSpeedFlyout(parentItem) {
      var flyout = document.createElement("div");
      flyout.className = "mistvideo-context-menu-flyout";
      flyout.setAttribute("role", "menu");
      var currentRate = MistVideo2.api.playbackRate || 1;
      for (var i2 = 0; i2 < speeds.length; i2++) {
        (function(speed) {
          var item = document.createElement("div");
          item.className = "mistvideo-context-menu-item mistvideo-context-menu-flyout-item";
          item.setAttribute("role", "menuitem");
          item.setAttribute("tabindex", "0");
          var check = document.createElement("span");
          check.className = "mistvideo-context-menu-check";
          check.textContent = Math.abs(currentRate - speed) < 0.01 ? "\u2713" : "";
          item.appendChild(check);
          var label = document.createElement("span");
          label.className = "mistvideo-context-menu-label";
          label.textContent = speed === 1 ? MistVideo2.translate("normal") : speed + "x";
          item.appendChild(label);
          item.addEventListener("click", function(e) {
            e.stopPropagation();
            MistVideo2.api.playbackRate = speed;
            hide();
          });
          flyout.appendChild(item);
        })(speeds[i2]);
      }
      parentItem.appendChild(flyout);
      parentItem.addEventListener("mouseenter", function() {
        flyout.style.display = "";
      });
      parentItem.addEventListener("mouseleave", function() {
        flyout.style.display = "none";
      });
      flyout.style.display = "none";
    }
    function build() {
      MistUtil.empty(container);
      itemElements = [];
      var isLive = MistVideo2.info && MistVideo2.info.type === "live";
      var hasApi = MistVideo2.player && MistVideo2.player.api;
      var hadItems = false;
      var playbackGroup = false;
      if (!isLive && hasApi && "loop" in MistVideo2.player.api) {
        addItem({
          label: MistVideo2.translate("loop"),
          check: MistVideo2.api.loop,
          shortcut: "L",
          action: function() {
            MistVideo2.api.loop = !MistVideo2.api.loop;
          }
        });
        playbackGroup = true;
      }
      if (!isLive && hasApi && "playbackRate" in MistVideo2.player.api) {
        var speedItem = addItem({
          label: MistVideo2.translate("playback speed"),
          arrow: true,
          keepOpen: true,
          action: function() {
          }
          // flyout handles it
        });
        buildSpeedFlyout(speedItem);
        playbackGroup = true;
      }
      if (playbackGroup) hadItems = true;
      var featuresGroup = false;
      if (MistVideo2.pip && MistVideo2.pip.supported) {
        if (hadItems && !featuresGroup) addSeparator();
        addItem({
          label: MistVideo2.translate("picture-in-picture"),
          shortcut: "I",
          action: function() {
            MistVideo2.pip.toggle();
          }
        });
        featuresGroup = true;
      }
      var castBtn = MistVideo2.container.querySelector("google-cast-launcher");
      if (castBtn) {
        if (hadItems && !featuresGroup) addSeparator();
        addItem({
          label: "Chromecast",
          action: function() {
            castBtn.click();
          }
        });
        featuresGroup = true;
      }
      var airplayBtn = MistVideo2.container.querySelector(".mistvideo-airplay");
      if (airplayBtn) {
        if (hadItems && !featuresGroup) addSeparator();
        addItem({
          label: "AirPlay",
          action: function() {
            airplayBtn.click();
          }
        });
        featuresGroup = true;
      }
      if (featuresGroup) hadItems = true;
      if (MistVideo2.source && MistVideo2.source.url) {
        if (hadItems) addSeparator();
        addItem({
          label: MistVideo2.translate("copy video url"),
          action: function() {
            if (navigator.clipboard && navigator.clipboard.writeText) {
              navigator.clipboard.writeText(MistVideo2.source.url);
            }
          }
        });
        hadItems = true;
      }
      if (hadItems) addSeparator();
      var brand = document.createElement("div");
      brand.className = "mistvideo-context-menu-brand";
      brand.textContent = "MistPlayer";
      brand.setAttribute("role", "none");
      container.appendChild(brand);
    }
    function show(e) {
      e.preventDefault();
      e.stopPropagation();
      build();
      container.style.display = "";
      var rect = MistVideo2.container.getBoundingClientRect();
      var x = e.clientX - rect.left;
      var y = e.clientY - rect.top;
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
    MistUtil.event.addListener(MistVideo2.options.target, "playerUpdate_videoElement", function() {
      if (MistVideo2.video) {
        MistUtil.event.addListener(MistVideo2.video, "contextmenu", show, container);
      }
    }, container);
    if (MistVideo2.video) {
      MistUtil.event.addListener(MistVideo2.video, "contextmenu", show, container);
    }
    document.addEventListener("click", function(e) {
      if (container.style.display !== "none" && !container.contains(e.target)) {
        hide();
      }
    });
    document.addEventListener("keydown", function(e) {
      if (e.key === "Escape" && container.style.display !== "none") {
        hide();
      }
    });
    return container;
  }
};

// src/ui/blueprints/idle-screen.js
var MIST_LOGO_SVG = '<svg viewBox="0 0 202.52 150" xmlns="http://www.w3.org/2000/svg" preserveAspectRatio="xMidYMid meet"><path d="m53.406 10.557-5.0488 3.3223 42.061 63.951c-1.0569 1.2973-1.8786 2.7896-2.418 4.4121h-70.115v6.0449h69.498c0.81169 6.9508 6.713 12.344 13.879 12.344 7.1658 0 13.064-5.393 13.875-12.344h69.5v-6.0449h-70.117c-0.53924-1.6225-1.3594-3.1148-2.416-4.4121l42.061-63.951-5.0508-3.3223-41.777 63.518c-1.8391-0.89026-3.894-1.4043-6.0742-1.4043-2.1814 0-4.238 0.51505-6.0781 1.4062z" class="mist-logo-highlight"/><path d="m49.875 0c-7.0953 0-12.848 5.7519-12.848 12.846 0 3.7198 1.5899 7.0604 4.1152 9.4062l-20.535 46.447c-1.0469-0.19686-2.1225-0.31055-3.2266-0.31055-9.5991 0-17.381 7.783-17.381 17.381 0 9.5997 7.7817 17.381 17.381 17.381 5.3445 0 10.122-2.4174 13.311-6.2129l54.49 29.096c-0.83369 2.0328-1.3008 4.2547-1.3008 6.5879 0 9.5997 7.7811 17.379 17.379 17.379 9.5997 0 17.381-7.7792 17.381-17.379 0-2.3333-0.46899-4.5551-1.3027-6.5879l54.492-29.098c3.1877 3.796 7.963 6.2148 13.309 6.2148 9.5997 0 17.381-7.7812 17.381-17.381 0-9.5978-7.7812-17.381-17.381-17.381-1.1042 0-2.1796 0.11369-3.2266 0.31055l-20.535-46.447c2.5256-2.3459 4.1152-5.6861 4.1152-9.4062 0-7.0938-5.7519-12.846-12.846-12.846-5.9165 0-10.885 4.0056-12.377 9.4473h-78.018c-1.4922-5.4418-6.4613-9.4473-12.377-9.4473zm13.406 15.492h75.957l-24.789 11.568c-3.1173-3.8293-7.8647-6.2793-13.188-6.2793-5.3228 0-10.072 2.45-13.189 6.2793zm-3.498 5.5312 24.967 12.77c-0.30913 1.2822-0.49218 2.6131-0.49218 3.9902 0 1.6082 0.23907 3.1582 0.65625 4.6328l-54.004 32.453c-1.2919-1.6017-2.8674-2.9575-4.6426-4.0176l20.174-45.635c1.0944 0.30322 2.2426 0.47851 3.4336 0.47851 3.9887 0 7.552-1.819 9.9082-4.6719zm82.953 0c2.3561 2.8528 5.9208 4.6719 9.9102 4.6719 1.1903 0 2.3377-0.17551 3.4316-0.47851l20.176 45.635c-1.7754 1.0602-3.3507 2.4157-4.6426 4.0176l-54.004-32.453c0.41706-1.4746 0.6543-3.0247 0.6543-4.6328 0-1.377-0.18122-2.7081-0.49024-3.9902zm-54.041 28.186c2.2766 2.5025 5.2868 4.3206 8.6894 5.1211v61.355c-3.5044 0.80386-6.605 2.6644-8.9492 5.2324l-54.734-29.225c0.67236-1.8515 1.0605-3.8399 1.0605-5.9238 0-1.8275-0.28752-3.5875-0.81055-5.2422zm25.131 2e-3 54.742 31.316c-0.52293 1.6547-0.80859 3.4147-0.80859 5.2422 0 2.0838 0.38643 4.0724 1.0586 5.9238l-54.734 29.225c-2.3448-2.5682-5.4441-4.4287-8.9492-5.2324v-61.354c3.4038-0.79961 6.4143-2.6181 8.6914-5.1211z" class="mist-logo-base"/></svg>';
var DVD_ASPECT = 202.52 / 150;
var PARTICLE_COLORS_DEFAULT = [
  "#7aa2f7",
  "#bb9af7",
  "#9ece6a",
  "#73daca",
  "#7dcfff",
  "#f7768e",
  "#e0af68",
  "#2ac3de"
];
var BUBBLE_COLORS_DEFAULT = [
  "rgba(122,162,247,0.2)",
  "rgba(187,154,247,0.2)",
  "rgba(158,206,106,0.2)",
  "rgba(115,218,202,0.2)",
  "rgba(125,207,255,0.2)",
  "rgba(247,118,142,0.2)",
  "rgba(224,175,104,0.2)",
  "rgba(42,195,222,0.2)"
];
var HITMARKER_AUDIO_DATA_URL = "data:audio/mpeg;base64,SUQzBAAAAAAANFRDT04AAAAHAAADT3RoZXIAVFNTRQAAAA8AAANMYXZmNTcuODMuMTAwAAAAAAAAAAAAAAD/+1QAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAABJbmZvAAAADwAAAAYAAAnAADs7Ozs7Ozs7Ozs7Ozs7OztiYmJiYmJiYmJiYmJiYmJiYomJiYmJiYmJiYmJiYmJiYmxsbGxsbGxsbGxsbGxsbGxsdjY2NjY2NjY2NjY2NjY2NjY/////////////////////wAAAABMYXZjNTcuMTAAAAAAAAAAAAAAAAAkAkAAAAAAAAAJwOuMZun/+5RkAA8S/F23AGAaAi0AF0AAAAAInXsEAIRXyQ8D4OQgjEhE3cO7ujuHF0XCOu4G7xKbi3Funu7u7p9dw7unu7u7p7u7u6fXcW7om7u7uiU3dxdT67u7p7uHdxelN3cW6fXcW7oXXd3eJTd3d0+u4t3iXdw4up70W4uiPruLDzMw8Pz79Y99JfkyfPv5/h9uTJoy79Y99Y97q3vyZPJk0ZfrL6x73Vn+J35dKKS/STQyQ8CAiCPNuRAOOqquAx+fzJeBKDAsgAMBuWcBsHKhjJTcCwIALyAvABbI0ZIcCmP8jHJe8gZAdVRp2TpnU/kUXV4iQuBAAkAQgisLPvwQ2Jz7wIkIpQ8QOl/KFy75w+2HpTFnRqXLQo0fzlSYRe5Ce9yZMEzRM4xesu95Mo8QQsoMH4gLg+fJqkmY3GZJE2kwGfMECJiAdIttoEa2yotfC7jsS2mjKgbzAfEMeiwZpGSUFCQwPKQiWXh0TnkNor5SmrKvwHlX2zFxKxPCzRL/+5RkIwADvUxLawwb0GdF6Y1hJlgNNJk+DSRwyQwI6AD2JCiBmhaff0dzCEBjgFABAcDNFc3YAEV4hQn0L/QvQnevom+n13eIjoTvABLrHg/L9RzdWXYonHbbbE2K0pX+gkL2g56RiwrbuWwhoABzQoMKOAIGAfE4UKk6BhSIJpECBq0CEYmZKYIiAJt72H24dNou7y/Ee7a/3v+MgySemSTYmnBAFwIAAGfCJ8/D9YfkwQEBcP38uA1d/EB1T5dZKEsgnuhwZirY5fIMRMdRn7U4OcN2m5NWeYdcPBwXDBOsJF1DBYks62pAURqz1hGoGHH/QIoRC80tYAJ8g4f3MPD51sywAbhAn/X9P/75tvZww3gZ3pYPDx/+ACO/7//ffHj/D/AAfATC4DYGFA3MRABo0lqWjBOl2yAda1C1BdhduXgm8FGnAQB/lDiEi6j9qw9EHigIIOLB6F1eIPd+T6Agc4//lMo6+k3tdttJY2gArU7cN07m2FLSm4gCjyz/+5RECwACwSRZawkdLFGi2mVh5h4LfFdPVPGACViTavaeMAAV0UkkEsDhxxJwqF04on002mZah8w9+5ItfSAoyZa1dchnPpLmAEKrVMRA//sD8w0WsB4xiw4JqaZMB45TdpIuXXUPf8Bpa35p/jQIAOAuZkmUeJoM5W6L2gqqO6rTuHjUTDnhy4QiK348vtFysOizShoHbBpsPRYcSINCbiN4XOLPPAgq3dW2Ga7SlyiKXBV7W1RQl5BiiVGkwayJfEnPxgXkQeZxxzyhTuLO2XFUDDstoc6CkM1J8QZAjUN3bM8580cRygNfmPAELGjIH0Z/0A+8csyH/4eHvgAf8APgABmZ98AARAADP////Dw8PHEmIpgGttpJQJsmZjq5nPQ8j5VqWW1evqdjP182PA6tHJZgkC5iSbEQkyJSz/BvP3eucLKN0+Wiza4feKKFBqiAEBAMXyYni5NZc16CDl/QY9j6BAcWSmQYcIcoMHYoQNBiIBgIBUAzQUMSnjj/+5RkCwADsFLffjEAAjrJe63JHACO6WtlnPMACKaCK1uMMADU5dI6JhW2cam98UlRmY4ihyKFrNsgpZd5PYgBALnYofKEt82De0GbW1DLibvFDK+bSeOm8qKdqUFZ7uiK8XMPHyqm3pTxUvcunUfxXEo9RNe5b/8vfCD3kzDN7vTtHyaIcntVDAYBAUBAAAAQBI2vguYNsHWm5AR3mZtZib8WAHFvz2Kf9//iYvlRB/+n///////////+UH7XoIDMoJAEAMtj8JshJPRwklVqNSpYnalfE+VzNCAISCoxVHEpIo/WrTiMvP7VTujOPnOglLbMLN/pq/d2Y4lRJIkSnPlUSJEjSKJqM41d88zWtMzP+fCOORmc9NeM+f1nnO//efM52/fG/ef385+5u+u1bRJkwU8FAkEItZpkRYeQYcAgZTEYlaZa2yROLeC0qdX73rZJJ/d2f6v6Or0u/+5FBYcng0MlCiQTR9GUU5LScmSuSlH00IWqXA6jlw4BEcD/+5REEAAi3RtU+eYbGF1E+lk9g0YJzLUgh7BlQVGTZJD0jKhhTNVilqrMzFRK+x/szcMKBWKep4NP1A0DR6RESkTp5Z1Q9Y8REgqMg1DpUBPleeqlRQcerBpMjiURHVD4XwAALhAgbxxlxYD5OFkG8oQRPB2EpsxSCNVlgcYUqoAyiVJmaARlkwplICfPoUy/zWEzM2pcNYzAQNJDSniEYecSEqxFEzQqEvUFGnvzwUfcRlpZ9T2LCR5QdDQDDhKICAjpJCagpRo9UQRPClZZlg6Ep9DMTkTl+okuhRIVIzAQEf9L+Mx/DUjqmqN6kX7M36lS4zgLyJV3iV6j3xF8kJduJawVw1nndAlBaLLgJupwsTcLkxmJgFLgSzoCmHjSNGSqkGPCpnNqTXIwolf6qlVWN+q/su37HzgrES1pWGg3KnWh0FXCVniJ9K5b4iCrpLEuIcFTqwkVLFiqgaDqCCSMVWqxBAVCFOLVrVahm2ahUThUKJnmFCw15hD0Qhb/+5REEAhCYSRCSQEb4FOGaBUMI6JIRYC0QIB2SQsgGpgwDghgIlS6FU8VBXDoiBp5Y9gtkVnhEhYBdJFQ7kQ3w1yp0NB2CoNPEttZ1/aeDUAAA26FEghWgEKNVAVWkFAQEmMK2Uwk/qI0hqUb/4epVIZH1ai6szf6kzH1f2arxYGS9FcOsN5UlJLQt///+oo0FRDTUQ0FBQr9f5LxXP+mEUfk0AIrf/5GRmQ0//mX//ZbLP5b5GrWSz+WSkZMrWyyyy2GRqyggVRyMv////////st//sn/yyVDI1l8mVgoYGDCOqiqIQBxmvxWCggTpZZZD//aWfyyWf/y/7KGDA0ssBggTof9k/+WS/8slQyMp/5Nfln8WAqGcUbULCrKxT9ISF+kKsxQWpMQU1FMy4xMDCqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqo=";
function createSyntheticHitmarkerSound() {
  try {
    var Ctor = window.AudioContext || window.webkitAudioContext;
    if (!Ctor) return;
    var ctx = new Ctor();
    var osc1 = ctx.createOscillator();
    var osc2 = ctx.createOscillator();
    var noiseBuf = ctx.createBuffer(1, ctx.sampleRate * 0.1, ctx.sampleRate);
    var noiseSrc = ctx.createBufferSource();
    var data = noiseBuf.getChannelData(0);
    for (var i2 = 0; i2 < data.length; i2++) data[i2] = Math.random() * 2 - 1;
    noiseSrc.buffer = noiseBuf;
    var g1 = ctx.createGain(), g2 = ctx.createGain(), ng = ctx.createGain(), mg = ctx.createGain();
    osc1.connect(g1);
    osc2.connect(g2);
    noiseSrc.connect(ng);
    g1.connect(mg);
    g2.connect(mg);
    ng.connect(mg);
    mg.connect(ctx.destination);
    var t = ctx.currentTime;
    osc1.frequency.setValueAtTime(1800, t);
    osc1.frequency.exponentialRampToValueAtTime(900, t + 0.08);
    osc2.frequency.setValueAtTime(3600, t);
    osc2.frequency.exponentialRampToValueAtTime(1800, t + 0.04);
    osc1.type = "triangle";
    osc2.type = "sine";
    g1.gain.setValueAtTime(0, t);
    g1.gain.linearRampToValueAtTime(0.4, t + 2e-3);
    g1.gain.exponentialRampToValueAtTime(1e-3, t + 0.12);
    g2.gain.setValueAtTime(0, t);
    g2.gain.linearRampToValueAtTime(0.3, t + 1e-3);
    g2.gain.exponentialRampToValueAtTime(1e-3, t + 0.06);
    ng.gain.setValueAtTime(0, t);
    ng.gain.linearRampToValueAtTime(0.2, t + 1e-3);
    ng.gain.exponentialRampToValueAtTime(1e-3, t + 0.01);
    mg.gain.setValueAtTime(0.5, t);
    osc1.start(t);
    osc2.start(t);
    noiseSrc.start(t);
    osc1.stop(t + 0.15);
    osc2.stop(t + 0.15);
    noiseSrc.stop(t + 0.02);
  } catch (e) {
  }
}
function playHitmarkerSound() {
  try {
    var audio = new Audio(HITMARKER_AUDIO_DATA_URL);
    audio.volume = 0.3;
    audio.play().catch(function() {
      createSyntheticHitmarkerSound();
    });
  } catch (e) {
    createSyntheticHitmarkerSound();
  }
}
function makeStatusIconSVG(type) {
  if (type === "loading") {
    return '<svg class="status-icon spin" fill="none" viewBox="0 0 24 24"><circle style="opacity:0.25" cx="12" cy="12" r="10" stroke="var(--mist-color-warning)" stroke-width="4"/><path style="opacity:0.75" fill="var(--mist-color-warning)" d="M4 12a8 8 0 018-8V0C5.373 0 0 5.373 0 12h4zm2 5.291A7.962 7.962 0 014 12H0c0 3.042 1.135 5.824 3 7.938l3-2.647z"/></svg>';
  }
  if (type === "offline") {
    return '<svg class="status-icon" fill="none" viewBox="0 0 24 24" stroke="var(--mist-color-danger)"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M18.364 5.636a9 9 0 010 12.728m0 0l-2.829-2.829m2.829 2.829L21 21M15.536 8.464a5 5 0 010 7.072m0 0l-2.829-2.829m-4.243 2.829a4.978 4.978 0 01-1.414-2.83m-1.414 5.658a9 9 0 01-2.167-9.238m7.824 2.167a1 1 0 111.414 1.414m-1.414-1.414L3 3m8.293 8.293l1.414 1.414"/></svg>';
  }
  if (type === "error") {
    return '<svg class="status-icon" fill="none" viewBox="0 0 24 24" stroke="var(--mist-color-danger)"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M12 9v2m0 4h.01m-6.938 4h13.856c1.54 0 2.502-1.667 1.732-3L13.732 4c-.77-1.333-2.694-1.333-3.464 0L3.34 16c-.77 1.333.192 3 1.732 3z"/></svg>';
  }
  return '<svg class="status-icon spin" fill="none" viewBox="0 0 24 24"><circle style="opacity:0.25" cx="12" cy="12" r="10" stroke="var(--mist-color-accent)" stroke-width="4"/><path style="opacity:0.75" fill="var(--mist-color-accent)" d="M4 12a8 8 0 018-8V0C5.373 0 0 5.373 0 12h4zm2 5.291A7.962 7.962 0 014 12H0c0 3.042 1.135 5.824 3 7.938l3-2.647z"/></svg>';
}
function classifyState(state) {
  if (!state) return "default";
  var s = state.toLowerCase();
  if (s.indexOf("offline") >= 0) return "offline";
  if (s.indexOf("initializing") >= 0 || s.indexOf("booting") >= 0 || s.indexOf("waiting") >= 0 || s.indexOf("shutting") >= 0) return "loading";
  if (s.indexOf("invalid") >= 0) return "error";
  if (s.indexOf("online") >= 0) return "online";
  return "default";
}
var idleScreenBlueprints = {
  idleScreen: function() {
    var MistVideo2 = this;
    var container = document.createElement("div");
    container.className = "mistvideo-idle";
    container.setAttribute("role", "status");
    container.setAttribute("aria-label", MistVideo2.translate("stream status"));
    var hitmarkerLayer = document.createElement("div");
    hitmarkerLayer.className = "mistvideo-idle-hitmarkers";
    container.appendChild(hitmarkerLayer);
    var particlesEl = document.createElement("div");
    particlesEl.className = "mistvideo-idle-particles";
    for (var i2 = 0; i2 < 12; i2++) {
      var p11 = document.createElement("div");
      p11.className = "particle";
      var pLeft = Math.random() * 100;
      var pSize = Math.random() * 4 + 2;
      var pDur = 8 + Math.random() * 4;
      var pDelay = Math.random() * 8;
      var pColor = PARTICLE_COLORS_DEFAULT[i2 % PARTICLE_COLORS_DEFAULT.length];
      p11.style.cssText = "--p-left:" + pLeft + "%;--p-size:" + pSize + "px;--p-dur:" + pDur + "s;--p-delay:" + pDelay + "s;--p-color:" + pColor;
      particlesEl.appendChild(p11);
    }
    container.appendChild(particlesEl);
    var bubblesEl = document.createElement("div");
    bubblesEl.className = "mistvideo-idle-bubbles";
    var bubbleEls = [];
    var bubbleTimers = [];
    for (var i2 = 0; i2 < 8; i2++) {
      var b = document.createElement("div");
      b.className = "bubble";
      b.style.top = Math.random() * 80 + 10 + "%";
      b.style.left = Math.random() * 80 + 10 + "%";
      b.style.width = Math.random() * 60 + 30 + "px";
      b.style.height = b.style.width;
      b.style.background = BUBBLE_COLORS_DEFAULT[i2 % BUBBLE_COLORS_DEFAULT.length];
      b.style.opacity = "0";
      bubblesEl.appendChild(b);
      bubbleEls.push(b);
    }
    container.appendChild(bubblesEl);
    function animateBubble(index) {
      var el = bubbleEls[index];
      if (!el) return;
      el.style.opacity = "0.15";
      var visDur = 4e3 + Math.random() * 3e3;
      bubbleTimers.push(MistVideo2.timers.start(function() {
        el.style.opacity = "0";
        bubbleTimers.push(MistVideo2.timers.start(function() {
          el.style.top = Math.random() * 80 + 10 + "%";
          el.style.left = Math.random() * 80 + 10 + "%";
          var sz = Math.random() * 60 + 30 + "px";
          el.style.width = sz;
          el.style.height = sz;
          bubbleTimers.push(MistVideo2.timers.start(function() {
            animateBubble(index);
          }, 200));
        }, 1500));
      }, visDur));
    }
    function startBubbles() {
      for (var i3 = 0; i3 < bubbleEls.length; i3++) {
        (function(idx) {
          bubbleTimers.push(MistVideo2.timers.start(function() {
            animateBubble(idx);
          }, idx * 500));
        })(i3);
      }
    }
    var logoWrap = document.createElement("div");
    logoWrap.className = "mistvideo-idle-logo";
    var logoPulse = document.createElement("div");
    logoPulse.className = "logo-pulse";
    logoWrap.appendChild(logoPulse);
    var logoBtn = document.createElement("div");
    logoBtn.className = "logo-button";
    logoBtn.setAttribute("role", "button");
    logoBtn.setAttribute("tabindex", "0");
    logoBtn.setAttribute("aria-label", MistVideo2.translate("mistserver logo"));
    logoBtn.innerHTML = MIST_LOGO_SVG;
    logoWrap.appendChild(logoBtn);
    container.appendChild(logoWrap);
    var logoSize = 100;
    var logoOffsetX = 0, logoOffsetY = 0;
    function updateLogoSize() {
      var rect = container.getBoundingClientRect();
      var min = Math.min(rect.width, rect.height);
      if (!isFinite(min) || min <= 0) return;
      logoSize = min * 0.2;
      logoBtn.style.width = logoSize + "px";
      logoBtn.style.height = logoSize + "px";
      logoPulse.style.width = logoSize * 1.4 + "px";
      logoPulse.style.height = logoSize * 1.4 + "px";
    }
    function updateLogoTransform() {
      logoWrap.style.transform = "translate(calc(-50% + " + logoOffsetX + "px), calc(-50% + " + logoOffsetY + "px))";
    }
    function onMouseMove(e) {
      var rect = container.getBoundingClientRect();
      if (rect.width === 0 || rect.height === 0) return;
      var cx = rect.left + rect.width / 2;
      var cy = rect.top + rect.height / 2;
      var dx = e.clientX - cx;
      var dy = e.clientY - cy;
      var dist = Math.sqrt(dx * dx + dy * dy);
      var maxDist = logoSize * 1.5;
      if (dist < maxDist && dist > 0) {
        var strength = (maxDist - dist) / maxDist;
        var push = 50 * strength;
        logoOffsetX = -(dx / dist) * push;
        logoOffsetY = -(dy / dist) * push;
      } else {
        logoOffsetX = 0;
        logoOffsetY = 0;
      }
      updateLogoTransform();
    }
    function onMouseLeave() {
      logoOffsetX = 0;
      logoOffsetY = 0;
      updateLogoTransform();
    }
    MistUtil.event.addListener(container, "mousemove", onMouseMove, container);
    MistUtil.event.addListener(container, "mouseleave", onMouseLeave, container);
    MistUtil.event.addListener(logoBtn, "click", function(e) {
      e.stopPropagation();
      var rect = container.getBoundingClientRect();
      var hx = e.clientX - rect.left;
      var hy = e.clientY - rect.top;
      var hm = document.createElement("div");
      hm.className = "hitmarker";
      hm.style.left = hx + "px";
      hm.style.top = hy + "px";
      hm.style.transform = "translate(-50%, -50%)";
      var lines = ["tl", "tr", "bl", "br"];
      for (var j = 0; j < 4; j++) {
        var line = document.createElement("div");
        line.className = "hitmarker-line " + lines[j];
        hm.appendChild(line);
      }
      hitmarkerLayer.appendChild(hm);
      playHitmarkerSound();
      MistVideo2.timers.start(function() {
        if (hm.parentNode) hm.parentNode.removeChild(hm);
      }, 600);
    }, container);
    var dvdEl = document.createElement("div");
    dvdEl.className = "mistvideo-idle-dvd";
    dvdEl.innerHTML = MIST_LOGO_SVG;
    container.appendChild(dvdEl);
    var dvdPos = { top: 0, left: 0 };
    var dvdVel = { x: 1.8, y: 1.6 };
    var dvdDims = { w: 60, h: 60 / DVD_ASPECT };
    var dvdColor = PARTICLE_COLORS_DEFAULT[0];
    var dvdColorIdx = 0;
    var dvdFrame = null;
    var dvdLastTime = 0;
    function dvdResize() {
      var pw = container.clientWidth;
      var ph = container.clientHeight;
      if (pw <= 0 || ph <= 0) return;
      var maxW = pw * 0.08;
      var maxH = ph * 0.08;
      var w = maxW;
      var h = w / DVD_ASPECT;
      if (h > maxH) {
        h = maxH;
        w = h * DVD_ASPECT;
      }
      dvdDims.w = Math.max(20, w);
      dvdDims.h = Math.max(20, h);
      dvdEl.style.width = dvdDims.w + "px";
      dvdEl.style.height = dvdDims.h + "px";
      dvdPos.top = Math.random() * Math.max(0, ph - dvdDims.h);
      dvdPos.left = Math.random() * Math.max(0, pw - dvdDims.w);
      var baseSpeed = Math.max(1.2, Math.min(dvdDims.w, dvdDims.h) / 70);
      dvdVel.x = baseSpeed * (Math.random() > 0.5 ? 1 : -1);
      dvdVel.y = baseSpeed * (Math.random() > 0.5 ? 1 : -1);
    }
    function dvdPickColor() {
      dvdColorIdx = (dvdColorIdx + 1) % PARTICLE_COLORS_DEFAULT.length;
      dvdColor = PARTICLE_COLORS_DEFAULT[dvdColorIdx];
      var paths = dvdEl.querySelectorAll("svg path");
      for (var i3 = 0; i3 < paths.length; i3++) {
        paths[i3].style.fill = dvdColor;
      }
    }
    function dvdTick(ts) {
      if (!dvdLastTime) dvdLastTime = ts;
      var dt = ts - dvdLastTime;
      dvdLastTime = ts;
      var mult = Math.min(dt / 16, 2);
      var pw = container.clientWidth;
      var ph = container.clientHeight;
      var maxTop = ph - dvdDims.h;
      var maxLeft = pw - dvdDims.w;
      dvdPos.top += dvdVel.y * mult;
      dvdPos.left += dvdVel.x * mult;
      var bounced = false;
      if (dvdPos.top <= 0 || dvdPos.top >= maxTop) {
        dvdVel.y = -dvdVel.y;
        dvdPos.top = Math.max(0, Math.min(maxTop, dvdPos.top));
        bounced = true;
      }
      if (dvdPos.left <= 0 || dvdPos.left >= maxLeft) {
        dvdVel.x = -dvdVel.x;
        dvdPos.left = Math.max(0, Math.min(maxLeft, dvdPos.left));
        bounced = true;
      }
      if (bounced) dvdPickColor();
      dvdEl.style.top = dvdPos.top + "px";
      dvdEl.style.left = dvdPos.left + "px";
      dvdFrame = requestAnimationFrame(dvdTick);
    }
    function dvdStart() {
      if (dvdFrame) return;
      dvdLastTime = 0;
      dvdFrame = requestAnimationFrame(dvdTick);
    }
    function dvdStop() {
      if (dvdFrame) {
        cancelAnimationFrame(dvdFrame);
        dvdFrame = null;
      }
    }
    var statusEl = document.createElement("div");
    statusEl.className = "mistvideo-idle-status";
    var indicatorEl = document.createElement("div");
    indicatorEl.className = "status-indicator";
    statusEl.appendChild(indicatorEl);
    var statusIconWrap = document.createElement("span");
    statusIconWrap.innerHTML = makeStatusIconSVG("default");
    indicatorEl.appendChild(statusIconWrap);
    var statusTextEl = document.createElement("span");
    statusTextEl.textContent = MistVideo2.translate("waiting for stream");
    indicatorEl.appendChild(statusTextEl);
    var progressBar = document.createElement("div");
    progressBar.className = "progress-bar";
    progressBar.style.display = "none";
    var progressFill = document.createElement("div");
    progressFill.className = "progress-fill";
    progressFill.style.width = "0%";
    progressBar.appendChild(progressFill);
    statusEl.appendChild(progressBar);
    var retryBtn = document.createElement("button");
    retryBtn.type = "button";
    retryBtn.className = "retry-btn";
    retryBtn.textContent = MistVideo2.translate("retry");
    retryBtn.style.display = "none";
    MistUtil.event.addListener(retryBtn, "click", function() {
      if (MistVideo2.player && MistVideo2.player.api && MistVideo2.player.api.load) {
        MistVideo2.player.api.load();
      } else {
        MistVideo2.reload("Retry button clicked");
      }
    }, container);
    statusEl.appendChild(retryBtn);
    container.appendChild(statusEl);
    var texture = document.createElement("div");
    texture.className = "mistvideo-idle-texture";
    texture.style.background = "radial-gradient(circle at 20% 80%, color-mix(in srgb, var(--mist-color-accent) 3%, transparent) 0%, transparent 50%),radial-gradient(circle at 80% 20%, color-mix(in srgb, var(--mist-color-link, var(--mist-color-accent)) 3%, transparent) 0%, transparent 50%),radial-gradient(circle at 40% 40%, color-mix(in srgb, var(--mist-color-warning) 2%, transparent) 0%, transparent 50%)";
    container.appendChild(texture);
    var isShowing = false;
    function updateStatus(state, percentage) {
      var type = classifyState(state);
      statusIconWrap.innerHTML = makeStatusIconSVG(type);
      if (state) {
        statusTextEl.textContent = state;
      } else {
        statusTextEl.textContent = MistVideo2.translate("waiting for stream");
      }
      if (type === "loading" && typeof percentage === "number") {
        progressBar.style.display = "";
        progressFill.style.width = Math.min(100, Math.max(0, percentage)) + "%";
      } else {
        progressBar.style.display = "none";
      }
      retryBtn.style.display = type === "error" ? "" : "none";
    }
    function show() {
      if (isShowing) return;
      isShowing = true;
      MistUtil.class.add(container, "show");
      dvdResize();
      dvdStart();
      updateLogoSize();
      startBubbles();
    }
    function hide() {
      if (!isShowing) return;
      isShowing = false;
      MistUtil.class.remove(container, "show");
      dvdStop();
      for (var i3 = 0; i3 < bubbleTimers.length; i3++) {
        MistVideo2.timers.stop(bubbleTimers[i3]);
      }
      bubbleTimers = [];
    }
    MistVideo2.showIdleScreen = function(state, percentage) {
      updateStatus(state, percentage);
      show();
      MistVideo2.clearError();
    };
    MistVideo2.hideIdleScreen = function() {
      hide();
    };
    if (typeof ResizeObserver !== "undefined") {
      var ro = new ResizeObserver(function() {
        if (isShowing) {
          updateLogoSize();
          dvdResize();
        }
      });
      var checkInserted = function() {
        if (MistVideo2.destroyed) return;
        if (container.parentNode) {
          ro.observe(container);
          return;
        }
        setTimeout(checkInserted, 100);
      };
      checkInserted();
    }
    if (MistVideo2.video) {
      var autoHideEvents = ["playing", "timeupdate"];
      for (var i2 = 0; i2 < autoHideEvents.length; i2++) {
        MistUtil.event.addListener(MistVideo2.video, autoHideEvents[i2], function(e) {
          if (!isShowing) return;
          if (e.type === "timeupdate" && MistVideo2.player && MistVideo2.player.api && MistVideo2.player.api.currentTime === 0) return;
          if (MistVideo2.state === "Stream is online") {
            hide();
          }
        }, container);
      }
    }
    return container;
  }
};

// src/ui/blueprints/index.js
var allBlueprints = Object.assign(
  {},
  containerBlueprints,
  controlsBarBlueprints,
  playbackBlueprints,
  settingsBlueprints,
  overlayBlueprints,
  mediaBlueprints,
  contextMenuBlueprints,
  idleScreenBlueprints
);

// src/ui/default-skin.js
var MistSkins = {};
MistSkins["default"] = {
  structure: {
    main: {
      if: function() {
        return !!this.info.hasVideo && this.source.type.split("/")[1] != "audio";
      },
      then: {
        //use this substructure when there is video
        type: "placeholder",
        classes: ["mistvideo"],
        children: [{ type: "contextMenu" }, {
          type: "hoverWindow",
          classes: ["mistvideo-maincontainer"],
          mode: "pos",
          style: { position: "relative" },
          transition: {
            hide: "left: 0; right: 0; bottom: calc(-1 * var(--mist-control-height) - 1px);",
            show: "bottom: 0;",
            viewport: "left:0; right: 0; top: -1000px; bottom: 0;"
          },
          button: { type: "videocontainer" },
          children: [{ type: "idleScreen" }, { type: "loading" }, { type: "keyControls" }, { type: "error" }],
          window: { type: "controls" }
        }]
      },
      else: {
        //use this subsctructure for audio only
        type: "container",
        classes: ["mistvideo"],
        style: { overflow: "visible" },
        children: [
          {
            type: "controls",
            classes: ["mistvideo-novideo"],
            style: { width: "480px" }
          },
          { type: "idleScreen" },
          { type: "loading" },
          { type: "keyControls" },
          { type: "error" },
          {
            if: function() {
              return this.options.controls == "stock";
            },
            then: {
              //show the video element if its controls will be used
              type: "video",
              style: { position: "absolute" }
            },
            else: {
              //hide the video element
              type: "video",
              style: {
                position: "absolute",
                display: "none"
              }
            }
          }
        ]
      }
    },
    videocontainer: {
      type: "container",
      children: [
        { type: "videobackground", alwaysDisplay: false, delay: 5 },
        { type: "video" },
        { type: "subtitles" }
      ]
    },
    controls: {
      if: function() {
        return !!(this.player && this.player.api && this.player.api.play);
      },
      then: {
        //use this subsctructure for players that have an api with at least a play function available
        type: "container",
        classes: ["mistvideo-column"],
        children: [
          {
            type: "progress",
            classes: ["mistvideo-pointer"]
          },
          {
            type: "container",
            classes: ["mistvideo-main", "mistvideo-padding", "mistvideo-row", "mistvideo-background"],
            children: [
              {
                type: "seekBackward",
                classes: ["mistvideo-pointer", "mistvideo-seek-btn"]
              },
              {
                type: "play",
                classes: ["mistvideo-pointer"]
              },
              {
                type: "seekForward",
                classes: ["mistvideo-pointer", "mistvideo-seek-btn"]
              },
              { type: "currentTime" },
              {
                if: function() {
                  if ("size" in this && this.size.width > 300 || (!this.info.hasVideo || this.source.type.split("/")[1] == "audio")) {
                    return true;
                  }
                  return false;
                },
                then: { type: "totalTime" }
              },
              {
                type: "container",
                classes: ["mistvideo-align-right"],
                children: [
                  {
                    type: "container",
                    classes: ["mistvideo-volume_group"],
                    children: [
                      {
                        type: "speaker",
                        classes: ["mistvideo-pointer"]
                      },
                      {
                        type: "container",
                        classes: ["mistvideo-volume_container"],
                        children: [{
                          type: "volume",
                          mode: "horizontal",
                          size: { height: 22 },
                          classes: ["mistvideo-pointer"]
                        }]
                      }
                    ]
                  },
                  {
                    if: function() {
                      if ("size" in this && this.size.width > 300 || (!this.info.hasVideo || this.source.type.split("/")[1] == "audio")) {
                        return true;
                      }
                      return false;
                    },
                    then: {
                      type: "container",
                      children: [
                        {
                          type: "chromecast",
                          classes: ["mistvideo-pointer"]
                        },
                        {
                          type: "airplay",
                          classes: ["mistvideo-pointer"]
                        },
                        {
                          type: "loop",
                          classes: ["mistvideo-pointer"]
                        },
                        {
                          type: "fullscreen",
                          classes: ["mistvideo-pointer"]
                        },
                        {
                          type: "picture-in-picture",
                          classes: ["mistvideo-pointer"]
                        }
                      ]
                    }
                  },
                  { type: "settings", classes: ["mistvideo-pointer"] },
                  { type: "submenu" }
                ]
              }
            ]
          }
        ]
      },
      else: {
        //use this subsctructure for players that don't have an api with at least a play function available
        if: function() {
          return !!(this.player && this.player.api);
        },
        then: {
          //use this subsctructure if some sort of api does exist
          type: "hoverWindow",
          mode: "pos",
          transition: {
            hide: "right: -1000px; bottom: calc(var(--mist-control-height) + 2px);",
            show: "right: var(--mist-space-xs);",
            viewport: "right: 0; left: -1000px; bottom: 0; top: -1000px"
          },
          style: { right: "5px", left: "auto" },
          button: {
            type: "settings",
            classes: ["mistvideo-background", "mistvideo-padding"]
          },
          window: { type: "submenu" }
        }
      }
    },
    submenu: {
      type: "container",
      style: {
        "maxWidth": "22em",
        "minWidth": "12em",
        "zIndex": 2
      },
      classes: ["mistvideo-padding", "mistvideo-column", "mistvideo-background"],
      children: [
        { type: "tracks" },
        {
          if: function() {
            if ("size" in this && this.size.width <= 300) {
              return true;
            }
            return false;
          },
          then: {
            type: "container",
            classes: ["mistvideo-center"],
            children: [
              {
                type: "chromecast",
                classes: ["mistvideo-pointer"]
              },
              {
                type: "loop",
                classes: ["mistvideo-pointer"]
              },
              {
                type: "fullscreen",
                classes: ["mistvideo-pointer"]
              },
              {
                type: "picture-in-picture",
                classes: ["mistvideo-pointer"]
              }
            ]
          }
        }
      ]
    },
    placeholder: {
      type: "container",
      classes: ["mistvideo", "mistvideo-delay-display"],
      children: [
        { type: "idleScreen" },
        { type: "placeholder" },
        { type: "loading" },
        { type: "error" }
      ]
    },
    secondaryVideo: function(switchThese) {
      return {
        type: "hoverWindow",
        classes: ["mistvideo"],
        mode: "pos",
        transition: {
          hide: "left: var(--mist-space-md); bottom: calc(-1 * var(--mist-control-height) + 2px);",
          show: "bottom: var(--mist-space-md);",
          viewport: "left: 0; right: 0; top: 0; bottom: 0"
        },
        button: {
          type: "container",
          children: [{ type: "videocontainer" }]
        },
        window: {
          type: "switchVideo",
          classes: ["mistvideo-controls", "mistvideo-padding", "mistvideo-background", "mistvideo-pointer"],
          containers: switchThese
        }
      };
    }
  },
  css: {
    skin: "/skins/default.css"
  },
  icons: {
    blueprints: {
      play: {
        size: 45,
        svg: '<path d="M6.26004984594 3.0550109625C5.27445051914 3.68940862462 4.67905105702 4.78142391497 4.67968264562 5.95354422781C4.67968264562 5.95354422781 4.70004942312 39.0717540916 4.70004942312 39.0717540916C4.70302341604 40.3033886636 5.36331656075 41.439734231 6.43188211452 42.0521884912C7.50044766829 42.6646427515 8.81469531629 42.6600161659 9.87892235656 42.0400537716C9.87892235656 42.0400537716 38.5612768409 25.4802882606 38.5612768409 25.4802882606C39.6181165777 24.8606067582 40.2663250096 23.7262617523 40.2636734301 22.5011460995C40.2610218505 21.2760304467 39.6079092743 20.1445019555 38.5483970356 19.5294009803C38.5483970356 19.5294009803 9.84567577375 2.9709566275 9.84567577375 2.9709566275C8.72898008118 2.32550764609 7.34527425735 2.35794451351 6.26004984594 3.0550109625C6.26004984594 3.0550109625 6.26004984594 3.0550109625 6.26004984594 3.0550109625" class="fill" />'
      },
      largeplay: {
        size: 45,
        svg: '<path d="M6.26004984594 3.0550109625C5.27445051914 3.68940862462 4.67905105702 4.78142391497 4.67968264562 5.95354422781C4.67968264562 5.95354422781 4.70004942312 39.0717540916 4.70004942312 39.0717540916C4.70302341604 40.3033886636 5.36331656075 41.439734231 6.43188211452 42.0521884912C7.50044766829 42.6646427515 8.81469531629 42.6600161659 9.87892235656 42.0400537716C9.87892235656 42.0400537716 38.5612768409 25.4802882606 38.5612768409 25.4802882606C39.6181165777 24.8606067582 40.2663250096 23.7262617523 40.2636734301 22.5011460995C40.2610218505 21.2760304467 39.6079092743 20.1445019555 38.5483970356 19.5294009803C38.5483970356 19.5294009803 9.84567577375 2.9709566275 9.84567577375 2.9709566275C8.72898008118 2.32550764609 7.34527425735 2.35794451351 6.26004984594 3.0550109625C6.26004984594 3.0550109625 6.26004984594 3.0550109625 6.26004984594 3.0550109625" class="stroke" />'
      },
      pause: {
        size: 45,
        svg: '<g><path d="m 7.5,38.531275 a 4.0011916,4.0011916 0 0 0 3.749999,3.96873 l 2.2812501,0 a 4.0011916,4.0011916 0 0 0 3.96875,-3.75003 l 0,-32.28123 a 4.0011916,4.0011916 0 0 0 -3.75,-3.96875 l -2.2812501,0 a 4.0011916,4.0011916 0 0 0 -3.968749,3.75 l 0,32.28128 z" class="fill" /><path d="m 27.5,38.531275 a 4.0011916,4.0011916 0 0 0 3.75,3.9687 l 2.28125,0 a 4.0011916,4.0011916 0 0 0 3.96875,-3.75 l 0,-32.28126 a 4.0011916,4.0011916 0 0 0 -3.75,-3.96875 l -2.28125,0 a 4.0011916,4.0011916 0 0 0 -3.96875,3.75 l 0,32.28131 z" class="fill" /></g>'
      },
      speaker: {
        size: 45,
        svg: '<path d="m 32.737813,5.2037363 c -1.832447,-1.10124 -4.200687,-0.8622 -5.771871,0.77112 0,0 -7.738819,8.0443797 -7.738819,8.0443797 0,0 -3.417976,0 -3.417976,0 -1.953668,0 -3.54696,1.65618 -3.54696,3.68694 0,0 0,9.58644 0,9.58644 0,2.03094 1.593292,3.68712 3.54696,3.68712 0,0 3.417976,0 3.417976,0 0,0 7.738819,8.04474 7.738819,8.04474 1.572104,1.63404 3.938942,1.8747 5.771871,0.77076 0,0 0,-34.5914997 0,-34.5914997 z" class="stroke semiFill toggle" />'
      },
      volume: {
        size: { width: 100, height: 45 },
        svg: function() {
          var uid = MistUtil.createUnique();
          return '<defs><mask id="' + uid + '"><path d="m6.202 33.254 86.029-28.394c2.6348-0.86966 4.7433 0.77359 4.7433 3.3092v28.617c0 1.9819-1.6122 3.5773-3.6147 3.5773h-86.75c-4.3249 0-5.0634-5.5287-0.40598-7.1098" fill="#fff" /></mask></defs><rect mask="url(#' + uid + ')" class="slider horizontal semiFill" width="100%" height="100%" /><path d="m6.202 33.254 86.029-28.394c2.6348-0.86966 4.7433 0.77359 4.7433 3.3092v28.617c0 1.9819-1.6122 3.5773-3.6147 3.5773h-86.75c-4.3249 0-5.0634-5.5287-0.40598-7.1098" class="stroke" /><rect x="0" y="0" width="100%" height="100%" fill="rgba(0,0,0,0.001)"/>';
        }
      },
      muted: {
        size: 45,
        svg: '<g class="stroke" stroke-linecap="round" vector-effect="none" stroke-width="2"><path d="m19.815 5.9747-7.7388 8.0444h-3.418c-1.9537 0-3.547 1.6562-3.547 3.6869v9.5864c0 2.0309 1.5933 3.6871 3.547 3.6871h3.418l7.7388 8.0447c1.5721 1.634 3.9389 1.8747 5.7719.77076v-34.591c-1.8324-1.1014-4.2007-.86258-5.7719.77074z"/><path d="m30.032 27.86 9.8517-9.8517"/><path d="m30.032 18.008 9.8517 9.8517"/></g>'
      },
      unmuted: {
        size: 45,
        svg: '<g class="stroke" stroke-linecap="round" vector-effect="none" stroke-width="2"><path d="m19.815 5.9747-7.7388 8.0444h-3.418c-1.9537 0-3.547 1.6562-3.547 3.6869v9.5864c0 2.0309 1.5933 3.6871 3.547 3.6871h3.418l7.7388 8.0447c1.5721 1.634 3.9389 1.8747 5.7719.77076v-34.591c-1.8324-1.1014-4.2007-.86258-5.7719.77074z"/><path d="m29.578 28.432c1.7601-1.2785 2.8014-3.3226 2.8008-5.498.000642-2.1754-1.0407-4.2196-2.8008-5.498"/><path d="m32.197 31.051c2.4404-1.9883 3.8564-4.9693 3.8555-8.1172-.000179-3.1475-1.4168-6.1278-3.8574-8.1152"/></g>'
      },
      fullscreen: {
        size: 45,
        svg: '<path d="m2.5 10.928v8.5898l4.9023-2.8008 9.6172 5.7832-9.6172 5.7832-4.9023-2.8008v8.5898h15.031l-4.9004-2.8008 9.8691-5.6387 9.8691 5.6387-4.9004 2.8008h15.031v-8.5898l-4.9023 2.8008-9.6172-5.7832 9.6172-5.7832 4.9023 2.8008v-8.5898h-15.033l4.9023 2.8008-9.8691 5.6387-9.8691-5.6387 4.9023-2.8008z" class="fill">'
      },
      pip: {
        size: 45,
        svg: '<rect x="5.25" y="12.25" width="34.5" height="19.5" ry="2" class="stroke semiFill toggle"/><rect x="20" y="21" width="17" height="8" rx="2" ry="2" class="semiFill toggle stroke"/>'
      },
      loop: {
        size: 45,
        svg: '<path d="M 21.279283,3.749797 A 18.750203,18.750203 0 0 0 8.0304417,9.2511582 L 12.740779,13.961496 A 12.083464,12.083464 0 0 1 21.279283,10.416536 12.083464,12.083464 0 0 1 33.362748,22.5 12.083464,12.083464 0 0 1 21.279283,34.583464 12.083464,12.083464 0 0 1 12.740779,31.038504 l 3.063185,-3.063185 H 4.9705135 V 38.80877 L 8.0304417,35.748842 A 18.750203,18.750203 0 0 0 21.279283,41.250203 18.750203,18.750203 0 0 0 40.029486,22.5 18.750203,18.750203 0 0 0 21.279283,3.749797 Z" class="stroke semiFill toggle" />'
      },
      settings: {
        size: 45,
        svg: '<path d="m24.139 3.834-1.4785 4.3223c-1.1018 0.0088-2.2727 0.13204-3.2031 0.33594l-2.3281-3.9473c-1.4974 0.45304-2.9327 1.091-4.2715 1.9004l1.3457 4.3672c-0.87808 0.62225-1.685 1.3403-2.4023 2.1426l-4.1953-1.8223c-0.9476 1.2456-1.7358 2.6055-2.3457 4.0469l3.6523 2.7383c-0.34895 1.0215-0.58154 2.0787-0.69336 3.1523l-4.4531 0.98828c-0.00716 0.14696-0.011931 0.29432-0.015625 0.44141 0.00628 1.4179 0.17336 2.8307 0.49805 4.2109l4.5703 0.070312c0.32171 1.0271 0.75826 2.0138 1.3008 2.9434l-3.0391 3.4355c0.89502 1.2828 1.9464 2.4492 3.1309 3.4707l3.7363-2.6289c0.86307 0.64582 1.7958 1.192 2.7812 1.6289l-0.43555 4.541c1.4754 0.52082 3.0099 0.85458 4.5684 0.99414l1.4766-4.3223c0.05369 3e-3 0.10838 0.005313 0.16211 0.007812 1.024-0.0061 2.0436-0.12048 3.043-0.34375l2.3281 3.9473c1.4974-0.45304 2.9327-1.091 4.2715-1.9004l-1.3457-4.3672c0.87808-0.62225 1.685-1.3403 2.4023-2.1426l4.1953 1.8223c0.9476-1.2456 1.7358-2.6055 2.3457-4.0469l-3.6523-2.7383c0.34895-1.0215 0.58154-2.0787 0.69336-3.1523l4.4531-0.98828c0.0072-0.14698 0.011925-0.29432 0.015625-0.44141-0.0062-1.4179-0.17336-2.8307-0.49805-4.2109l-4.5703-0.070312c-0.32171-1.0271-0.75826-2.0138-1.3008-2.9434l3.0391-3.4355c-0.89502-1.2828-1.9464-2.4492-3.1309-3.4707l-3.7363 2.6289c-0.86307-0.64582-1.7958-1.192-2.7812-1.6289l0.43555-4.541c-1.4754-0.52082-3.0099-0.85457-4.5684-0.99414zm-1.6387 7.8789a10.786 10.786 0 0 1 10.787 10.787 10.786 10.786 0 0 1-10.787 10.787 10.786 10.786 0 0 1-10.787-10.787 10.786 10.786 0 0 1 10.787-10.787z" class="fill"/>'
      },
      loading: {
        size: 100,
        svg: '<path d="m49.998 8.7797e-4c-0.060547 0.0018431-0.12109 0.0037961-0.18164 0.0058593-0.1251 0.0015881-0.25012 0.0061465-0.375 0.013672h-0.001954c-27.388 0.30599-49.432 22.59-49.439 49.98 0.020074 2.6488 0.25061 5.292 0.68945 7.904 3.8792-24.231 24.77-42.065 49.311-42.096v-0.0058582h0.001954c4.3638 3.0803e-4 7.9013-3.5366 7.9021-7.9002 1.474e-4 -2.0958-0.83235-4.106-2.3144-5.5879-1.482-1.482-3.492-2.3145-5.5879-2.3144-6.5007e-4 -7.9369e-8 -0.0013001-7.9369e-8 -0.001954 0" class="semiFill spin"></path>'
      },
      timeout: {
        size: 25,
        svg: function(options) {
          if (!options || !options.delay) {
            options = { delay: 10 };
          }
          var delay = options.delay;
          var uid = MistUtil.createUnique();
          return '<defs><mask id="' + uid + '"><rect x="0" y="0" width="25" height="25" fill="#fff"/><rect x="-5" y="-5" width="17.5" height="35" fill="#000" transform="rotate(180,12.5,12.5)"><animateTransform attributeName="transform" type="rotate" from="0,12.5,12.5" to="180,12.5,12.5" begin="DOMNodeInsertedIntoDocument" dur="' + delay / 2 + 's" repeatCount="1"/></rect><rect x="0" y="0" width="12.5" height="25" fill="#fff"/><rect x="-5" y="-5" width="17.5" height="35" fill="#000" transform="rotate(360,12.5,12.5)"><animate attributeType="CSS" attributeName="opacity" from="0" to="1" begin="DOMNodeInsertedIntoDocument" dur="' + delay + 's" calcMode="discrete" repeatCount="1" /><animateTransform attributeName="transform" type="rotate" from="180,12.5,12.5" to="360,12.5,12.5" begin="DOMNodeInsertedIntoDocument+' + delay / 2 + 's" dur="' + delay / 2 + 's" repeatCount="1"/></rect><circle cx="12.5" cy="12.5" r="8" fill="#000"/></mask></defs><circle cx="12.5" cy="12.5" r="12.5" class="fill" mask="url(#' + uid + ')"/>';
        }
      },
      popout: {
        size: 45,
        svg: '<path d="m24.721 11.075c-12.96 0.049575-32.113 15.432-10.336 28.834-7.6763-7.9825-2.4795-21.824 10.336-22.19v5.5368l15.276-8.862-15.276-8.86v5.5419z" class="stroke fill"/>'
      },
      switchvideo: {
        size: 45,
        svg: '<path d="m8.4925 18.786c-3.9578 1.504-6.4432 3.632-6.4434 5.9982 2.183e-4 4.1354 7.5562 7.5509 17.399 8.1467v4.7777l10.718-6.2573-10.718-6.2529v4.5717c-6.9764-0.4712-12.229-2.5226-12.227-4.9859 6.693e-4 -0.72127 0.45868-1.4051 1.2714-2.0267zm28.015 0v3.9715c0.81164 0.62126 1.2685 1.3059 1.2692 2.0267-0.0014 1.4217-1.791 2.75-4.8021 3.6968-2.0515 0.82484-0.93693 3.7696 1.2249 2.9659 5.3088-1.8593 8.7426-3.8616 8.7514-6.6627-1.26e-4 -2.3662-2.4856-4.4942-6.4434-5.9982z" class="fill"/><rect rect x="10.166" y="7.7911" width="24.668" height="15.432" class="stroke"/>'
      },
      forward: {
        size: 45,
        svg: '<g class="stroke" style="fill:none;stroke-linecap:round;stroke-linejoin:round;stroke-width:6"><path d="m26.144 11.568 7.2881 10.932-7.2881 10.932"/><path d="m11.568 11.568 7.2881 10.932-7.2881 10.932"/></g>'
      },
      backward: {
        size: 45,
        svg: '<g class="stroke" style="fill:none;stroke-linecap:round;stroke-linejoin:round;stroke-width:6;transform:scaleX(-1);transform-origin:center"><path d="m26.144 11.568 7.2881 10.932-7.2881 10.932"/><path d="m11.568 11.568 7.2881 10.932-7.2881 10.932"/></g>'
      },
      right: {
        size: 45,
        svg: '<path class="fill"  d="m3.5 26.048c0 1.4295 1.1443 2.5803 2.5656 2.5803h21.975v7.4083l13.459-13.537-13.459-13.537v7.4083h-21.975c-1.4214 0-2.5656 1.1508-2.5656 2.5803z" style="stroke-linejoin:round;stroke-width:2">'
      },
      left: {
        size: 45,
        svg: '<path class="fill"  d="m3.5 26.048c0 1.4295 1.1443 2.5803 2.5656 2.5803h21.975v7.4083l13.459-13.537-13.459-13.537v7.4083h-21.975c-1.4214 0-2.5656 1.1508-2.5656 2.5803z" style="stroke-linejoin:round;stroke-width:2;transform:scaleX(-1);transform-origin:center">'
      },
      seekforward: {
        size: 45,
        svg: '<g class="stroke" style="fill:none;stroke-linecap:round;stroke-linejoin:round;stroke-width:3"><path d="M22.5 6.5A16 16 0 1 1 9.2 12.8"/><path d="M22.5 2v9h-9"/></g><text x="22.5" y="28" text-anchor="middle" class="fill" style="font-size:11px;font-family:sans-serif;stroke:none">10</text>'
      },
      seekbackward: {
        size: 45,
        svg: '<g class="stroke" style="fill:none;stroke-linecap:round;stroke-linejoin:round;stroke-width:3"><path d="M22.5 6.5A16 16 0 1 0 35.8 12.8"/><path d="M22.5 2v9h9"/></g><text x="22.5" y="28" text-anchor="middle" class="fill" style="font-size:11px;font-family:sans-serif;stroke:none">10</text>'
      },
      airplay: {
        size: 45,
        svg: '<g class="stroke" style="fill:none;stroke-linecap:round;stroke-linejoin:round;stroke-width:3"><path d="M5 36V10a3 3 0 0 1 3-3h29a3 3 0 0 1 3 3v26"/><polygon points="22.5,38 11,26 34,26" class="fill"/></g>'
      }
    }
  },
  blueprints: allBlueprints,
  colors: {
    fill: "#fff",
    semiFill: "rgba(255,255,255,0.5)",
    stroke: "#fff",
    strokeWidth: 1.5,
    background: "rgba(0,0,0,0.8)",
    progressBackground: "#333",
    accent: "#0f0"
  }
};
MistSkins["minimal"] = {
  inherit: "default",
  structure: {
    main: {
      if: function() {
        return !!this.info.hasVideo && this.source.type.split("/")[1] != "audio";
      },
      then: {
        type: "container",
        classes: ["mistvideo"],
        children: [
          { type: "videocontainer" },
          {
            type: "container",
            classes: ["mistvideo-column"],
            children: [
              { type: "progress", classes: ["mistvideo-pointer"] },
              {
                type: "container",
                classes: ["mistvideo-main", "mistvideo-padding", "mistvideo-row", "mistvideo-background"],
                children: [
                  { type: "play", classes: ["mistvideo-pointer"] },
                  { type: "currentTime" },
                  {
                    type: "container",
                    classes: ["mistvideo-align-right"],
                    children: [
                      { type: "fullscreen", classes: ["mistvideo-pointer"] }
                    ]
                  }
                ]
              }
            ]
          },
          { type: "idleScreen" },
          { type: "loading" },
          { type: "error" }
        ]
      },
      else: {
        type: "container",
        classes: ["mistvideo"],
        children: [
          {
            type: "container",
            classes: ["mistvideo-column"],
            children: [
              { type: "play", classes: ["mistvideo-pointer"] },
              { type: "progress", classes: ["mistvideo-pointer"] }
            ]
          },
          { type: "idleScreen" },
          { type: "loading" },
          { type: "error" }
        ]
      }
    },
    videocontainer: {
      type: "container",
      children: [
        { type: "video" },
        { type: "subtitles" }
      ]
    }
  },
  css: {
    skin: "/skins/default.css"
  }
};
MistSkins["branded"] = {
  inherit: "default",
  colors: {
    fill: "#e8e8e8",
    semiFill: "rgba(232,232,232,0.4)",
    stroke: "#e8e8e8",
    strokeWidth: 2,
    background: "rgba(15,15,25,0.92)",
    progressBackground: "#2a2a3a",
    accent: "#e63946"
  },
  tokens: {
    "--mist-radius-sm": "6px",
    "--mist-radius-md": "8px",
    "--mist-progress-height": "3px",
    "--mist-progress-height-hover": "8px",
    "--mist-progress-knob-size": "6px"
  },
  blueprints: {
    logo: function() {
      var container = document.createElement("div");
      container.className = "mistvideo-brand-logo";
      container.textContent = "BRAND";
      return container;
    }
  },
  css: {
    skin: "/skins/default.css"
  }
};

// src/core/themes.js
var MistThemes = {
  "default": {
    dark: {
      "color-text": "#fff",
      "color-text-muted": "rgba(255,255,255,0.5)",
      "color-icon-fill": "#fff",
      "color-icon-stroke": "#fff",
      "color-surface": "rgba(0,0,0,0.8)",
      "color-accent": "#0f0",
      "color-accent-hover": "#80ff80",
      "color-progress-track": "#333",
      "color-progress-buffer": "rgba(255,255,255,0.25)",
      "color-border": "rgba(255,255,255,0.3)",
      "color-link": "#0f0",
      "color-hover": "rgba(255,255,255,0.1)",
      "color-separator": "rgba(255,255,255,0.06)",
      "color-warning": "#e0af68",
      "color-danger": "#f7768e",
      "idle-bg-from": "#0a0a0a",
      "idle-bg-to": "#141414"
    },
    light: {
      "color-text": "#1a1a1a",
      "color-text-muted": "rgba(0,0,0,0.4)",
      "color-icon-fill": "#333",
      "color-icon-stroke": "#333",
      "color-surface": "rgba(245,245,245,0.95)",
      "color-accent": "#0066cc",
      "color-accent-hover": "#3385d6",
      "color-progress-track": "#ddd",
      "color-progress-buffer": "rgba(0,0,0,0.15)",
      "color-border": "rgba(0,0,0,0.15)",
      "color-link": "#0066cc",
      "color-hover": "rgba(0,0,0,0.08)",
      "color-separator": "rgba(0,0,0,0.06)",
      "color-warning": "#b8860b",
      "color-danger": "#dc3545",
      "idle-bg-from": "#e8e8e8",
      "idle-bg-to": "#f5f5f5"
    }
  },
  "tokyo-night": {
    dark: {
      "color-text": "#c0caf5",
      "color-text-muted": "rgba(192,202,245,0.5)",
      "color-icon-fill": "#c0caf5",
      "color-icon-stroke": "#c0caf5",
      "color-surface": "rgba(26,27,38,0.95)",
      "color-accent": "#7aa2f7",
      "color-accent-hover": "#9ab8f9",
      "color-progress-track": "#1f2335",
      "color-progress-buffer": "rgba(192,202,245,0.25)",
      "color-border": "rgba(65,72,104,0.5)",
      "color-link": "#7aa2f7",
      "color-hover": "rgba(192,202,245,0.08)",
      "color-separator": "rgba(192,202,245,0.06)",
      "color-warning": "#e0af68",
      "color-danger": "#f7768e",
      "idle-bg-from": "#16161e",
      "idle-bg-to": "#1a1b26"
    },
    light: {
      "color-text": "#3b4261",
      "color-text-muted": "rgba(59,66,97,0.4)",
      "color-icon-fill": "#4e5772",
      "color-icon-stroke": "#4e5772",
      "color-surface": "rgba(225,226,231,0.95)",
      "color-accent": "#2e7de9",
      "color-accent-hover": "#5797ed",
      "color-progress-track": "#d5d6db",
      "color-progress-buffer": "rgba(59,66,97,0.15)",
      "color-border": "rgba(180,181,185,0.5)",
      "color-link": "#2e7de9",
      "color-hover": "rgba(59,66,97,0.08)",
      "color-separator": "rgba(59,66,97,0.06)",
      "color-warning": "#8c6c3e",
      "color-danger": "#c64756",
      "idle-bg-from": "#d5d6db",
      "idle-bg-to": "#e1e2e7"
    }
  },
  "dracula": {
    dark: {
      "color-text": "#f8f8f2",
      "color-text-muted": "rgba(248,248,242,0.5)",
      "color-icon-fill": "#f8f8f2",
      "color-icon-stroke": "#f8f8f2",
      "color-surface": "rgba(40,42,54,0.95)",
      "color-accent": "#bd93f9",
      "color-accent-hover": "#ff79c6",
      "color-progress-track": "#44475a",
      "color-progress-buffer": "rgba(248,248,242,0.25)",
      "color-border": "rgba(98,114,164,0.5)",
      "color-link": "#8be9fd",
      "color-hover": "rgba(248,248,242,0.08)",
      "color-separator": "rgba(248,248,242,0.06)",
      "color-warning": "#f1fa8c",
      "color-danger": "#ff5555",
      "idle-bg-from": "#21222c",
      "idle-bg-to": "#282a36"
    }
  },
  "nord": {
    dark: {
      "color-text": "#eceff4",
      "color-text-muted": "rgba(236,239,244,0.5)",
      "color-icon-fill": "#d8dee9",
      "color-icon-stroke": "#d8dee9",
      "color-surface": "rgba(46,52,64,0.95)",
      "color-accent": "#88c0d0",
      "color-accent-hover": "#8fbcbb",
      "color-progress-track": "#3b4252",
      "color-progress-buffer": "rgba(236,239,244,0.2)",
      "color-border": "rgba(76,86,106,0.5)",
      "color-link": "#81a1c1",
      "color-hover": "rgba(236,239,244,0.08)",
      "color-separator": "rgba(236,239,244,0.06)",
      "color-warning": "#ebcb8b",
      "color-danger": "#bf616a",
      "idle-bg-from": "#242933",
      "idle-bg-to": "#2e3440"
    }
  },
  "catppuccin": {
    dark: {
      "color-text": "#cdd6f4",
      "color-text-muted": "rgba(205,214,244,0.5)",
      "color-icon-fill": "#cdd6f4",
      "color-icon-stroke": "#cdd6f4",
      "color-surface": "rgba(30,30,46,0.95)",
      "color-accent": "#cba6f7",
      "color-accent-hover": "#f5c2e7",
      "color-progress-track": "#313244",
      "color-progress-buffer": "rgba(205,214,244,0.2)",
      "color-border": "rgba(88,91,112,0.5)",
      "color-link": "#89b4fa",
      "color-hover": "rgba(205,214,244,0.08)",
      "color-separator": "rgba(205,214,244,0.06)",
      "color-warning": "#f9e2af",
      "color-danger": "#f38ba8",
      "idle-bg-from": "#11111b",
      "idle-bg-to": "#1e1e2e"
    },
    light: {
      "color-text": "#4c4f69",
      "color-text-muted": "rgba(76,79,105,0.5)",
      "color-icon-fill": "#5c5f77",
      "color-icon-stroke": "#5c5f77",
      "color-surface": "rgba(239,241,245,0.95)",
      "color-accent": "#8839ef",
      "color-accent-hover": "#dd7878",
      "color-progress-track": "#ccd0da",
      "color-progress-buffer": "rgba(76,79,105,0.15)",
      "color-border": "rgba(156,160,176,0.5)",
      "color-link": "#1e66f5",
      "color-hover": "rgba(76,79,105,0.08)",
      "color-separator": "rgba(76,79,105,0.06)",
      "color-warning": "#df8e1d",
      "color-danger": "#d20f39",
      "idle-bg-from": "#dce0e8",
      "idle-bg-to": "#eff1f5"
    }
  },
  "gruvbox": {
    dark: {
      "color-text": "#ebdbb2",
      "color-text-muted": "rgba(235,219,178,0.5)",
      "color-icon-fill": "#ebdbb2",
      "color-icon-stroke": "#ebdbb2",
      "color-surface": "rgba(40,40,40,0.95)",
      "color-accent": "#d79921",
      "color-accent-hover": "#fabd2f",
      "color-progress-track": "#3c3836",
      "color-progress-buffer": "rgba(235,219,178,0.2)",
      "color-border": "rgba(146,131,116,0.5)",
      "color-link": "#83a598",
      "color-hover": "rgba(235,219,178,0.08)",
      "color-separator": "rgba(235,219,178,0.06)",
      "color-warning": "#fabd2f",
      "color-danger": "#fb4934",
      "idle-bg-from": "#1d2021",
      "idle-bg-to": "#282828"
    },
    light: {
      "color-text": "#3c3836",
      "color-text-muted": "rgba(60,56,54,0.5)",
      "color-icon-fill": "#504945",
      "color-icon-stroke": "#504945",
      "color-surface": "rgba(251,241,199,0.95)",
      "color-accent": "#d79921",
      "color-accent-hover": "#b57614",
      "color-progress-track": "#ebdbb2",
      "color-progress-buffer": "rgba(60,56,54,0.15)",
      "color-border": "rgba(168,153,132,0.5)",
      "color-link": "#427b58",
      "color-hover": "rgba(60,56,54,0.08)",
      "color-separator": "rgba(60,56,54,0.06)",
      "color-warning": "#d79921",
      "color-danger": "#cc241d",
      "idle-bg-from": "#ebdbb2",
      "idle-bg-to": "#fbf1c7"
    }
  },
  "one-dark": {
    dark: {
      "color-text": "#abb2bf",
      "color-text-muted": "rgba(171,178,191,0.5)",
      "color-icon-fill": "#abb2bf",
      "color-icon-stroke": "#abb2bf",
      "color-surface": "rgba(40,44,52,0.95)",
      "color-accent": "#61afef",
      "color-accent-hover": "#528bff",
      "color-progress-track": "#2c313a",
      "color-progress-buffer": "rgba(171,178,191,0.2)",
      "color-border": "rgba(62,68,81,0.7)",
      "color-link": "#61afef",
      "color-hover": "rgba(171,178,191,0.08)",
      "color-separator": "rgba(171,178,191,0.06)",
      "color-warning": "#e5c07b",
      "color-danger": "#e06c75",
      "idle-bg-from": "#21252b",
      "idle-bg-to": "#282c34"
    }
  },
  "github-dark": {
    dark: {
      "color-text": "#e6edf3",
      "color-text-muted": "rgba(230,237,243,0.5)",
      "color-icon-fill": "#e6edf3",
      "color-icon-stroke": "#e6edf3",
      "color-surface": "rgba(13,17,23,0.95)",
      "color-accent": "#58a6ff",
      "color-accent-hover": "#79c0ff",
      "color-progress-track": "#161b22",
      "color-progress-buffer": "rgba(230,237,243,0.15)",
      "color-border": "rgba(48,54,61,0.7)",
      "color-link": "#58a6ff",
      "color-hover": "rgba(230,237,243,0.08)",
      "color-separator": "rgba(230,237,243,0.06)",
      "color-warning": "#d29922",
      "color-danger": "#f85149",
      "idle-bg-from": "#010409",
      "idle-bg-to": "#0d1117"
    }
  },
  "rose-pine": {
    dark: {
      "color-text": "#e0def4",
      "color-text-muted": "rgba(224,222,244,0.5)",
      "color-icon-fill": "#e0def4",
      "color-icon-stroke": "#e0def4",
      "color-surface": "rgba(25,23,36,0.95)",
      "color-accent": "#c4a7e7",
      "color-accent-hover": "#ebbcba",
      "color-progress-track": "#1f1d2e",
      "color-progress-buffer": "rgba(224,222,244,0.2)",
      "color-border": "rgba(110,106,134,0.5)",
      "color-link": "#9ccfd8",
      "color-hover": "rgba(224,222,244,0.08)",
      "color-separator": "rgba(224,222,244,0.06)",
      "color-warning": "#f6c177",
      "color-danger": "#eb6f92",
      "idle-bg-from": "#131120",
      "idle-bg-to": "#191724"
    }
  },
  "solarized": {
    dark: {
      "color-text": "#839496",
      "color-text-muted": "rgba(131,148,150,0.6)",
      "color-icon-fill": "#93a1a1",
      "color-icon-stroke": "#93a1a1",
      "color-surface": "rgba(0,43,54,0.95)",
      "color-accent": "#2aa198",
      "color-accent-hover": "#268bd2",
      "color-progress-track": "#073642",
      "color-progress-buffer": "rgba(131,148,150,0.3)",
      "color-border": "rgba(88,110,117,0.5)",
      "color-link": "#268bd2",
      "color-hover": "rgba(131,148,150,0.1)",
      "color-separator": "rgba(131,148,150,0.06)",
      "color-warning": "#b58900",
      "color-danger": "#dc322f",
      "idle-bg-from": "#002028",
      "idle-bg-to": "#002b36"
    },
    light: {
      "color-text": "#657b83",
      "color-text-muted": "rgba(101,123,131,0.6)",
      "color-icon-fill": "#586e75",
      "color-icon-stroke": "#586e75",
      "color-surface": "rgba(253,246,227,0.95)",
      "color-accent": "#2aa198",
      "color-accent-hover": "#268bd2",
      "color-progress-track": "#eee8d5",
      "color-progress-buffer": "rgba(101,123,131,0.2)",
      "color-border": "rgba(147,161,161,0.4)",
      "color-link": "#268bd2",
      "color-hover": "rgba(101,123,131,0.08)",
      "color-separator": "rgba(101,123,131,0.06)",
      "color-warning": "#b58900",
      "color-danger": "#dc322f",
      "idle-bg-from": "#eee8d5",
      "idle-bg-to": "#fdf6e3"
    }
  },
  "ayu-mirage": {
    dark: {
      "color-text": "#cccac2",
      "color-text-muted": "rgba(204,202,194,0.5)",
      "color-icon-fill": "#cccac2",
      "color-icon-stroke": "#cccac2",
      "color-surface": "rgba(36,40,54,0.95)",
      "color-accent": "#ffad66",
      "color-accent-hover": "#f29e59",
      "color-progress-track": "#1f2430",
      "color-progress-buffer": "rgba(204,202,194,0.2)",
      "color-border": "rgba(86,91,112,0.5)",
      "color-link": "#73d0ff",
      "color-hover": "rgba(204,202,194,0.08)",
      "color-separator": "rgba(204,202,194,0.06)",
      "color-warning": "#ffd580",
      "color-danger": "#ff6666",
      "idle-bg-from": "#171b24",
      "idle-bg-to": "#1f2430"
    }
  }
};
function resolveTheme(themeOrTokens, mode) {
  if (!themeOrTokens) return null;
  if (typeof themeOrTokens === "object") return themeOrTokens;
  if (typeof themeOrTokens !== "string") return null;
  var theme = MistThemes[themeOrTokens];
  if (!theme) return null;
  if (mode && theme[mode]) return theme[mode];
  if (theme.dark) return theme.dark;
  if (theme.light) return theme.light;
  return null;
}

// src/core/locales.js
var locales = {
  en: {
    play: "Play",
    pause: "Pause",
    mute: "Mute",
    unmute: "Unmute",
    volume: "Volume",
    fullscreen: "Full screen",
    "exit fullscreen": "Exit full screen",
    pip: "Picture in picture",
    loop: "Loop",
    settings: "Settings",
    automatic: "Automatic",
    none: "None",
    live: "live",
    "current time": "Current time",
    "total time": "Total time",
    "seek bar": "Seek bar",
    "seek forward": "Seek forward",
    "seek backward": "Seek backward",
    "-10s": "-10s",
    "+10s": "+10s",
    airplay: "AirPlay",
    speed: "Speed",
    "speed doubled": "Speed doubled",
    muted: "Muted",
    "(muted)": "(muted)",
    "seek backward seconds": "- 10 seconds",
    "seek forward seconds": "+ 10 seconds",
    "to start": "To start..",
    "to end": "To end..",
    "frame forward": "Frame +1",
    "frame backward": "Frame -1",
    "reload video": "Reload video",
    "reload player": "Reload player",
    "next source": "Next source",
    ignore: "Ignore",
    "player encountered a problem": "The player has encountered a problem",
    "chromecast encountered a problem": "The chromecast has encountered a problem",
    "stop casting": "Stop casting",
    chromecast: "Chromecast",
    "select cast device": "Select a device to cast to",
    "casting to": "Casting to",
    "the current track": "The current",
    track: "Track",
    "n sec ago": "{n} sec ago",
    "in n sec": "in {n} sec",
    at: "at",
    logs: "Logs",
    player: "Player",
    protocol: "Protocol",
    language: "Language",
    theme: "Theme",
    "is playing": "is playing",
    "player control": "Player control",
    reload: "Reload",
    "next combo": "Next combo",
    "playback score": "Playback score",
    "corrupted frames": "Corrupted frames",
    "dropped frames": "Dropped frames",
    "total frames": "Total frames",
    "decoded audio": "Decoded audio",
    "decoded video": "Decoded video",
    nack: "Negative acknowledgements",
    "picture losses": "Picture losses",
    "packets lost": "Packets lost",
    "packets received": "Packets received",
    "bytes received": "Bytes received",
    "local latency": "Local latency",
    "messages received": "Messages received",
    "messages sent": "Messages sent",
    "current bitrate": "Current bitrate",
    "framerate in": "Framerate in",
    "framerate out": "Framerate out",
    "playback speed": "Playback speed",
    normal: "Normal",
    "picture-in-picture": "Picture-in-picture",
    "copy video url": "Copy video URL",
    copied: "Copied",
    "waiting for stream": "Waiting for stream\u2026",
    retry: "Retry",
    "stream status": "Stream status",
    "mistserver logo": "MistServer logo"
  },
  nl: {
    play: "Afspelen",
    pause: "Pauzeren",
    mute: "Dempen",
    unmute: "Dempen opheffen",
    volume: "Volume",
    fullscreen: "Volledig scherm",
    "exit fullscreen": "Volledig scherm sluiten",
    pip: "Beeld in beeld",
    loop: "Herhalen",
    settings: "Instellingen",
    automatic: "Automatisch",
    none: "Geen",
    live: "live",
    "current time": "Huidige tijd",
    "total time": "Totale tijd",
    "seek bar": "Spoelbalk",
    "seek forward": "Vooruitspoelen",
    "seek backward": "Terugspoelen",
    "-10s": "-10s",
    "+10s": "+10s",
    airplay: "AirPlay",
    speed: "Snelheid",
    "speed doubled": "Snelheid verdubbeld",
    muted: "Gedempt",
    "(muted)": "(gedempt)",
    "seek backward seconds": "- 10 seconden",
    "seek forward seconds": "+ 10 seconden",
    "to start": "Naar begin..",
    "to end": "Naar einde..",
    "frame forward": "Frame +1",
    "frame backward": "Frame -1",
    "reload video": "Video herladen",
    "reload player": "Speler herladen",
    "next source": "Volgende bron",
    ignore: "Negeren",
    "player encountered a problem": "De speler heeft een probleem ondervonden",
    "chromecast encountered a problem": "De Chromecast heeft een probleem ondervonden",
    "stop casting": "Stop met casten",
    chromecast: "Chromecast",
    "select cast device": "Selecteer een apparaat om naar te casten",
    "casting to": "Casten naar",
    "the current track": "De huidige",
    track: "Track",
    "n sec ago": "{n} sec geleden",
    "in n sec": "over {n} sec",
    at: "om",
    logs: "Logs",
    player: "Speler",
    protocol: "Protocol",
    language: "Taal",
    theme: "Thema",
    "is playing": "speelt af via",
    "player control": "Speler bediening",
    reload: "Herladen",
    "next combo": "Volgende combi",
    "playback score": "Afspeelscore",
    "corrupted frames": "Beschadigde frames",
    "dropped frames": "Verloren frames",
    "total frames": "Totaal frames",
    "decoded audio": "Gedecodeerde audio",
    "decoded video": "Gedecodeerde video",
    nack: "NACK",
    "picture losses": "Beeldverlies",
    "packets lost": "Verloren pakketten",
    "packets received": "Ontvangen pakketten",
    "bytes received": "Ontvangen bytes",
    "local latency": "Lokale latentie",
    "messages received": "Ontvangen berichten",
    "messages sent": "Verzonden berichten",
    "current bitrate": "Huidige bitrate",
    "framerate in": "Framerate in",
    "framerate out": "Framerate uit",
    "playback speed": "Afspeelsnelheid",
    normal: "Normaal",
    "picture-in-picture": "Beeld-in-beeld",
    "copy video url": "Video-URL kopi\xEBren",
    copied: "Gekopieerd",
    "waiting for stream": "Wachten op stream\u2026",
    retry: "Opnieuw",
    "stream status": "Streamstatus",
    "mistserver logo": "MistServer-logo"
  },
  de: {
    play: "Abspielen",
    pause: "Pause",
    mute: "Stummschalten",
    unmute: "Ton einschalten",
    volume: "Lautst\xE4rke",
    fullscreen: "Vollbild",
    "exit fullscreen": "Vollbild beenden",
    pip: "Bild im Bild",
    loop: "Wiederholen",
    settings: "Einstellungen",
    automatic: "Automatisch",
    none: "Keine",
    live: "live",
    "current time": "Aktuelle Zeit",
    "total time": "Gesamtzeit",
    "seek bar": "Zeitleiste",
    "seek forward": "Vorspulen",
    "seek backward": "Zur\xFCckspulen",
    "-10s": "-10s",
    "+10s": "+10s",
    airplay: "AirPlay",
    speed: "Geschwindigkeit",
    "speed doubled": "Geschwindigkeit verdoppelt",
    muted: "Stumm",
    "(muted)": "(stumm)",
    "seek backward seconds": "- 10 Sekunden",
    "seek forward seconds": "+ 10 Sekunden",
    "to start": "Zum Anfang..",
    "to end": "Zum Ende..",
    "frame forward": "Frame +1",
    "frame backward": "Frame -1",
    "reload video": "Video neu laden",
    "reload player": "Player neu laden",
    "next source": "N\xE4chste Quelle",
    ignore: "Ignorieren",
    "player encountered a problem": "Der Player hat ein Problem festgestellt",
    "chromecast encountered a problem": "Der Chromecast hat ein Problem festgestellt",
    "stop casting": "\xDCbertragung beenden",
    chromecast: "Chromecast",
    "select cast device": "Ger\xE4t zum \xDCbertragen ausw\xE4hlen",
    "casting to": "\xDCbertragen auf",
    "the current track": "Die aktuelle",
    track: "Track",
    "n sec ago": "vor {n} Sek",
    "in n sec": "in {n} Sek",
    at: "um",
    logs: "Logs",
    player: "Player",
    protocol: "Protokoll",
    language: "Sprache",
    theme: "Thema",
    "is playing": "spielt ab \xFCber",
    "player control": "Player-Steuerung",
    reload: "Neu laden",
    "next combo": "N\xE4chste Kombi",
    "playback score": "Wiedergabe-Score",
    "corrupted frames": "Besch\xE4digte Frames",
    "dropped frames": "Verworfene Frames",
    "total frames": "Frames gesamt",
    "decoded audio": "Decodiertes Audio",
    "decoded video": "Decodiertes Video",
    nack: "NACK",
    "picture losses": "Bildverluste",
    "packets lost": "Verlorene Pakete",
    "packets received": "Empfangene Pakete",
    "bytes received": "Empfangene Bytes",
    "local latency": "Lokale Latenz",
    "messages received": "Empfangene Nachrichten",
    "messages sent": "Gesendete Nachrichten",
    "current bitrate": "Aktuelle Bitrate",
    "framerate in": "Bildrate ein",
    "framerate out": "Bildrate aus",
    "playback speed": "Wiedergabegeschwindigkeit",
    normal: "Normal",
    "picture-in-picture": "Bild-in-Bild",
    "copy video url": "Video-URL kopieren",
    copied: "Kopiert",
    "waiting for stream": "Warten auf Stream\u2026",
    retry: "Erneut versuchen",
    "stream status": "Stream-Status",
    "mistserver logo": "MistServer-Logo"
  },
  fr: {
    play: "Lecture",
    pause: "Pause",
    mute: "Couper le son",
    unmute: "R\xE9tablir le son",
    volume: "Volume",
    fullscreen: "Plein \xE9cran",
    "exit fullscreen": "Quitter le plein \xE9cran",
    pip: "Image dans l\u2019image",
    loop: "Boucle",
    settings: "Param\xE8tres",
    automatic: "Automatique",
    none: "Aucun",
    live: "en direct",
    "current time": "Temps actuel",
    "total time": "Dur\xE9e totale",
    "seek bar": "Barre de progression",
    "seek forward": "Avancer",
    "seek backward": "Reculer",
    "-10s": "-10s",
    "+10s": "+10s",
    airplay: "AirPlay",
    speed: "Vitesse",
    "speed doubled": "Vitesse doubl\xE9e",
    muted: "Son coup\xE9",
    "(muted)": "(son coup\xE9)",
    "seek backward seconds": "- 10 secondes",
    "seek forward seconds": "+ 10 secondes",
    "to start": "Au d\xE9but..",
    "to end": "\xC0 la fin..",
    "frame forward": "Image +1",
    "frame backward": "Image -1",
    "reload video": "Recharger la vid\xE9o",
    "reload player": "Recharger le lecteur",
    "next source": "Source suivante",
    ignore: "Ignorer",
    "player encountered a problem": "Le lecteur a rencontr\xE9 un probl\xE8me",
    "chromecast encountered a problem": "Le Chromecast a rencontr\xE9 un probl\xE8me",
    "stop casting": "Arr\xEAter la diffusion",
    chromecast: "Chromecast",
    "select cast device": "S\xE9lectionner un appareil de diffusion",
    "casting to": "Diffusion sur",
    "the current track": "La piste actuelle",
    track: "Piste",
    "n sec ago": "il y a {n} sec",
    "in n sec": "dans {n} sec",
    at: "\xE0",
    logs: "Journaux",
    player: "Lecteur",
    protocol: "Protocole",
    language: "Langue",
    theme: "Th\xE8me",
    "is playing": "lit via",
    "player control": "Contr\xF4le du lecteur",
    reload: "Recharger",
    "next combo": "Combinaison suivante",
    "playback score": "Score de lecture",
    "corrupted frames": "Images corrompues",
    "dropped frames": "Images perdues",
    "total frames": "Total des images",
    "decoded audio": "Audio d\xE9cod\xE9",
    "decoded video": "Vid\xE9o d\xE9cod\xE9e",
    nack: "Accus\xE9s de non-r\xE9ception",
    "picture losses": "Pertes d\u2019images",
    "packets lost": "Paquets perdus",
    "packets received": "Paquets re\xE7us",
    "bytes received": "Octets re\xE7us",
    "local latency": "Latence locale",
    "messages received": "Messages re\xE7us",
    "messages sent": "Messages envoy\xE9s",
    "current bitrate": "D\xE9bit actuel",
    "framerate in": "FPS entr\xE9e",
    "framerate out": "FPS sortie",
    "playback speed": "Vitesse de lecture",
    normal: "Normal",
    "picture-in-picture": "Image dans l'image",
    "copy video url": "Copier l'URL de la vid\xE9o",
    copied: "Copi\xE9",
    "waiting for stream": "En attente du flux\u2026",
    retry: "R\xE9essayer",
    "stream status": "\xC9tat du flux",
    "mistserver logo": "Logo MistServer"
  },
  es: {
    play: "Reproducir",
    pause: "Pausa",
    mute: "Silenciar",
    unmute: "Activar sonido",
    volume: "Volumen",
    fullscreen: "Pantalla completa",
    "exit fullscreen": "Salir de pantalla completa",
    pip: "Imagen en imagen",
    loop: "Bucle",
    settings: "Ajustes",
    automatic: "Autom\xE1tico",
    none: "Ninguno",
    live: "en vivo",
    "current time": "Tiempo actual",
    "total time": "Tiempo total",
    "seek bar": "Barra de progreso",
    "seek forward": "Avanzar",
    "seek backward": "Retroceder",
    "-10s": "-10s",
    "+10s": "+10s",
    airplay: "AirPlay",
    speed: "Velocidad",
    "speed doubled": "Velocidad duplicada",
    muted: "Silenciado",
    "(muted)": "(silenciado)",
    "seek backward seconds": "- 10 segundos",
    "seek forward seconds": "+ 10 segundos",
    "to start": "Al inicio..",
    "to end": "Al final..",
    "frame forward": "Fotograma +1",
    "frame backward": "Fotograma -1",
    "reload video": "Recargar video",
    "reload player": "Recargar reproductor",
    "next source": "Siguiente fuente",
    ignore: "Ignorar",
    "player encountered a problem": "El reproductor ha encontrado un problema",
    "chromecast encountered a problem": "El Chromecast ha encontrado un problema",
    "stop casting": "Detener transmisi\xF3n",
    chromecast: "Chromecast",
    "select cast device": "Seleccionar un dispositivo de transmisi\xF3n",
    "casting to": "Transmitiendo a",
    "the current track": "La pista actual",
    track: "Pista",
    "n sec ago": "hace {n} seg",
    "in n sec": "en {n} seg",
    at: "a las",
    logs: "Registros",
    player: "Reproductor",
    protocol: "Protocolo",
    language: "Idioma",
    theme: "Tema",
    "is playing": "reproduce v\xEDa",
    "player control": "Control del reproductor",
    reload: "Recargar",
    "next combo": "Siguiente combinaci\xF3n",
    "playback score": "Puntuaci\xF3n de reproducci\xF3n",
    "corrupted frames": "Fotogramas da\xF1ados",
    "dropped frames": "Fotogramas perdidos",
    "total frames": "Total de fotogramas",
    "decoded audio": "Audio decodificado",
    "decoded video": "V\xEDdeo decodificado",
    nack: "NACK",
    "picture losses": "P\xE9rdidas de imagen",
    "packets lost": "Paquetes perdidos",
    "packets received": "Paquetes recibidos",
    "bytes received": "Bytes recibidos",
    "local latency": "Latencia local",
    "messages received": "Mensajes recibidos",
    "messages sent": "Mensajes enviados",
    "current bitrate": "Velocidad de bits actual",
    "framerate in": "FPS entrada",
    "framerate out": "FPS salida",
    "playback speed": "Velocidad de reproducci\xF3n",
    normal: "Normal",
    "picture-in-picture": "Imagen en imagen",
    "copy video url": "Copiar URL del v\xEDdeo",
    copied: "Copiado",
    "waiting for stream": "Esperando transmisi\xF3n\u2026",
    retry: "Reintentar",
    "stream status": "Estado de la transmisi\xF3n",
    "mistserver logo": "Logo de MistServer"
  }
};

// src/ui/dev-skin.js
MistSkins.dev = {
  structure: MistUtil.object.extend({}, MistSkins["default"].structure, true),
  blueprints: {
    timeout: function() {
      if (this.options.reloadDelay !== false) {
        return MistSkins.default.blueprints.timeout.apply(this, arguments);
      }
      return false;
    },
    log: function() {
      var container = document.createElement("div");
      container.appendChild(document.createTextNode(this.translate("logs")));
      var logsc = document.createElement("div");
      logsc.className = "logs";
      container.appendChild(logsc);
      var logs = document.createElement("table");
      logsc.appendChild(logs);
      var MistVideo2 = this;
      var lastmessage = { message: false };
      var count = false;
      var scroll = true;
      function addMessage(time, message, data) {
        if (!data) {
          data = {};
        }
        if (lastmessage.message == message) {
          count++;
          lastmessage.counter.nodeValue = count;
          if (count == 2 && lastmessage.counter.parentElement) {
            lastmessage.counter.parentElement.style.display = "";
          }
          return;
        }
        count = 1;
        var entry = document.createElement("tr");
        entry.className = "entry";
        if (data.type && data.type != "log") {
          MistUtil.class.add(entry, "type-" + data.type);
          message = MistUtil.format.ucFirst(data.type) + ": " + message;
        }
        logs.appendChild(entry);
        var timestamp = document.createElement("td");
        timestamp.className = "timestamp";
        entry.appendChild(timestamp);
        var stamp = time.toLocaleTimeString();
        var t = stamp.split(" ");
        t[0] += "." + ("00" + time.getMilliseconds()).slice(-3);
        timestamp.appendChild(document.createTextNode(t[0]));
        if ("currentTime" in data) {
          timestamp.title = "Video playback time: " + MistUtil.format.time(data.currentTime, { ms: true });
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
        if (scroll) {
          logsc.scrollTop = logsc.scrollHeight;
        }
        lastmessage = { message, counter: countnode };
      }
      MistUtil.event.addListener(logsc, "scroll", function() {
        if (logsc.scrollTop + logsc.clientHeight >= logsc.scrollHeight - 5) {
          scroll = true;
        } else {
          scroll = false;
        }
      });
      for (var i2 in MistVideo2.logs) {
        addMessage(MistVideo2.logs[i2].time, MistVideo2.logs[i2].message, MistVideo2.logs[i2].data);
      }
      MistUtil.event.addListener(MistVideo2.options.target, "log", function(e) {
        if (!e.message) {
          return;
        }
        var data = {};
        if (MistVideo2.player && MistVideo2.player.api && "currentTime" in MistVideo2.player.api) {
          data.currentTime = MistVideo2.player.api.currentTime;
        }
        addMessage(/* @__PURE__ */ new Date(), e.message, data);
      }, container);
      MistUtil.event.addListener(MistVideo2.options.target, "error", function(e) {
        if (!e.message) {
          return;
        }
        var data = { type: "error" };
        if (MistVideo2.player && MistVideo2.player.api && "currentTime" in MistVideo2.player.api) {
          data.currentTime = MistVideo2.player.api.currentTime;
        }
        addMessage(/* @__PURE__ */ new Date(), e.message, data);
      }, container);
      return container;
    },
    decodingIssues: function() {
      if (!this.player) {
        return;
      }
      var MistVideo2 = this;
      var container = document.createElement("div");
      function buildItem(options) {
        var label = document.createElement("label");
        container.appendChild(label);
        label.style.display = "none";
        var text = document.createElement("span");
        label.appendChild(text);
        text.appendChild(document.createTextNode(options.name + ":"));
        text.className = "mistvideo-description";
        var valuec = document.createElement("span");
        label.appendChild(valuec);
        var value = document.createTextNode(options.value ? options.value : "");
        valuec.appendChild(value);
        var ele = document.createElement("span");
        valuec.appendChild(ele);
        label.set = function(val) {
          if (val !== 0 && (typeof val != "object" || MistUtil.object.keys(val).length > 0)) {
            this.style.display = "";
          } else return;
          if (typeof val == "object") {
            try {
              if (val instanceof Promise) {
                val.then(function(val2) {
                  label.set(val2);
                }, function() {
                });
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
            } catch (e) {
            }
            if ("val" in val) {
              value.nodeValue = val.val;
              valuec.className = "value";
            }
            if (ele.children.length) {
              var graph = ele.children[0];
              return graph.addData(val);
            } else {
              var graph = MistUtil.createGraph({ x: [val.x], y: [val.y] }, val.options);
              ele.style.display = "";
              MistUtil.empty(ele);
              return ele.appendChild(graph);
            }
          }
          return value.nodeValue = val;
        };
        container.appendChild(label);
        updates.push(function() {
          var result = options.function();
          label.set(result);
        });
      }
      if (MistVideo2.player.api) {
        let processMultiOutput = function(values, formatter) {
          if (typeof values == "number") {
            return formatter(values);
          }
          var out = document.createElement("div");
          for (var i3 in values) {
            if (values[i3]) {
              var c = document.createElement("div");
              var l = document.createElement("span");
              l.className = "mistvideo-description";
              c.appendChild(l);
              l.appendChild(document.createTextNode(i3[0] + ": "));
              l.setAttribute("title", i3);
              var v = document.createElement("span");
              c.appendChild(v);
              v.appendChild(document.createTextNode(formatter(values[i3])));
              out.appendChild(c);
            }
          }
          return out.children.length ? out : 0;
        };
        var videovalues = {
          "playback score": function() {
            if ("monitor" in MistVideo2) {
              if ("vars" in MistVideo2.monitor && "score" in MistVideo2.monitor.vars) {
                if (MistVideo2.monitor.vars.values.length) {
                  var last = MistVideo2.monitor.vars.values[MistVideo2.monitor.vars.values.length - 1];
                  if ("score" in last) {
                    var score = Math.min(1, Math.max(0, last.score));
                    return {
                      x: last.clock,
                      y: Math.min(1, Math.max(0, last.score)),
                      options: {
                        y: {
                          min: 0,
                          max: 1
                        },
                        x: {
                          count: 10
                        }
                      },
                      val: Math.round(Math.min(1, Math.max(0, MistVideo2.monitor.vars.score)) * 100) + "%"
                    };
                  }
                }
              }
              return 0;
            }
          },
          "corrupted frames": function() {
            if (MistVideo2.player.api && "getVideoPlaybackQuality" in MistVideo2.player.api) {
              var r = MistVideo2.player.api.getVideoPlaybackQuality();
              if (r) {
                if (r.corruptedVideoFrames) {
                  return {
                    val: MistUtil.format.number(r.corruptedVideoFrames),
                    x: (/* @__PURE__ */ new Date()).getTime() * 1e-3,
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
          "dropped frames": function() {
            if (MistVideo2.player.api) {
              if ("getVideoPlaybackQuality" in MistVideo2.player.api) {
                var r = MistVideo2.player.api.getVideoPlaybackQuality();
                if (r) {
                  if (r.droppedVideoFrames) {
                    return MistUtil.format.number(r.droppedVideoFrames);
                  }
                  return 0;
                }
              }
              if ("webkitDroppedFrameCount" in MistVideo2.player.api) {
                return MistVideo2.player.api.webkitDroppedFrameCount;
              }
            }
          },
          "total frames": function() {
            if (MistVideo2.player.api && "getVideoPlaybackQuality" in MistVideo2.player.api) {
              var r = MistVideo2.player.api.getVideoPlaybackQuality();
              if (r) {
                return MistUtil.format.number(r.totalVideoFrames);
              }
            }
          },
          "decoded audio": function() {
            if (MistVideo2.player.api) {
              return MistUtil.format.bytes(MistVideo2.player.api.webkitAudioDecodedByteCount);
            }
          },
          "decoded video": function() {
            if (MistVideo2.player.api) {
              return MistUtil.format.bytes(MistVideo2.player.api.webkitVideoDecodedByteCount);
            }
          },
          "nack": function() {
            if (MistVideo2.player.api && MistVideo2.player.api.nackCount) {
              return processMultiOutput(MistVideo2.player.api.nackCount, MistUtil.format.number);
            }
          },
          "picture losses": function() {
            if (MistVideo2.player.api && MistVideo2.player.api.pliCount) {
              return processMultiOutput(MistVideo2.player.api.pliCount, MistUtil.format.number);
            }
          },
          "packets lost": function() {
            if (MistVideo2.player.api && MistVideo2.player.api.packetsLost) {
              return processMultiOutput(MistVideo2.player.api.packetsLost, MistUtil.format.number);
            }
          },
          "packets received": function() {
            if (MistVideo2.player.api && MistVideo2.player.api.packetsReceived) {
              return processMultiOutput(MistVideo2.player.api.packetsReceived, MistUtil.format.number);
            }
          },
          "bytes received": function() {
            if (MistVideo2.player.api && MistVideo2.player.api.bytesReceived) {
              return processMultiOutput(MistVideo2.player.api.bytesReceived, function(v) {
                return MistUtil.format.bytes(v);
              });
            }
          },
          "local latency": function() {
            if (MistVideo2.player.api && MistVideo2.player.api.jitterDelay) {
              return processMultiOutput(MistVideo2.player.api.jitterDelay, function(v) {
                return MistUtil.format.number(v * 1e3) + "ms";
              });
            }
          },
          "messages received": function() {
            if (MistVideo2.player.api && MistVideo2.player.api.messagesReceived) {
              return processMultiOutput(MistVideo2.player.api.messagesReceived, MistUtil.format.number);
            }
          },
          "messages sent": function() {
            if (MistVideo2.player.api && MistVideo2.player.api.messagesSent) {
              return processMultiOutput(MistVideo2.player.api.messagesSent, MistUtil.format.number);
            }
          },
          "current bitrate": function() {
            if (MistVideo2.player.monitor && "currentBps" in MistVideo2.player.monitor) {
              var out = MistUtil.format.bits(MistVideo2.player.monitor.currentBps);
              return out ? out + "ps" : out;
            }
            if (MistVideo2.player.api && "currentBps" in MistVideo2.player.api) {
              var out = MistUtil.format.bits(MistVideo2.player.api.currentBps());
              return out ? out + "ps" : out;
            }
          },
          "framerate in": function() {
            if (MistVideo2.player.api && "framerate_in" in MistVideo2.player.api) {
              return MistUtil.format.number(MistVideo2.player.api.framerate_in());
            }
          },
          "framerate out": function() {
            if (MistVideo2.player.api && "framerate_out" in MistVideo2.player.api) {
              return MistUtil.format.number(MistVideo2.player.api.framerate_out());
            }
          }
        };
        var updates = [];
        for (var i2 in videovalues) {
          if (typeof videovalues[i2]() == "undefined") {
            continue;
          }
          buildItem({
            name: MistVideo2.translate(i2),
            function: videovalues[i2]
          });
        }
        container.update = function() {
          for (var i3 in updates) {
            updates[i3]();
          }
          MistVideo2.timers.start(function() {
            container.update();
          }, 1e3);
        };
        container.update();
      }
      return container;
    },
    forcePlayer: function() {
      var container = document.createElement("div");
      container.className = "mistvideo-force-row";
      container.title = "Reload MistVideo and use the selected player";
      var MistVideo2 = this;
      var label = document.createElement("span");
      label.className = "mistvideo-force-label";
      label.textContent = MistVideo2.translate("player") + ":";
      container.appendChild(label);
      var menuItems = [{ value: "", label: MistVideo2.translate("automatic") }];
      for (var i2 in mistplayers) {
        menuItems.push({ value: i2, label: mistplayers[i2].name });
      }
      var current = this.options.forcePlayer || "";
      var menuWrap = document.createElement("div");
      menuWrap.style.position = "relative";
      menuWrap.style.display = "inline-block";
      container.appendChild(menuWrap);
      var menuBtn = document.createElement("button");
      menuBtn.className = "mistvideo-menu-trigger";
      menuBtn.textContent = MistVideo2.translate("automatic");
      for (var m = 0; m < menuItems.length; m++) {
        if (menuItems[m].value === current) {
          menuBtn.textContent = menuItems[m].label;
          break;
        }
      }
      menuWrap.appendChild(menuBtn);
      var menu = createMenu(MistVideo2, {
        type: "forcePlayer",
        items: menuItems,
        selected: current,
        onChange: function(val) {
          for (var m2 = 0; m2 < menuItems.length; m2++) {
            if (menuItems[m2].value === val) {
              menuBtn.textContent = menuItems[m2].label;
              break;
            }
          }
          MistVideo2.options.forcePlayer = val === "" ? false : val;
          if (MistVideo2.options.forcePlayer != MistVideo2.playerName) {
            MistVideo2.reload("Reloading to force player.");
          }
        }
      });
      menuWrap.appendChild(menu);
      menuBtn.addEventListener("click", function(e) {
        e.stopPropagation();
        menu.toggle();
      });
      return container;
    },
    forceType: function() {
      if (!this.info) {
        return;
      }
      var container = document.createElement("div");
      container.className = "mistvideo-force-row";
      container.title = "Reload MistVideo and use the selected protocol";
      var MistVideo2 = this;
      var label = document.createElement("span");
      label.className = "mistvideo-force-label";
      label.textContent = MistVideo2.translate("protocol") + ":";
      container.appendChild(label);
      var menuItems = [{ value: "", label: MistVideo2.translate("automatic") }];
      var sofar = {};
      for (var i2 in MistVideo2.info.source) {
        var source = MistVideo2.info.source[i2];
        if (source.type in sofar) {
          continue;
        }
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
      menuBtn.textContent = MistVideo2.translate("automatic");
      for (var m = 0; m < menuItems.length; m++) {
        if (menuItems[m].value === current) {
          menuBtn.textContent = menuItems[m].label;
          break;
        }
      }
      menuWrap.appendChild(menuBtn);
      var menu = createMenu(MistVideo2, {
        type: "forceType",
        items: menuItems,
        selected: current,
        onChange: function(val) {
          for (var m2 = 0; m2 < menuItems.length; m2++) {
            if (menuItems[m2].value === val) {
              menuBtn.textContent = menuItems[m2].label;
              break;
            }
          }
          MistVideo2.options.forceType = val === "" ? false : val;
          if (!MistVideo2.source || MistVideo2.options.forceType != MistVideo2.source.type) {
            MistVideo2.reload("Reloading to force new type.");
          }
        }
      });
      menuWrap.appendChild(menu);
      menuBtn.addEventListener("click", function(e) {
        e.stopPropagation();
        menu.toggle();
      });
      return container;
    },
    forceLanguage: function() {
      var container = document.createElement("div");
      container.className = "mistvideo-force-row";
      container.title = "Reload MistVideo with the selected language";
      var MistVideo2 = this;
      var label = document.createElement("span");
      label.className = "mistvideo-force-label";
      label.textContent = MistVideo2.translate("language") + ":";
      container.appendChild(label);
      var langNames = { en: "English", nl: "Nederlands", de: "Deutsch", fr: "Fran\xE7ais", es: "Espa\xF1ol" };
      var menuItems = [{ value: "", label: MistVideo2.translate("automatic") }];
      for (var code in locales) {
        menuItems.push({ value: code, label: langNames[code] || code });
      }
      var current = "";
      if (typeof MistVideo2.options.translations === "object" && MistVideo2.options.translations) {
        for (var code in locales) {
          if (MistVideo2.options.translations === locales[code]) {
            current = code;
            break;
          }
        }
      }
      var menuWrap = document.createElement("div");
      menuWrap.style.position = "relative";
      menuWrap.style.display = "inline-block";
      container.appendChild(menuWrap);
      var menuBtn = document.createElement("button");
      menuBtn.className = "mistvideo-menu-trigger";
      menuBtn.textContent = MistVideo2.translate("automatic");
      for (var m = 0; m < menuItems.length; m++) {
        if (menuItems[m].value === current) {
          menuBtn.textContent = menuItems[m].label;
          break;
        }
      }
      menuWrap.appendChild(menuBtn);
      var menu = createMenu(MistVideo2, {
        type: "forceLanguage",
        items: menuItems,
        selected: current,
        onChange: function(val) {
          for (var m2 = 0; m2 < menuItems.length; m2++) {
            if (menuItems[m2].value === val) {
              menuBtn.textContent = menuItems[m2].label;
              break;
            }
          }
          MistVideo2.options.translations = val === "" ? false : locales[val];
          MistVideo2.reload("Reloading to apply language.");
        }
      });
      menuWrap.appendChild(menu);
      menuBtn.addEventListener("click", function(e) {
        e.stopPropagation();
        menu.toggle();
      });
      return container;
    },
    forceTheme: function() {
      var container = document.createElement("div");
      container.className = "mistvideo-force-row";
      container.title = "Apply a preset theme";
      var MistVideo2 = this;
      var currentTheme = "default";
      var currentMode = "auto";
      var lastTokenKeys = null;
      var label = document.createElement("span");
      label.className = "mistvideo-force-label";
      label.textContent = MistVideo2.translate("theme") + ":";
      container.appendChild(label);
      function effectiveMode() {
        if (currentMode !== "auto") return currentMode;
        return window.matchMedia && window.matchMedia("(prefers-color-scheme: light)").matches ? "light" : "dark";
      }
      function apply() {
        if (!MistVideo2.container) return;
        if (lastTokenKeys) {
          for (var i2 = 0; i2 < lastTokenKeys.length; i2++) {
            MistVideo2.container.style.removeProperty(lastTokenKeys[i2]);
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
          MistVideo2.container.style.setProperty(prop, tokens[key]);
          lastTokenKeys.push(prop);
        }
      }
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
      var themeMenu = createMenu(MistVideo2, {
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
      themeBtn.addEventListener("click", function(e) {
        e.stopPropagation();
        themeMenu.toggle();
      });
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
      var modeMenu = createMenu(MistVideo2, {
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
      modeBtn.addEventListener("click", function(e) {
        e.stopPropagation();
        modeMenu.toggle();
      });
      if (window.matchMedia) {
        window.matchMedia("(prefers-color-scheme: light)").addEventListener("change", function() {
          if (currentMode === "auto") apply();
        });
      }
      return container;
    },
    forceSource: function() {
      var container = document.createElement("label");
      container.title = "Reload MistVideo and use the selected source";
      var MistVideo2 = this;
      var s = document.createElement("span");
      container.appendChild(s);
      s.appendChild(document.createTextNode("Force source: "));
      var select = document.createElement("select");
      container.appendChild(select);
      var option = document.createElement("option");
      select.appendChild(option);
      option.value = "";
      option.appendChild(document.createTextNode("Automatic"));
      for (var i2 in MistVideo2.info.source) {
        var source = MistVideo2.info.source[i2];
        var option = document.createElement("option");
        select.appendChild(option);
        option.value = i2;
        option.appendChild(document.createTextNode(source.url + " (" + MistUtil.format.mime2human(source.type) + ")"));
      }
      if (this.options.forceSource) {
        select.value = this.options.forceSource;
      }
      MistUtil.event.addListener(select, "change", function() {
        MistVideo2.options.forceSource = this.value == "" ? false : this.value;
        if (MistVideo2.options.forceSource != MistVideo2.source.index) {
          MistVideo2.reload("Reloading to force new source.");
        }
      });
      return container;
    }
  }
};
MistSkins.dev.css = { skin: "/skins/dev.css" };
MistSkins.dev.blueprints.closeSubmenu = function() {
  var MistVideo2 = this;
  var btn = document.createElement("button");
  btn.innerHTML = "&times;";
  btn.addEventListener("click", function(e) {
    e.stopPropagation();
    MistVideo2.container.removeAttribute("data-show-submenu");
    var menus = MistVideo2.container.querySelectorAll(".mistvideo-menu[data-open]");
    for (var i2 = 0; i2 < menus.length; i2++) {
      menus[i2].style.display = "none";
      menus[i2].removeAttribute("data-open");
    }
  });
  return btn;
};
MistSkins.dev.blueprints.submenu = function() {
  var container = this.UI.buildStructure(this.skin.structure.submenu);
  var MistVideo2 = this;
  setTimeout(function() {
    if (container && container.parentNode && MistVideo2.container) {
      MistVideo2.container.appendChild(container);
      MistUtil.class.remove(container, "inner_window");
    }
  }, 0);
  return container;
};
MistSkins.dev.structure.submenu = MistUtil.object.extend({}, MistSkins["default"].structure.submenu, true);
MistSkins.dev.structure.submenu.type = "draggable";
delete MistSkins.dev.structure.submenu.style.maxWidth;
delete MistSkins.dev.structure.submenu.style.minWidth;
MistSkins.dev.structure.submenu.children.unshift({
  type: "container",
  style: { flexShrink: 1 },
  classes: ["mistvideo-column"],
  children: [
    { type: "closeSubmenu" },
    {
      if: function() {
        return this.playerName && this.source;
      },
      then: {
        type: "container",
        classes: ["mistvideo-description", "mistvideo-displayCombo"],
        style: { display: "block" },
        children: [
          { type: "playername", style: { display: "inline" } },
          { type: "text", i18n: "is playing", style: { margin: "0 0.2em" } },
          { type: "mimetype" }
        ]
      }
    },
    { type: "log" },
    { type: "decodingIssues" },
    {
      type: "container",
      classes: ["mistvideo-column", "mistvideo-devcontrols"],
      style: { "font-size": "0.9em" },
      children: [
        {
          type: "text",
          i18n: "player control"
        },
        {
          type: "container",
          classes: ["mistvideo-devbuttons"],
          style: { "flex-wrap": "wrap" },
          children: [
            {
              type: "button",
              title: "Build MistVideo again",
              i18n: "reload",
              label: "Reload",
              onclick: function() {
                this.reload("Dev-reload button clicked.");
              }
            },
            {
              type: "button",
              title: "Switch to the next available player and source combination",
              i18n: "next combo",
              label: "Next combo",
              onclick: function() {
                this.nextCombo();
              }
            }
          ]
        },
        { type: "forcePlayer" },
        { type: "forceType" },
        { type: "forceLanguage" },
        { type: "forceTheme" }
        //,
        //{type:"forceSource"}
      ]
    }
  ]
});

// src/ui/skin-loader.js
function MistSkin(MistVideo2) {
  MistVideo2.skin = this;
  this.applySkinOptions = function(skinOptions) {
    if (typeof skinOptions == "string" && skinOptions in MistSkins) {
      skinOptions = MistUtil.object.extend({}, MistSkins[skinOptions], true);
    }
    var skinParent;
    if ("inherit" in skinOptions && skinOptions.inherit && skinOptions.inherit in MistSkins) {
      skinParent = this.applySkinOptions(skinOptions.inherit);
    } else {
      skinParent = MistSkins.default;
    }
    this.structure = MistUtil.object.extend({}, skinParent.structure);
    if (skinOptions && "structure" in skinOptions) {
      MistUtil.object.extend(this.structure, skinOptions.structure);
    }
    this.blueprints = MistUtil.object.extend({}, skinParent.blueprints);
    if (skinOptions && "blueprints" in skinOptions) {
      MistUtil.object.extend(this.blueprints, skinOptions.blueprints);
    }
    this.icons = MistUtil.object.extend({}, skinParent.icons, true);
    if (skinOptions && "icons" in skinOptions) {
      MistUtil.object.extend(this.icons.blueprints, skinOptions.icons);
    }
    this.icons.build = function(type, size, options) {
      if (!size) {
        size = 22;
      }
      var d = this.blueprints[type];
      var svg;
      if (typeof d.svg == "function") {
        svg = d.svg.call(MistVideo2, options);
      } else {
        svg = d.svg;
      }
      if (typeof size != "object") {
        size = {
          height: size,
          width: size
        };
      }
      if (typeof d.size != "object") {
        d.size = {
          height: d.size,
          width: d.size
        };
      }
      if (!("width" in size) && "height" in size || !("height" in size) && "width" in size) {
        if ("width" in size) {
          size.height = size.width * d.size.height / d.size.width;
        }
        if ("height" in size) {
          size.width = size.height * d.size.width / d.size.height;
        }
      }
      var str = "";
      str += '<svg viewBox="0 0 ' + d.size.width + " " + d.size.height + '"' + ("width" in size ? ' width="' + size.width + '"' : "") + ("height" in size ? ' height="' + size.height + '"' : "") + ' class="mist icon ' + type + '">';
      str += '<svg xmlns="http://www.w3.org/2000/svg" version="1.1" height="100%" width="100%">';
      str += svg;
      str += "</svg>";
      str += "</svg>";
      var container = document.createElement("div");
      container.innerHTML = str;
      return container.firstChild;
    };
    this.colors = MistUtil.object.extend({}, skinParent.colors);
    if (skinOptions && "colors" in skinOptions) {
      MistUtil.object.extend(this.colors, skinOptions.colors, true);
    }
    this.tokens = MistUtil.object.extend({}, skinParent.tokens || {});
    if (skinOptions && "tokens" in skinOptions) {
      MistUtil.object.extend(this.tokens, skinOptions.tokens);
    }
    this.css = MistUtil.object.extend({}, skinParent.css);
    if (skinOptions && "css" in skinOptions) {
      MistUtil.object.extend(this.css, skinOptions.css);
    }
    return this;
  };
  this.applySkinOptions("skin" in MistVideo2.options ? MistVideo2.options.skin : "default");
  if (MistVideo2.options.theme) {
    var themeTokens = resolveTheme(MistVideo2.options.theme, MistVideo2.options.themeMode);
    if (themeTokens) {
      MistUtil.object.extend(this.tokens, themeTokens);
    }
  }
  this.setToken = function(name, value) {
    if (MistVideo2.container) {
      MistVideo2.container.style.setProperty(
        name.indexOf("--") === 0 ? name : "--mist-" + name,
        value
      );
    }
  };
  this.setTokens = function(tokens) {
    for (var name in tokens) {
      this.setToken(name, tokens[name]);
    }
  };
  var styles = [];
  for (var i2 in this.css) {
    if (typeof this.css[i2] == "string") {
      var cssUrl = this.css[i2];
      if (MistVideo2.options.host && !/^https?:\/\//.test(cssUrl)) {
        var path = cssUrl.replace(/^(\.\.\/)+/, "");
        if (path[0] !== "/") path = "/" + path;
        cssUrl = MistVideo2.options.host.replace(/\/$/, "") + path;
      }
      var a = MistUtil.css.load(MistVideo2.urlappend(cssUrl), this.colors);
      styles.push(a);
    }
  }
  this.css = styles;
  return;
}

// src/ui/controls.js
function MistUI(MistVideo2, structure) {
  MistVideo2.UI = this;
  this.elements = [];
  this.buildStructure = function(structure2) {
    if (typeof structure2 == "function") {
      structure2 = structure2.call(MistVideo2);
    }
    if ("if" in structure2) {
      var result = false;
      if (structure2.if.call(MistVideo2, structure2)) {
        result = structure2.then;
      } else if ("else" in structure2) {
        result = structure2.else;
      }
      if (!result) {
        return;
      }
      for (var i3 in structure2) {
        if (["if", "then", "else"].indexOf(i3) < 0) {
          if (i3 in result) {
            if (!(result[i3] instanceof Array)) {
              result[i3] = [result[i3]];
            }
            result[i3] = result[i3].concat(structure2[i3]);
          } else {
            result[i3] = structure2[i3];
          }
        }
      }
      return this.buildStructure(result);
    }
    if ("type" in structure2) {
      if (structure2.type in MistVideo2.skin.blueprints) {
        var container2 = MistVideo2.skin.blueprints[structure2.type].call(MistVideo2, structure2);
        if (!container2) {
          return;
        }
        MistUtil.class.add(container2, "mistvideo-" + structure2.type);
        if ("css" in structure2) {
          var uid2 = MistUtil.createUnique();
          structure2.css = [].concat(structure2.css);
          for (var i3 in structure2.css) {
            var style2 = MistUtil.css.createStyle(structure2.css[i3], uid2);
            container2.appendChild(style2);
          }
          MistUtil.class.add(container2, uid2);
          container2.uid = uid2;
        }
        if ("classes" in structure2) {
          for (var i3 in structure2.classes) {
            MistUtil.class.add(container2, structure2.classes[i3]);
          }
        }
        if ("title" in structure2) {
          container2.title = structure2.title;
        }
        if ("style" in structure2) {
          for (var i3 in structure2.style) {
            container2.style[i3] = structure2.style[i3];
          }
        }
        if ("children" in structure2) {
          for (var i3 in structure2.children) {
            var child = this.buildStructure(structure2.children[i3]);
            if (child) {
              container2.appendChild(child);
            }
          }
        }
        MistVideo2.UI.elements.push(container2);
        return container2;
      }
    }
    return false;
  };
  this.build = function() {
    return this.buildStructure(structure ? structure : MistVideo2.skin.structure.main);
  };
  var container = this.build();
  MistUtil.css.applyCustomProperties(container, MistVideo2.skin.colors, MistVideo2.skin.tokens);
  var uid = MistUtil.createUnique();
  var loaded = 0;
  if (MistVideo2.skin.css.length) {
    container.style.opacity = 0;
  }
  for (var i2 in MistVideo2.skin.css) {
    var style = MistVideo2.skin.css[i2];
    style.callback = function(css) {
      if (css == "/*Failed to load*/") {
        this.textContent = css;
        MistVideo2.showError("Failed to load CSS from " + this.getAttribute("data-source"));
      } else {
        this.textContent = MistUtil.css.prependClass(css, uid, true);
      }
      loaded++;
      if (MistVideo2.skin.css.length <= loaded) {
        container.style.opacity = "";
      }
    };
    if (style.textContent != "") {
      style.callback(style.textContent);
    }
    container.appendChild(style);
  }
  MistUtil.class.add(container, uid);
  var browser = MistUtil.getBrowser();
  if (browser) {
    MistUtil.class.add(container, "browser-" + browser);
  }
  return container;
}

// src/core/state.js
function PlayerState(MistVideo2) {
  var listeners = {};
  var state = {
    paused: true,
    playing: false,
    currentTime: 0,
    duration: 0,
    volume: 1,
    muted: false,
    buffered: null,
    seeking: false,
    ended: false,
    loading: true,
    fullscreen: false,
    error: null,
    playbackRate: 1,
    loop: false,
    pip: false,
    tracks: null,
    streamState: null
  };
  function set(prop, value) {
    if (state[prop] === value) return;
    state[prop] = value;
    var cbs = listeners[prop];
    if (cbs) {
      for (var i2 = 0; i2 < cbs.length; i2++) {
        cbs[i2](value, prop);
      }
    }
  }
  this.on = function(prop, callback) {
    if (!(prop in listeners)) listeners[prop] = [];
    listeners[prop].push(callback);
    callback(state[prop], prop);
    return function unsubscribe() {
      var cbs = listeners[prop];
      if (cbs) {
        var idx = cbs.indexOf(callback);
        if (idx >= 0) cbs.splice(idx, 1);
      }
    };
  };
  this.off = function(prop, callback) {
    var cbs = listeners[prop];
    if (cbs) {
      var idx = cbs.indexOf(callback);
      if (idx >= 0) cbs.splice(idx, 1);
    }
  };
  this.get = function(prop) {
    return state[prop];
  };
  this.set = function(prop, value) {
    set(prop, value);
  };
  this.bind = function(video) {
    var eventMap = {
      play: function() {
        set("paused", false);
        set("playing", true);
        set("ended", false);
      },
      pause: function() {
        set("paused", true);
        set("playing", false);
      },
      playing: function() {
        set("loading", false);
        set("playing", true);
      },
      timeupdate: function() {
        set("currentTime", video.currentTime);
      },
      durationchange: function() {
        set("duration", video.duration);
      },
      volumechange: function() {
        set("volume", video.volume);
        set("muted", video.muted);
      },
      ratechange: function() {
        set("playbackRate", video.playbackRate);
      },
      seeking: function() {
        set("seeking", true);
      },
      seeked: function() {
        set("seeking", false);
      },
      ended: function() {
        set("ended", true);
        set("playing", false);
      },
      waiting: function() {
        set("loading", true);
      },
      canplay: function() {
        set("loading", false);
      },
      error: function() {
        set("error", video.error);
      },
      progress: function() {
        set("buffered", video.buffered);
      }
    };
    for (var event in eventMap) {
      MistUtil.event.addListener(video, event, eventMap[event]);
    }
    MistUtil.event.addListener(video, "enterpictureinpicture", function() {
      set("pip", true);
    });
    MistUtil.event.addListener(video, "leavepictureinpicture", function() {
      set("pip", false);
    });
    var fsEvents = ["fullscreenchange", "webkitfullscreenchange", "mozfullscreenchange", "MSFullscreenChange"];
    function onFullscreenChange() {
      var el = document.fullscreenElement || document.webkitFullscreenElement || document.mozFullScreenElement || document.msFullscreenElement;
      set("fullscreen", !!el);
    }
    for (var i2 = 0; i2 < fsEvents.length; i2++) {
      document.addEventListener(fsEvents[i2], onFullscreenChange);
    }
    MistUtil.event.addListener(document, "fakefullscreenchange", function() {
      var container = MistVideo2.container;
      set("fullscreen", !!(container && container.hasAttribute("data-fullscreen")));
    });
    MistUtil.event.addListener(video, "metaUpdate_tracks", function(e) {
      if (e.message && e.message.meta && e.message.meta.tracks) {
        set("tracks", MistUtil.tracks.parse(e.message.meta.tracks));
      }
    });
  };
  this.destroy = function() {
    listeners = {};
  };
}

// src/core/player.js
function MistPlayer() {
}
function mistPlay2(streamName, options) {
  return new MistVideo(streamName, options);
}
function MistVideo(streamName, options) {
  var MistVideo2 = this;
  if (!options) {
    options = {};
  }
  if (typeof mistoptions != "undefined") {
    options = MistUtil.object.extend(MistUtil.object.extend({}, mistoptions), options);
  }
  options = MistUtil.object.extend({
    host: null,
    //override mistserver host (default is the host that player.js is loaded from)
    autoplay: true,
    //start playing when loaded
    controls: true,
    //show controls (MistControls when available)
    keyControls: true,
    //enable keyboard controls (false to disable, "focus" to require focus on the player to capture keys)
    loop: false,
    //don't loop when the stream has finished
    poster: false,
    //don't show an image before the stream has started
    muted: false,
    //don't start muted
    callback: false,
    //don't call a function when the player has finished building
    streaminfo: false,
    //don't use this streaminfo but collect it from the mistserverhost
    startCombo: false,
    //start looking for a player/source match at the start
    forceType: false,
    //don't force a mimetype
    forcePlayer: false,
    //don't force a player
    forceSource: false,
    //don't force a source
    forcePriority: false,
    //no custom priority sorting
    monitor: false,
    //no custom monitoring
    reloadDelay: false,
    //don't override default reload delay
    urlappend: false,
    //don't add this to urls
    setTracks: false,
    //don't set tracks
    fillSpace: false,
    //don't fill parent container; true (grow to fit container if video is smaller) or "cover" (if container aspect is different, clip larger side), "cover_h" (only clip top/bottom) or "cover_v" (only clip left/right)
    width: false,
    //no set width
    height: false,
    //no set height,
    rotate: false,
    //do not rotate; 1: rotate clockwise, -1: counter clockwise, 2: 180 degrees
    maxwidth: false,
    //no max width (apart from targets dimensions)
    maxheight: false,
    //no max height (apart from targets dimensions)
    ABR_resize: true,
    //for supporting wrappers: when the player resizes, request a video track that matches the resolution best
    ABR_bitrate: true,
    //for supporting wrappers: when there are playback issues, request a lower bitrate video track
    useDateTime: true,
    //when the unix timestamp of the stream is known, display the date/time,
    subscribeToMetaTrack: false,
    //pass [[track index,callback]]; the callback function will be called whenever the specified meta data track receives a message. 
    MistVideoObject: false,
    //no reference object is passed
    translations: false
    //override UI strings for i18n, e.g. { play: "Afspelen" } or "nl"
  }, options);
  if (typeof options.translations === "string") {
    options.translations = locales[options.translations] || false;
  }
  if (options.host) {
    options.host = MistUtil.http.url.sanitizeHost(options.host);
  }
  this.options = options;
  this.stream = streamName;
  this.info = false;
  var defaultStrings = {
    // Playback controls
    play: "Play",
    pause: "Pause",
    mute: "Mute",
    unmute: "Unmute",
    volume: "Volume",
    fullscreen: "Full screen",
    "exit fullscreen": "Exit full screen",
    pip: "Picture in picture",
    loop: "Loop",
    settings: "Settings",
    automatic: "Automatic",
    none: "None",
    live: "live",
    "current time": "Current time",
    "total time": "Total time",
    "seek bar": "Seek bar",
    "seek forward": "Seek forward",
    "seek backward": "Seek backward",
    "-10s": "-10s",
    "+10s": "+10s",
    airplay: "AirPlay",
    // Keyboard overlay feedback
    speed: "Speed",
    "speed doubled": "Speed doubled",
    muted: "Muted",
    "(muted)": "(muted)",
    "seek backward seconds": "- 10 seconds",
    "seek forward seconds": "+ 10 seconds",
    "to start": "To start..",
    "to end": "To end..",
    "frame forward": "Frame +1",
    "frame backward": "Frame -1",
    // Error overlay
    "reload video": "Reload video",
    "reload player": "Reload player",
    "next source": "Next source",
    ignore: "Ignore",
    "player encountered a problem": "The player has encountered a problem",
    "chromecast encountered a problem": "The chromecast has encountered a problem",
    // Casting
    "stop casting": "Stop casting",
    chromecast: "Chromecast",
    "select cast device": "Select a device to cast to",
    "casting to": "Casting to",
    // Track selection
    "the current track": "The current",
    track: "Track",
    // Dev skin
    logs: "Logs",
    player: "Player",
    protocol: "Protocol",
    language: "Language",
    theme: "Theme",
    "is playing": "is playing",
    "player control": "Player control",
    reload: "Reload",
    "next combo": "Next combo",
    // Time formatting
    "n sec ago": "{n} sec ago",
    "in n sec": "in {n} sec",
    at: "at",
    // Dev diagnostics
    "playback score": "Playback score",
    "corrupted frames": "Corrupted frames",
    "dropped frames": "Dropped frames",
    "total frames": "Total frames",
    "decoded audio": "Decoded audio",
    "decoded video": "Decoded video",
    nack: "Negative acknowledgements",
    "picture losses": "Picture losses",
    "packets lost": "Packets lost",
    "packets received": "Packets received",
    "bytes received": "Bytes received",
    "local latency": "Local latency",
    "messages received": "Messages received",
    "messages sent": "Messages sent",
    "current bitrate": "Current bitrate",
    "framerate in": "Framerate in",
    "framerate out": "Framerate out",
    // Context menu
    "playback speed": "Playback speed",
    normal: "Normal",
    "picture-in-picture": "Picture-in-picture",
    "copy video url": "Copy video URL",
    copied: "Copied",
    // Idle screen
    "waiting for stream": "Waiting for stream\u2026",
    retry: "Retry",
    "stream status": "Stream status",
    "mistserver logo": "MistServer logo"
  };
  this.translate = function(key, fallback) {
    if (this.options.translations && key in this.options.translations) {
      return this.options.translations[key];
    }
    if (key in defaultStrings) {
      return defaultStrings[key];
    }
    return fallback || key;
  };
  this.playerState = new PlayerState(this);
  var fsCache = null;
  this.fullscreen = {
    get supported() {
      if (!MistVideo2.container && !MistVideo2.video) return false;
      var f = MistVideo2.fullscreen._detect();
      return f !== false;
    },
    get active() {
      var f = MistVideo2.fullscreen._detect();
      if (f) return f.element() === f.target();
      return !!(MistVideo2.container && MistVideo2.container.hasAttribute("data-fullscreen"));
    },
    _detect: function() {
      if (fsCache !== null) return fsCache;
      if (!MistVideo2.container && !MistVideo2.video) return false;
      var requestfuncs = ["requestFullscreen", "webkitRequestFullscreen", "mozRequestFullScreen", "msRequestFullscreen", "webkitEnterFullscreen"];
      var cancelfuncs = ["exitFullscreen", "webkitCancelFullScreen", "mozCancelFullScreen", "msExitFullscreen", "webkitExitFullscreen"];
      var elementfuncs = ["fullscreenElement", "webkitFullscreenElement", "mozFullScreenElement", "msFullscreenElement", "webkitFullscreenElement"];
      var eventnames = ["fullscreenchange", "webkitfullscreenchange", "mozfullscreenchange", "MSFullscreenChange", "webkitfullscreenchange"];
      var targets = [function() {
        return MistVideo2.container;
      }, function() {
        return MistVideo2.video;
      }];
      for (var j = 0; j < targets.length; j++) {
        for (var i2 = 0; i2 < requestfuncs.length; i2++) {
          var el = targets[j]();
          if (el && requestfuncs[i2] in el) {
            fsCache = {
              request: function() {
                return fsCache.target()[requestfuncs[i2]]();
              },
              cancel: function() {
                return document[cancelfuncs[i2]]();
              },
              element: function() {
                return document[elementfuncs[i2]];
              },
              event: eventnames[i2],
              target: targets[j]
            };
            return fsCache;
          }
        }
      }
      fsCache = false;
      return fsCache;
    },
    request: function() {
      var f = MistVideo2.fullscreen._detect();
      if (f) return f.request();
      if (MistVideo2.container) {
        MistVideo2.container.setAttribute("data-fullscreen", "");
        MistUtil.event.send("fakefullscreenchange", null, document);
        if (MistVideo2.player && MistVideo2.player.resizeAll) MistVideo2.player.resizeAll();
        var keydownHandler = function(e) {
          if (e.key === "Escape") MistVideo2.fullscreen.exit();
        };
        MistVideo2.fullscreen._escHandler = keydownHandler;
        document.addEventListener("keydown", keydownHandler);
      }
    },
    exit: function() {
      var f = MistVideo2.fullscreen._detect();
      if (f) return f.cancel();
      if (MistVideo2.container && MistVideo2.container.hasAttribute("data-fullscreen")) {
        MistVideo2.container.removeAttribute("data-fullscreen");
        MistUtil.event.send("fakefullscreenchange", null, document);
        if (MistVideo2.player && MistVideo2.player.resizeAll) MistVideo2.player.resizeAll();
        if (MistVideo2.fullscreen._escHandler) {
          document.removeEventListener("keydown", MistVideo2.fullscreen._escHandler);
          MistVideo2.fullscreen._escHandler = null;
        }
      }
    },
    toggle: function() {
      if (MistVideo2.fullscreen.active) MistVideo2.fullscreen.exit();
      else MistVideo2.fullscreen.request();
    },
    _escHandler: null
  };
  this.pip = {
    get supported() {
      return !!(document.pictureInPictureEnabled && MistVideo2.video && "requestPictureInPicture" in MistVideo2.video);
    },
    get active() {
      return !!document.pictureInPictureElement;
    },
    request: function() {
      if (MistVideo2.video && "requestPictureInPicture" in MistVideo2.video) {
        return MistVideo2.video.requestPictureInPicture();
      }
      return Promise.reject("PiP not supported");
    },
    exit: function() {
      if (document.pictureInPictureElement) {
        return document.exitPictureInPicture();
      }
      return Promise.resolve();
    },
    toggle: function() {
      if (MistVideo2.pip.active) return MistVideo2.pip.exit();
      return MistVideo2.pip.request();
    }
  };
  this.gain = {
    _ctx: null,
    _node: null,
    _source: null,
    _value: 1,
    get value() {
      return this._value;
    },
    set value(v) {
      this._value = Math.max(0, Math.min(2, v));
      if (this._node) this._node.gain.value = this._value;
    },
    init: function() {
      if (MistVideo2.gain._ctx) return;
      if (!MistVideo2.video) return;
      try {
        var ctx = new (window.AudioContext || window.webkitAudioContext)();
        var source = ctx.createMediaElementSource(MistVideo2.video);
        var gainNode = ctx.createGain();
        source.connect(gainNode);
        gainNode.connect(ctx.destination);
        gainNode.gain.value = MistVideo2.gain._value;
        MistVideo2.gain._ctx = ctx;
        MistVideo2.gain._node = gainNode;
        MistVideo2.gain._source = source;
      } catch (e) {
        MistVideo2.log("AudioContext init failed: " + e, "error");
      }
    }
  };
  this.getCapabilities = function() {
    var api = MistVideo2.player && MistVideo2.player.api ? MistVideo2.player.api : null;
    return {
      playbackRate: !!(api && "playbackRate" in api),
      setTracks: !!(api && ("setTrack" in api || "setTracks" in api)),
      subtitles: !!(api && ("setSubtitle" in api || "setWSSubtitle" in api)),
      stats: !!(api && "getStats" in api),
      fullscreen: MistVideo2.fullscreen.supported,
      pip: MistVideo2.pip.supported,
      loop: !!(api && "loop" in api)
    };
  };
  if (!window.MistInstances) {
    window.MistInstances = 0;
  }
  window.MistInstances++;
  this.n = window.MistInstances;
  this.logs = [];
  this.log = function(message, type) {
    if (!type) {
      type = "log";
    }
    var event = MistUtil.event.send(type, message, options.target);
    var data = {
      type
    };
    this.logs.push({
      time: /* @__PURE__ */ new Date(),
      message,
      data
    });
    if (this.options.skin == "dev") {
      try {
        var msg = "[" + (type ? type : "log") + "] " + (MistVideo2.destroyed ? "[DESTROYED] " : "") + "[#" + MistVideo2.n + "] " + (this.player && this.player.api ? MistUtil.format.time(this.player.api.currentTime, { ms: true }) + " " : "") + message;
        if (type && type != "log") {
          console.warn(msg);
        } else {
          console.log(msg);
        }
      } catch (e) {
      }
    }
    return event;
  };
  this.log("Initializing..");
  this.bootMs = (/* @__PURE__ */ new Date()).getTime();
  this.timers = {
    list: {},
    //will contain the timeouts, format timeOutIndex: endTime
    start: function(callback, delay) {
      var i2 = setTimeout(function() {
        delete MistVideo2.timers.list[i2];
        if (MistVideo2.destroyed) return;
        callback();
      }, delay);
      this.list[i2] = new Date((/* @__PURE__ */ new Date()).getTime() + delay);
      return i2;
    },
    stop: function(which) {
      var list;
      if (which == "all") {
        list = this.list;
      } else {
        list = {};
        list[which] = 1;
      }
      for (var i2 in list) {
        clearTimeout(i2);
        delete this.list[i2];
      }
    }
  };
  this.errorListeners = [];
  this.resumeTime = false;
  this.urlappend = function(url) {
    if (this.options.urlappend) {
      url = MistUtil.http.url.append(url, this.options.urlappend);
    }
    return url;
  };
  if (options.reloadDelay && options.reloadDelay > 3600) {
    options.reloadDelay /= 1e3;
    this.log("A reloadDelay of more than an hour was set: assuming milliseconds were intended. ReloadDelay is now " + options.reloadDelay + "s");
  }
  new MistSkin(this);
  this.checkCombo = function(options2, quiet) {
    if (!MistVideo2.info) {
      return false;
    }
    if (!options2) {
      options2 = {};
    }
    options2 = MistUtil.object.extend(MistUtil.object.extend({}, this.options), options2);
    var source = false;
    var mistPlayer = false;
    var sources;
    if (options2.forceSource) {
      sources = [MistVideo2.info.source[options2.forceSource]];
      MistVideo2.log("Forcing source " + options2.forceSource + ": " + sources[0].type + " @ " + sources[0].url);
    } else if (options2.forceType) {
      sources = MistVideo2.info.source.filter(function(d) {
        return d.type == options2.forceType;
      });
      MistVideo2.log("Forcing type " + options2.forceType);
    } else {
      sources = MistVideo2.info.source;
    }
    var players;
    for (var i2 in mistplayers) {
      mistplayers[i2].shortname = i2;
    }
    if (options2.forcePlayer && mistplayers[options2.forcePlayer]) {
      players = [mistplayers[options2.forcePlayer]];
      MistVideo2.log("Forcing player " + options2.forcePlayer);
    } else {
      players = MistUtil.object.values(mistplayers);
    }
    sources = [].concat(sources);
    var sortoptions = {
      first: "source",
      source: [function(a) {
        if ("origIndex" in a) {
          return a.origIndex;
        }
        a.origIndex = MistVideo2.info.source.indexOf(a);
        return a.origIndex;
      }],
      player: [{ priority: 1 }]
    };
    var map = {
      inner: "player",
      outer: "source"
    };
    if (options2.forcePriority) {
      if ("source" in options2.forcePriority) {
        if (!(options2.forcePriority.source instanceof Array)) {
          throw "forcePriority.source is not an array.";
        }
        sortoptions.source = options2.forcePriority.source.concat(sortoptions.source);
        MistUtil.array.multiSort(sources, sortoptions.source);
      }
      if ("player" in options2.forcePriority) {
        if (!(options2.forcePriority.player instanceof Array)) {
          throw "forcePriority.player is not an array.";
        }
        sortoptions.player = options2.forcePriority.player.concat(sortoptions.player);
        MistUtil.array.multiSort(players, sortoptions.player);
      }
      if ("first" in options2.forcePriority) {
        sortoptions.first = options2.forcePriority.first;
      }
      if (sortoptions.first == "player") {
        map.outer = "player";
        map.inner = "source";
      }
    }
    var variables = {
      player: {
        list: players,
        current: false
      },
      source: {
        list: sources,
        current: false
      }
    };
    if (options2.startCombo) {
      options2.startCombo.started = {
        player: false,
        source: false
      };
      for (var i2 = 0; i2 < players.length; i2++) {
        if (players[i2].shortname == options2.startCombo.player) {
          options2.startCombo.player = i2;
          break;
        }
      }
    }
    function checkStartCombo(which) {
      if (options2.startCombo && !options2.startCombo.started[which]) {
        if (options2.startCombo[which] == variables[which].current || options2.startCombo[which] == variables[which].list[variables[which].current]) {
          options2.startCombo.started[which] = true;
          return 1;
        }
        return 2;
      }
      return 0;
    }
    var best = {
      score: 0,
      source_index: null,
      player: null
    };
    function calcScore(tracktypes2) {
      if (tracktypes2 === true) {
        return 1.9;
      }
      var scores = {
        video: 2,
        audio: 1,
        subtitle: 0.5
      };
      var score2 = 0;
      for (var i3 in tracktypes2) {
        score2 += scores[tracktypes2[i3]];
      }
      return score2;
    }
    var hastracktypes = {};
    for (var i2 in MistVideo2.info.meta.tracks) {
      if (MistVideo2.info.meta.tracks[i2].type == "meta") {
        hastracktypes[MistVideo2.info.meta.tracks[i2].codec] = 1;
      } else {
        hastracktypes[MistVideo2.info.meta.tracks[i2].type] = 1;
      }
    }
    var maxscore = calcScore(MistUtil.object.keys(hastracktypes));
    outerloop:
      for (var n in variables[map.outer].list) {
        variables[map.outer].current = n;
        if (checkStartCombo(map.outer) >= 2) {
          continue;
        }
        innerloop:
          for (var m in variables[map.inner].list) {
            variables[map.inner].current = m;
            if (checkStartCombo(map.inner) >= 1) {
              continue;
            }
            var source = variables.source.list[variables.source.current];
            var p_shortname = variables.player.list[variables.player.current].shortname;
            var player = mistplayers[p_shortname];
            if (player.isMimeSupported(source.type)) {
              var tracktypes = player.isBrowserSupported(source.type, source, MistVideo2);
              if (tracktypes) {
                var score = calcScore(tracktypes);
                if (score > best.score) {
                  if (!quiet) MistVideo2.log("Found a " + (best.score ? "better" : "working") + " combo: " + player.name + " with " + source.url + " (Score: " + score + ")");
                  best = {
                    score,
                    player: p_shortname,
                    source,
                    source_index: variables.source.current
                  };
                  if (best.score == maxscore) {
                    return best;
                  }
                }
              }
            }
          }
      }
    if (best.score) {
      return best;
    }
    return false;
  };
  this.choosePlayer = function() {
    MistVideo2.log("Checking available players..");
    var result = this.checkCombo();
    if (!result) {
      return false;
    }
    var player = mistplayers[result.player];
    var source = result.source;
    MistVideo2.log("Selected: " + player.name + " with " + source.type + " @ " + source.url);
    MistVideo2.playerName = result.player;
    source = MistUtil.object.extend({}, source);
    source.index = result.source_index;
    source.url = MistVideo2.urlappend(source.url);
    MistVideo2.source = source;
    MistUtil.event.send("comboChosen", "Player/source combination selected", MistVideo2.options.target);
    return true;
  };
  function hasVideo(d) {
    if ("meta" in d && "tracks" in d.meta) {
      var tracks = d.meta.tracks;
      var hasVideo2 = false;
      for (var i2 in tracks) {
        if (tracks[i2].type == "video") {
          return true;
        }
      }
    }
    return false;
  }
  function hasAudio(d) {
    if ("meta" in d && "tracks" in d.meta) {
      var tracks = d.meta.tracks;
      var hasAudio2 = false;
      for (var i2 in tracks) {
        if (tracks[i2].type == "audio") {
          return true;
        }
      }
    }
    return false;
  }
  function onStreamInfo(d) {
    if (MistVideo2.player && MistVideo2.player.api && MistVideo2.player.api.unload) {
      MistVideo2.log("Received new stream info while a player was already loaded: unloading player");
      MistVideo2.player.api.unload();
    }
    MistVideo2.info = d;
    MistVideo2.info.updated = /* @__PURE__ */ new Date();
    MistUtil.event.send("haveStreamInfo", d, MistVideo2.options.target);
    MistVideo2.log("Stream info was loaded succesfully.");
    if ("error" in d) {
      var e = d.error;
      if ("on_error" in d) {
        MistVideo2.log(e);
        e = d.on_error;
      } else if ("perc" in d) {
        e += " (" + Math.round(d.perc * 10) / 10 + "%)";
      }
      MistVideo2.showError(e, { reload: true, hideTitle: true });
      return;
    }
    if (Math.abs(MistVideo2.options.rotate) == 1) {
      var w = d.width;
      MistVideo2.info.width = d.height;
      MistVideo2.info.height = w;
    }
    MistVideo2.calcSize = function(size) {
      if (!size) {
        size = { width: false, height: false };
      }
      var fw = size.width || ("width" in options && options.width ? options.width : false);
      var fh = size.height || ("height" in options && options.height ? options.height : false);
      if (!this.info || !("source" in this.info)) {
        fw = 640;
        fh = 480;
      } else if (!this.info.hasVideo || this.source.type.split("/")[1] == "audio") {
        if (!fw) {
          fw = 480;
        }
        if (!fh) {
          fh = 42;
        }
      } else {
        if (!(fw && fh)) {
          var ratio = MistVideo2.info.width / MistVideo2.info.height;
          if (fw || fh) {
            if (fw) {
              fh = fw / ratio;
            } else {
              fw = fh * ratio;
            }
          } else {
            let rescale = function(factor) {
              fw /= factor;
              fh /= factor;
            };
            var cw = window.innerWidth;
            if ("maxwidth" in options && options.maxwidth) cw = options.maxwidth;
            if ("maxwidth" in size && size.maxwidth) cw = Math.min(cw, size.maxwidth);
            var ch = window.innerHeight;
            if ("maxheight" in options && options.maxheight) ch = options.maxheight;
            if ("maxheight" in size && size.maxheight) ch = Math.min(ch, size.maxheight);
            var fw = MistVideo2.info.width;
            var fh = MistVideo2.info.height;
            ;
            if (size.cover) {
              if (size.cover.h && cw && cw > fw) {
                rescale(fw / cw);
              }
              if (size.cover.v && ch && ch > fh) {
                rescale(fh / ch);
              }
            }
            if (cw && (!size.cover || !size.cover.v)) {
              if (fw > cw) {
                rescale(fw / cw);
              }
            }
            if (ch && (!size.cover || !size.cover.h)) {
              if (fh > ch) {
                rescale(fh / ch);
              }
            }
            if (fw < 426) {
              rescale(fw / 426);
            }
            if (fh < 240) {
              rescale(fh / 240);
            }
          }
        }
      }
      this.size = {
        width: Math.round(fw),
        height: Math.round(fh)
      };
      return this.size;
    };
    d.hasVideo = hasVideo(d);
    d.hasAudio = hasAudio(d);
    if (MistVideo2.playerState && d.meta && d.meta.tracks) {
      MistVideo2.playerState.set("tracks", MistUtil.tracks.parse(d.meta.tracks));
    }
    if (d.type == "live") {
      var maxms = 0;
      for (var i2 in MistVideo2.info.meta.tracks) {
        maxms = Math.max(maxms, MistVideo2.info.meta.tracks[i2].lastms);
      }
      d.lastms = maxms;
    } else {
      var time = MistVideo2.resumeTime;
      if (time) {
        var f = function() {
          if (MistVideo2.player && MistVideo2.player.api) {
            MistVideo2.player.api.currentTime = time;
          }
          this.removeEventListener("initialized", f);
        };
        MistUtil.event.addListener(MistVideo2.options.target, "initialized", f);
      }
    }
    if (MistVideo2.options.ABR_bitrate && MistVideo2.options.ABR_resize && (MistVideo2.info && !MistVideo2.info.selver)) {
      MistVideo2.options.ABR_bitrate = false;
    }
    if (MistVideo2.choosePlayer()) {
      if (MistVideo2.reporting) {
        MistVideo2.reporting.report({
          player: MistVideo2.playerName,
          sourceType: MistVideo2.source.type,
          sourceUrl: MistVideo2.source.url,
          pageUrl: location.href
        });
      }
      MistVideo2.player = new mistplayers[MistVideo2.playerName].player();
      MistVideo2.player.onreadylist = [];
      MistVideo2.player.onready = function(dothis) {
        this.onreadylist.push(dothis);
      };
      MistVideo2.player.build(MistVideo2, function(video) {
        MistVideo2.log("Building new player");
        MistVideo2.container.removeAttribute("data-loading");
        MistVideo2.video = video;
        MistVideo2.playerState.bind(video);
        var _api = function() {
          return MistVideo2.player && MistVideo2.player.api ? MistVideo2.player.api : null;
        };
        MistVideo2.api = {
          get currentTime() {
            var a = _api();
            return a ? a.currentTime : 0;
          },
          set currentTime(v) {
            var a = _api();
            if (a) a.currentTime = v;
          },
          get duration() {
            var a = _api();
            return a ? a.duration : 0;
          },
          get paused() {
            var a = _api();
            return a ? a.paused : true;
          },
          get volume() {
            var a = _api();
            return a ? a.volume : 1;
          },
          set volume(v) {
            var a = _api();
            if (a) a.volume = v;
          },
          get muted() {
            var a = _api();
            return a ? a.muted : false;
          },
          set muted(v) {
            var a = _api();
            if (a) a.muted = v;
          },
          get buffered() {
            var a = _api();
            return a ? a.buffered : null;
          },
          play: function() {
            var a = _api();
            return a && a.play ? a.play() : Promise.resolve();
          },
          pause: function() {
            var a = _api();
            if (a && a.pause) a.pause();
          },
          get playbackRate() {
            var a = _api();
            return a && "playbackRate" in a ? a.playbackRate : 1;
          },
          set playbackRate(v) {
            var a = _api();
            if (a && "playbackRate" in a) a.playbackRate = v;
          },
          get loop() {
            var a = _api();
            return a && "loop" in a ? a.loop : false;
          },
          set loop(v) {
            var a = _api();
            if (a && "loop" in a) a.loop = v;
            if (MistVideo2.playerState) MistVideo2.playerState.set("loop", !!v);
          },
          setTrack: function(type, trackid) {
            var a = _api();
            if (!a) return false;
            if (trackid === "auto") {
              MistUtil.event.send("trackSetToAuto", type, MistVideo2.video);
              return true;
            }
            if (type === "subtitle") {
              if (trackid === "" || trackid === "none" || trackid === null) {
                if ("setWSSubtitle" in a) {
                  a.setWSSubtitle(void 0);
                } else if ("setSubtitle" in a) {
                  a.setSubtitle();
                }
                MistUtil.event.send("playerUpdate_trackChanged", { type: "subtitle", trackid: "" }, MistVideo2.video);
                return true;
              }
              if ("setWSSubtitle" in a) {
                a.setWSSubtitle(trackid);
                MistUtil.event.send("playerUpdate_trackChanged", { type: "subtitle", trackid }, MistVideo2.video);
                return true;
              }
              if ("setSubtitle" in a) {
                var tracks = MistUtil.tracks.parse(MistVideo2.info.meta.tracks);
                if (!tracks.subtitle || !(trackid in tracks.subtitle)) return false;
                var meta2 = MistUtil.object.extend({}, tracks.subtitle[trackid]);
                var subtitleSource = null;
                for (var i4 in MistVideo2.info.source) {
                  var src = MistVideo2.info.source[i4];
                  if (src.type === "html5/text/vtt") {
                    subtitleSource = src.url.replace(/.srt$/, ".vtt");
                    break;
                  }
                }
                if (!subtitleSource) return false;
                meta2.src = MistUtil.http.url.addParam(subtitleSource, { track: trackid });
                meta2.label = meta2.displayName || "track " + trackid;
                meta2.lang = meta2.language || "und";
                a.setSubtitle(meta2);
                MistUtil.event.send("playerUpdate_trackChanged", { type: "subtitle", trackid }, MistVideo2.video);
                return true;
              }
              return false;
            }
            if ("setTrack" in a) {
              a.setTrack(type, trackid);
              MistUtil.event.send("playerUpdate_trackChanged", { type, trackid }, MistVideo2.video);
              return true;
            }
            if ("setTracks" in a) {
              var obj = {};
              obj[type] = trackid;
              a.setTracks(obj);
              MistUtil.event.send("playerUpdate_trackChanged", { type, trackid }, MistVideo2.video);
              return true;
            }
            return false;
          },
          getStats: function() {
            var a = _api();
            return a && "getStats" in a ? a.getStats() : null;
          }
        };
        if (MistVideo2.reporting) {
          MistVideo2.reporting.init();
        }
        if ("api" in MistVideo2.player) {
          MistVideo2.monitor = {
            MistVideo: MistVideo2,
            //added here so that the other functions can use it. Do not override it.
            delay: 1,
            //the amount of seconds between measurements.
            averagingSteps: 20,
            //the amount of measurements that are saved.
            threshold: function() {
              if (this.MistVideo.source.type == "webrtc") {
                return 0.95;
              }
              return 0.75;
            },
            init: function() {
              if (this.vars && this.vars.active) {
                return;
              }
              this.MistVideo.log("Enabling monitor");
              this.vars = {
                values: [],
                score: false,
                active: true
              };
              var monitor = this;
              function repeat() {
                if (monitor.vars && monitor.vars.active) {
                  monitor.vars.timer = monitor.MistVideo.timers.start(function() {
                    var score = monitor.calcScore();
                    if (score !== false) {
                      if (monitor.check(score)) {
                        monitor.action();
                      }
                    }
                    repeat();
                  }, monitor.delay * 1e3);
                }
              }
              repeat();
            },
            destroy: function() {
              if (!this.vars || !this.vars.active) {
                return;
              }
              this.MistVideo.log("Disabling monitor");
              this.MistVideo.timers.stop(this.vars.timer);
              delete this.vars;
            },
            reset: function() {
              if (!this.vars || !this.vars.active) {
                this.init();
                return;
              }
              this.MistVideo.log("Resetting monitor");
              this.vars.values = [];
            },
            calcScore: function() {
              var list = this.vars.values;
              list.push(this.getValue());
              if (list.length <= 1) {
                return false;
              }
              var score = this.valueToScore(list[0], list[list.length - 1]);
              if (list.length > this.averagingSteps) {
                list.shift();
              }
              score = Math.max(score, list[list.length - 1].score);
              this.vars.score = score;
              if (MistVideo2.reporting) {
                MistVideo2.reporting.stats.set("playbackScore", Math.round(score * 10) / 10);
              }
              return score;
            },
            valueToScore: function(a, b) {
              var rate = 1;
              if ("player" in this.MistVideo && "api" in this.MistVideo.player && "playbackRate" in this.MistVideo.player.api) {
                rate = this.MistVideo.player.api.playbackRate;
              }
              return (b.video - a.video) / (b.clock - a.clock) / rate;
            },
            getValue: function() {
              var result = {
                clock: (/* @__PURE__ */ new Date()).getTime() * 1e-3,
                video: this.MistVideo.player.api.currentTime
              };
              if (this.vars.values.length) {
                result.score = this.valueToScore(this.vars.values[this.vars.values.length - 1], result);
              }
              return result;
            },
            check: function(score) {
              if (this.vars.values.length < this.averagingSteps * 0.5) {
                return false;
              }
              if (score < this.threshold()) {
                return true;
              }
            },
            action: function() {
              var score = this.vars.score;
              this.MistVideo.showError("Poor playback: " + Math.max(0, Math.round(score * 100)) + "%", {
                passive: true,
                reload: true,
                nextCombo: true,
                ignore: true,
                type: "poor_playback"
              });
            }
          };
          if ("monitor" in MistVideo2.options) {
            MistVideo2.monitor.default = MistUtil.object.extend({}, MistVideo2.monitor);
            MistUtil.object.extend(MistVideo2.monitor, MistVideo2.options.monitor);
          }
          var events = ["loadstart", "play", "playing"];
          for (var i3 in events) {
            MistUtil.event.addListener(MistVideo2.video, events[i3], function() {
              MistVideo2.monitor.init();
            });
          }
          var events = ["loadeddata", "pause", "abort", "emptied", "ended"];
          for (var i3 in events) {
            MistUtil.event.addListener(MistVideo2.video, events[i3], function() {
              if (MistVideo2.monitor) {
                MistVideo2.monitor.destroy();
              }
            });
          }
          var events = [
            "seeking",
            "seeked",
            /*"canplay","playing",*/
            "ratechange"
          ];
          for (var i3 in events) {
            MistUtil.event.addListener(MistVideo2.video, events[i3], function() {
              if (MistVideo2.monitor) {
                MistVideo2.monitor.reset();
              }
            });
          }
          if ("currentTime" in MistVideo2.player.api) {
            var json_source = MistUtil.sources.find(MistVideo2.info.source, {
              type: "html5/text/javascript",
              protocol: "ws" + (location.protocol.charAt(location.protocol.length - 2) == "s" ? "s" : "") + ":"
            });
            if (json_source) {
              MistVideo2.metaTrackSubscriptions = {
                subscriptions: {},
                socket: null,
                listeners: {},
                init: function() {
                  var me = this;
                  if (MistVideo2.player.api.metaTrackSocket) {
                    this.socket = new MistVideo2.player.api.metaTrackSocket();
                  } else {
                    this.socket = new WebSocket(MistUtil.http.url.addParam(MistVideo2.urlappend(json_source.url), { rate: 1 }));
                  }
                  me.send_queue = [];
                  me.checktimer = null;
                  me.s = function(obj) {
                    if (me.socket.readyState == me.socket.OPEN) {
                      me.socket.send(JSON.stringify(obj));
                      return true;
                    }
                    if (me.socket.readyState >= me.socket.CLOSING) {
                      me.init();
                    }
                    this.send_queue.push(obj);
                  };
                  var stayahead = 1;
                  var isfarahead = false;
                  me.socket.setTracks = function() {
                    me.s({ type: "tracks", meta: MistUtil.object.keys(me.subscriptions).join(",") });
                  };
                  me.socket.onopen = function() {
                    MistVideo2.log("Metadata socket opened");
                    me.socket.setTracks();
                    if (MistVideo2.player.api.playbackRate != 1) {
                      me.s({ type: "set_speed", play_rate: MistVideo2.player.api.playbackRate });
                    }
                    me.s({ type: "seek", seek_time: Math.round(MistVideo2.player.api.currentTime * 1e3), ff_to: Math.round((MistVideo2.player.api.currentTime + stayahead) * 1e3) });
                    me.socket.addEventListener("message", function(e2) {
                      if (!e2.data) {
                        MistVideo2.log("Subtitle websocket received empty message.");
                        return;
                      }
                      var message = JSON.parse(e2.data);
                      if (!message) {
                        MistVideo2.log("Subtitle websocket received invalid message.");
                        return;
                      }
                      if ("time" in message && "track" in message && "data" in message) {
                        var pushed = false;
                        if ("all" in me.subscriptions) {
                          me.subscriptions.all.buffer.push(message);
                          pushed = true;
                        }
                        if (message.track in me.subscriptions) {
                          me.subscriptions[message.track].buffer.push(message);
                          pushed = true;
                        }
                        if (pushed) {
                          if (!me.checktimer) {
                            me.check();
                          } else {
                            var willCheckAt = MistVideo2.timers.list[me.checktimer];
                            if (willCheckAt) {
                              var messageAt = (/* @__PURE__ */ new Date()).getTime() + message.time - MistVideo2.player.api.currentTime * 1e3;
                              if (willCheckAt > messageAt) {
                                MistVideo2.log("The metadata socket received a message that should be displayed sooner than the current check time; resetting");
                                MistVideo2.timers.stop(me.checktimer);
                                me.checktimer = null;
                                me.check();
                              }
                            }
                          }
                        }
                      }
                      if ("type" in message) {
                        switch (message.type) {
                          case "on_time": {
                            if (!isfarahead && message.data.current > (MistVideo2.player.api.currentTime + stayahead * 6) * 1e3) {
                              isfarahead = true;
                              me.s({ type: "hold" });
                              MistVideo2.log("Pausing metadata buffer because it is very far ahead, checking again in 5 seconds: " + message.data.current + " > " + MistVideo2.player.api.currentTime * 1e3);
                              MistVideo2.timers.start(function() {
                                if (!MistVideo2.player.api.paused) {
                                  me.s({ type: "play" });
                                }
                                me.s({ type: "fast_forward", ff_to: Math.round((MistVideo2.player.api.currentTime + stayahead) * 1e3) });
                              }, 5e3);
                            }
                            break;
                          }
                          case "seek":
                            {
                              for (var i4 in me.subscriptions) {
                                me.subscriptions[i4].buffer = [];
                              }
                              MistVideo2.log("Cleared metadata buffer after completed seek");
                              if (me.checktimer) {
                                MistVideo2.timers.stop(me.checktimer);
                                me.checktimer = null;
                              }
                            }
                            break;
                        }
                      }
                    });
                    me.socket.onclose = function() {
                      MistVideo2.log("Metadata socket closed");
                    };
                    while (me.send_queue.length && me.socket.readyState == me.socket.OPEN) {
                      me.s(me.send_queue.shift());
                    }
                  };
                  if (!("seeked" in this.listeners)) {
                    var lastff = (/* @__PURE__ */ new Date()).getTime();
                    me.check = function() {
                      if (me.checktimer) {
                        MistVideo2.timers.stop(me.checktimer);
                        me.checktimer = null;
                      }
                      if (MistVideo2.player.api.paused) {
                        return;
                      }
                      var nextAtGlobal = null;
                      for (var i4 in me.subscriptions) {
                        var buffer = me.subscriptions[i4].buffer;
                        while (buffer.length && buffer[0].time <= MistVideo2.player.api.currentTime * 1e3) {
                          var message = buffer.shift();
                          if (message.time < (MistVideo2.player.api.currentTime - 5) * 1e3) {
                            continue;
                          } else {
                            for (var j in me.subscriptions[i4].callbacks) {
                              me.subscriptions[i4].callbacks[j].call(MistVideo2, message);
                            }
                          }
                        }
                        if (buffer.length) {
                          nextAtGlobal = Math.min(nextAtGlobal === null ? 1e9 : nextAtGlobal, buffer[0].time);
                        }
                      }
                      var now = (/* @__PURE__ */ new Date()).getTime();
                      if (now > lastff + 5e3) {
                        me.s({ type: "fast_forward", ff_to: Math.round((MistVideo2.player.api.currentTime + stayahead) * 1e3) });
                        lastff = now;
                      }
                      if (nextAtGlobal) {
                        var delay = nextAtGlobal - MistVideo2.player.api.currentTime * 1e3;
                        me.checktimer = MistVideo2.timers.start(function() {
                          me.check();
                        }, delay);
                      }
                    };
                    this.listeners.seeked = MistUtil.event.addListener(MistVideo2.video, "seeked", function() {
                      for (var i4 in me.subscriptions) {
                        me.subscriptions[i4].buffer = [];
                      }
                      me.s({ type: "seek", seek_time: Math.round(MistVideo2.player.api.currentTime * 1e3), ff_to: Math.round((MistVideo2.player.api.currentTime + stayahead) * 1e3) });
                      lastff = (/* @__PURE__ */ new Date()).getTime();
                    });
                    this.listeners.pause = MistUtil.event.addListener(MistVideo2.video, "pause", function() {
                      me.s({ type: "hold" });
                      MistVideo2.timers.stop(me.checktimer);
                      me.checktimer = null;
                    });
                    this.listeners.playing = MistUtil.event.addListener(MistVideo2.video, "playing", function() {
                      me.s({ type: "play" });
                      if (!me.checktimer) me.check();
                    });
                    this.listeners.ratechange = MistUtil.event.addListener(MistVideo2.video, "ratechange", function() {
                      me.s({ type: "set_speed", play_rate: MistVideo2.player.api.playbackRate });
                    });
                  }
                  if (me.socket.readyState == me.socket.OPEN) {
                    me.socket.onopen();
                  }
                },
                destroy: function() {
                  MistVideo2.log("Closing metadata socket..");
                  this.socket.close();
                  this.socket = null;
                  this.subscriptions = {};
                  for (var i4 in this.listeners) {
                    MistUtil.event.removeListener(this.listeners[i4]);
                  }
                  this.listeners = {};
                },
                add: function(trackid, callback) {
                  if (typeof trackid == "function" && !callback) {
                    callback = trackid;
                    trackid = "all";
                  }
                  if (typeof callback != "function") {
                    return;
                  }
                  if (!(trackid in this.subscriptions)) {
                    this.subscriptions[trackid] = {
                      buffer: [],
                      callbacks: []
                    };
                  }
                  this.subscriptions[trackid].callbacks.push(callback);
                  if (this.socket === null) {
                    this.init();
                  } else {
                    this.socket.setTracks();
                  }
                },
                remove: function(trackid, callback) {
                  if (trackid in this.subscriptions) {
                    for (var i4 in this.subscriptions[trackid].callbacks) {
                      if (callback == this.subscriptions[trackid].callbacks[i4]) {
                        this.subscriptions[trackid].callbacks.splice(i4, 1);
                        break;
                      }
                    }
                    if (this.subscriptions[trackid].callbacks.length == 0) {
                      delete this.subscriptions[trackid];
                      if (MistUtil.object.keys(this.subscriptions).length) {
                        this.socket.setTracks();
                      } else {
                        this.destroy();
                      }
                    }
                  }
                }
              };
              if (typeof options.subscribeToMetaTrack == "function") {
                options.subscribeToMetaTrack = [["all", options.subscribeToMetaTrack]];
              }
              if (options.subscribeToMetaTrack.length) {
                if (typeof options.subscribeToMetaTrack[0] != "object") {
                  options.subscribeToMetaTrack = [options.subscribeToMetaTrack];
                }
                for (var i3 in options.subscribeToMetaTrack) {
                  MistVideo2.metaTrackSubscriptions.add.apply(MistVideo2.metaTrackSubscriptions, options.subscribeToMetaTrack[i3]);
                }
              }
            }
          }
        }
        MistUtil.empty(MistVideo2.options.target);
        new MistSkin(MistVideo2);
        MistVideo2.container = new MistUI(MistVideo2);
        MistVideo2.options.target.appendChild(MistVideo2.container);
        MistVideo2.container.setAttribute("data-loading", "");
        MistVideo2.video.p = MistVideo2.player;
        var events = [
          "abort",
          "canplay",
          "canplaythrough",
          ,
          "emptied",
          "ended",
          "loadeddata",
          "loadedmetadata",
          "loadstart",
          "pause",
          "play",
          "playing",
          "ratechange",
          "seeked",
          "seeking",
          "stalled",
          "volumechange",
          "waiting",
          "metaUpdate_tracks",
          "resizing"
          //,"timeupdate"
        ];
        for (var i3 in events) {
          MistUtil.event.addListener(MistVideo2.video, events[i3], function(e2) {
            if (e2.message && e2.message == "chromecast") {
              return;
            }
            MistVideo2.log("Player event fired: " + e2.type);
          });
        }
        MistUtil.event.addListener(MistVideo2.video, "error", function(e2) {
          var msg;
          if ("player" in MistVideo2 && "api" in MistVideo2.player && "error" in MistVideo2.player.api && MistVideo2.player.api.error) {
            if ("message" in MistVideo2.player.api.error) {
              msg = MistVideo2.player.api.error.message;
            } else if ("code" in MistVideo2.player.api.error && MistVideo2.player.api.error instanceof MediaError) {
              var human = {
                1: "MEDIA_ERR_ABORTED: The fetching of the associated resource was aborted by the user's request.",
                2: "MEDIA_ERR_NETWORK: Some kind of network error occurred which prevented the media from being successfully fetched, despite having previously been available.",
                3: "MEDIA_ERR_DECODE: Despite having previously been determined to be usable, an error occurred while trying to decode the media resource, resulting in an error.",
                4: "MEDIA_ERR_SRC_NOT_SUPPORTED: The associated resource or media provider object (such as a MediaStream) has been found to be unsuitable."
              };
              if (MistVideo2.player.api.error.code in human) {
                msg = human[MistVideo2.player.api.error.code];
              } else {
                msg = "MediaError code " + MistVideo2.player.api.error.code;
              }
            } else {
              msg = MistVideo2.player.api.error;
              if (typeof msg != "string") {
                msg = JSON.stringify(msg);
              }
            }
          } else {
            msg = "An error was encountered.";
          }
          if (MistVideo2.state == "Stream is online") {
            MistVideo2.showError(msg);
          } else {
            MistVideo2.log(msg, "error");
            MistVideo2.showError(MistVideo2.state, { polling: true });
          }
        });
        if ("setSize" in MistVideo2.player) {
          MistVideo2.player.videocontainer = MistVideo2.video.parentNode;
          MistVideo2.video.currentTarget = MistVideo2.options.target;
          if (!MistUtil.class.has(MistVideo2.options.target, "mistvideo-secondaryVideo")) {
            MistVideo2.player.resizeAll = function() {
              if (MistVideo2.destroyed) return;
              function findVideo(startAt, matchTarget) {
                if (startAt.video.currentTarget == matchTarget) {
                  return startAt.video;
                }
                if (startAt.secondary) {
                  for (var i5 = 0; i5 < startAt.secondary.length; i5++) {
                    var result = findVideo(startAt.secondary[i5].MistVideo, matchTarget);
                    if (result) {
                      return result;
                    }
                  }
                }
                return false;
              }
              var main = findVideo(MistVideo2, MistVideo2.options.target);
              if (!main) {
                throw "Main video not found";
              }
              main.p.resize();
              if ("secondary" in MistVideo2) {
                let tryResize = function(mv) {
                  if (mv.MistVideo) {
                    if ("player" in mv.MistVideo) {
                      var sec = findVideo(MistVideo2, mv.MistVideo.options.target);
                      if (!sec) {
                        throw "Secondary video not found";
                      }
                      sec.p.resize();
                    }
                  } else {
                    MistVideo2.timers.start(function() {
                      tryResize(mv);
                    }, 100);
                  }
                };
                for (var i4 in MistVideo2.secondary) {
                  tryResize(MistVideo2.secondary[i4]);
                }
              }
            };
          }
          MistVideo2.player.resize = function(options2, oldsize) {
            var container = MistVideo2.video.currentTarget.querySelector(".mistvideo");
            var size;
            if (!oldsize) {
              oldsize = {
                width: MistVideo2.video.clientWidth,
                height: MistVideo2.video.clientHeight
              };
            }
            if (("" + MistVideo2.options.fillSpace).slice(0, 5) == "cover") {
              var cover = {
                h: ("" + MistVideo2.options.fillSpace).slice(6, 7) == "v" ? false : true,
                v: ("" + MistVideo2.options.fillSpace).slice(6, 7) == "h" ? false : true
              };
              if (container.hasAttribute("data-fullscreen")) {
                var size = MistVideo2.calcSize({
                  maxwidth: window.innerWidth,
                  maxheight: window.innerHeight,
                  cover
                });
                MistVideo2.player.setSize(size);
              } else {
                let apply = function() {
                  var opts = {
                    maxwidth: tw,
                    maxheight: th,
                    cover
                  };
                  var size2 = MistVideo2.calcSize(opts);
                  MistVideo2.player.setSize(size2);
                  MistVideo2.video.currentTarget.style.overflow = "hidden";
                  container.style.width = Math.min(size2.width, opts.maxwidth) + "px";
                  container.style.height = Math.min(size2.height, opts.maxheight) + "px";
                  container.style.setProperty("--width", container.style.width);
                  container.style.setProperty("--height", container.style.height);
                  var ntw = getComputedStyle ? getComputedStyle(MistVideo2.video.currentTarget).width.slice(0, -2) : MistVideo2.video.currentTarget.clientWidth;
                  var nth = getComputedStyle ? getComputedStyle(MistVideo2.video.currentTarget).height.slice(0, -2) : MistVideo2.video.currentTarget.clientHeight;
                  if (ntw != tw || nth != th) {
                    tw = ntw;
                    th = nth;
                    return apply();
                  }
                  return size2;
                };
                container.style.width = window.innerWidth + "px";
                container.style.height = window.innerHeight + "px";
                var tw = getComputedStyle ? getComputedStyle(MistVideo2.video.currentTarget).width.slice(0, -2) : MistVideo2.video.currentTarget.clientWidth;
                var th = getComputedStyle ? getComputedStyle(MistVideo2.video.currentTarget).height.slice(0, -2) : MistVideo2.video.currentTarget.clientHeight;
                var size = apply();
              }
            } else {
              if (!container.hasAttribute("data-fullscreen")) {
                var size = MistVideo2.calcSize(options2);
                this.setSize(size);
                container.style.width = size.width + "px";
                container.style.height = size.height + "px";
                container.style.setProperty("--width", size.width + "px");
                container.style.setProperty("--height", size.height + "px");
                if (MistVideo2.options.fillSpace && (!options2 || !options2.reiterating)) {
                  return this.resize({
                    width: window.innerWidth,
                    height: false,
                    reiterating: true
                  }, oldsize);
                }
                if (MistVideo2.video.currentTarget.clientHeight && MistVideo2.video.currentTarget.clientHeight < size.height) {
                  return this.resize({
                    width: false,
                    height: MistVideo2.video.currentTarget.clientHeight,
                    reiterating: true
                  }, oldsize);
                }
                if (MistVideo2.video.currentTarget.clientWidth && MistVideo2.video.currentTarget.clientWidth < size.width) {
                  return this.resize({
                    width: MistVideo2.video.currentTarget.clientWidth,
                    height: false,
                    reiterating: true
                  }, oldsize);
                }
              } else {
                size = {
                  width: window.innerWidth,
                  height: window.innerHeight
                };
                this.setSize(size);
              }
            }
            if (size.width != oldsize.width || size.height != oldsize.height) {
              MistVideo2.log("Player size calculated: " + size.width + " x " + size.height + " px");
              MistUtil.event.send("player_resize", size, MistVideo2.video);
            }
            if (container) {
              var w2 = size.width;
              var bp = w2 < 384 ? "xs" : w2 < 576 ? "sm" : w2 < 768 ? "md" : w2 < 960 ? "lg" : "xl";
              container.setAttribute("data-size", bp);
            }
            return true;
          };
          if (!MistUtil.class.has(MistVideo2.options.target, "mistvideo-secondaryVideo")) {
            MistUtil.event.addListener(window, "resize", function() {
              if (MistVideo2.destroyed) {
                return;
              }
              MistVideo2.player.resizeAll();
            }, MistVideo2.video);
            if (ResizeObserver) {
              var ro = new ResizeObserver(function(entries) {
                if (MistVideo2.destroyed) {
                  ro.disconnect();
                }
                MistVideo2.player.resizeAll();
              });
              ro.observe(MistVideo2.options.target);
            }
          }
        }
        if (MistVideo2.player.api) {
          if ("setSource" in MistVideo2.player.api) {
            MistVideo2.sourceParams = {};
            MistVideo2.player.api.setSourceParams = function(url, params) {
              MistUtil.object.extend(MistVideo2.sourceParams, params);
              MistVideo2.player.api.setSource(MistUtil.http.url.addParam(url, params));
            };
            if (!("setTracks" in MistVideo2.player.api)) {
              MistVideo2.player.api.setTracks = function(usetracks) {
                var meta2 = MistUtil.tracks.parse(MistVideo2.info.meta.tracks);
                for (var i4 in usetracks) {
                  if (i4 in meta2 && (usetracks[i4] in meta2[i4] || usetracks[i4] == "none")) {
                    continue;
                  }
                  MistVideo2.log("Skipping trackselection of " + i4 + " track " + usetracks[i4] + " because it does not exist");
                  delete usetracks[i4];
                }
                var newurl = MistVideo2.source.url;
                var time2 = MistVideo2.player.api.currentTime;
                this.setSourceParams(newurl, usetracks);
                if (MistVideo2.info.type != "live") {
                  var f2 = function() {
                    MistVideo2.player.api.currentTime = time2;
                    this.removeEventListener("loadedmetadata", f2);
                  };
                  MistUtil.event.addListener(MistVideo2.video, "loadedmetadata", f2);
                }
              };
            }
          }
          if (!("setTracks" in MistVideo2.player.api) && "setTrack" in MistVideo2.player.api) {
            MistVideo2.player.api.setTracks = function(usetracks) {
              for (var i4 in usetracks) {
                MistVideo2.player.api.setTrack(i4, usetracks[i4]);
              }
            };
          }
          if (options.setTracks) {
            var setTracks = MistUtil.object.extend({}, options.setTracks);
            if ("subtitle" in options.setTracks && "setSubtitle" in MistVideo2.player.api) {
              MistVideo2.player.onready(function() {
                var subtitleSource = false;
                for (var i4 in MistVideo2.info.source) {
                  var source = MistVideo2.info.source[i4];
                  if (source.type == "html5/text/vtt" && MistUtil.http.url.split(source.url).protocol == MistUtil.http.url.split(MistVideo2.source.url).protocol) {
                    subtitleSource = source.url.replace(/.srt$/, ".vtt");
                    break;
                  }
                }
                if (!subtitleSource) {
                  return;
                }
                var tracks = MistUtil.tracks.parse(MistVideo2.info.meta.tracks);
                if (!("subtitle" in tracks) || !(setTracks.subtitle in tracks.subtitle)) {
                  return;
                }
                meta = tracks.subtitle[setTracks.subtitle];
                meta.src = MistUtil.http.url.addParam(subtitleSource, { track: setTracks.subtitle });
                meta.label = "automatic";
                meta.lang = "unknown";
                MistVideo2.player.api.setSubtitle(meta);
                MistUtil.event.send("playerUpdate_trackChanged", {
                  type: "subtitle",
                  trackid: setTracks.subtitle
                }, MistVideo2.video);
                delete setTracks.subtitle;
              });
            }
            if ("setTrack" in MistVideo2.player.api) {
              MistVideo2.player.onready(function() {
                for (var i4 in setTracks) {
                  MistVideo2.player.api.setTrack(i4, setTracks[i4]);
                  MistUtil.event.send("playerUpdate_trackChanged", {
                    type: i4,
                    trackid: setTracks[i4]
                  }, MistVideo2.video);
                }
              });
            } else if ("setTracks" in MistVideo2.player.api) {
              MistVideo2.player.onready(function() {
                MistVideo2.player.api.setTracks(setTracks);
              });
              for (var i3 in setTracks) {
                MistUtil.event.send("playerUpdate_trackChanged", {
                  type: i3,
                  trackid: setTracks[i3]
                }, MistVideo2.video);
              }
            }
          }
          if (MistVideo2.player.api.ABR_resize && MistVideo2.options.ABR_resize) {
            var resizeratelimiter = false;
            var newsize = false;
            MistUtil.event.addListener(MistVideo2.video, "player_resize", function(e2) {
              if (MistVideo2.options.setTracks && MistVideo2.options.setTracks.video) {
                return;
              }
              if (resizeratelimiter) {
                MistVideo2.timers.stop(resizeratelimiter);
              }
              resizeratelimiter = MistVideo2.timers.start(function() {
                MistVideo2.player.api.ABR_resize(e2.message);
                resizeratelimiter = false;
              }, 1e3);
            });
            MistUtil.event.addListener(MistVideo2.video, "trackSetToAuto", function(e2) {
              if (e2.message == "video") {
                MistVideo2.player.api.ABR_resize({
                  width: MistVideo2.video.clientWidth,
                  height: MistVideo2.video.clientHeight
                });
              }
            });
            MistVideo2.player.api.ABR_resize({
              width: MistVideo2.video.clientWidth,
              height: MistVideo2.video.clientHeight
            });
          }
        }
        for (var i3 in MistVideo2.player.onreadylist) {
          MistVideo2.player.onreadylist[i3]();
        }
        MistUtil.event.send("initialized", null, options.target);
        MistVideo2.log("Initialized");
        if (MistVideo2.options.callback) {
          options.callback(MistVideo2);
        }
      });
    } else if (MistVideo2.options.startCombo) {
      delete MistVideo2.options.startCombo;
      MistVideo2.unload("No compatible players found - retrying without startCombo.");
      mistPlay2(MistVideo2.stream, MistVideo2.options);
    } else {
      MistVideo2.showError("No compatible player/source combo found.", { reload: true });
      MistUtil.event.send("initializeFailed", null, options.target);
      MistVideo2.log("Initialization failed");
    }
  }
  MistVideo2.calcSize = function() {
    return {
      width: 640,
      height: 480
    };
  };
  MistUtil.empty(MistVideo2.options.target);
  new MistSkin(MistVideo2);
  MistVideo2.container = new MistUI(MistVideo2, MistVideo2.skin.structure.placeholder);
  MistVideo2.options.target.appendChild(MistVideo2.container);
  MistVideo2.container.setAttribute("data-loading", "");
  function openWithGet() {
    var url = MistUtil.http.url.addParam(MistVideo2.urlappend(options.host + "/json_" + encodeURIComponent(MistVideo2.stream) + ".js"), { metaeverywhere: 1, inclzero: 1 });
    MistVideo2.log("Requesting stream info from " + url);
    MistUtil.http.get(url, function(d) {
      if (MistVideo2.destroyed) {
        return;
      }
      onStreamInfo(JSON.parse(d));
    }, function(xhr) {
      var msg = "Connection failed: the media server at " + MistVideo2.options.host + " may be offline";
      MistVideo2.showError(msg, { reload: 30 });
      if (!MistVideo2.info) {
        MistUtil.event.send("initializeFailed", null, options.target);
        MistVideo2.log("Initialization failed");
      }
    });
  }
  if ("WebSocket" in window) {
    let openSocket = function() {
      MistVideo2.log("Opening stream status stream through websocket..");
      var url = MistVideo2.options.host.replace(/^http/i, "ws");
      url = MistUtil.http.url.addParam(MistVideo2.urlappend(url + "/json_" + encodeURIComponent(MistVideo2.stream) + ".js"), { metaeverywhere: 1, inclzero: 1 });
      var socket;
      try {
        socket = new WebSocket(url);
      } catch (e) {
        MistVideo2.log("Error while attempting to open WebSocket to " + url);
        openWithGet();
        return;
      }
      MistVideo2.socket = socket;
      socket.die = false;
      socket.destroy = function() {
        this.die = true;
        if (MistVideo2.reporting) {
          MistVideo2.reporting.reportStats();
          MistVideo2.reporting = false;
        }
        this.onclose = function() {
        };
        this.close();
      };
      socket.timeOut = MistVideo2.timers.start(function() {
        if (socket.readyState <= 1) {
          socket.destroy();
          openWithGet();
        }
      }, 5e3);
      socket.onopen = function(e) {
        this.wasConnected = true;
        if (!MistVideo2.reporting) {
          MistVideo2.reporting = {
            stats: {
              set: function(key, value) {
                this.d[key] = value;
              },
              add: function(key, add) {
                if (typeof add == "undefined") {
                  add = 1;
                }
                this.d[key] += add;
              },
              d: {
                nWaiting: 0,
                timeWaiting: 0,
                nStalled: 0,
                timeStalled: 0,
                timeUnpaused: 0,
                nError: 0,
                nLog: 0,
                videoHeight: null,
                videoWidth: null,
                playerHeight: null,
                playerWidth: null
              },
              last: {
                firstPlayback: null,
                nWaiting: 0,
                timeWaiting: 0,
                nStalled: 0,
                timeStalled: 0,
                timeUnpaused: 0,
                nError: 0,
                lastError: null,
                playbackScore: 1,
                nLog: 0,
                autoplay: null,
                videoHeight: null,
                videoWidth: null,
                playerHeight: null,
                playerWidth: null
              }
            },
            report: function(d) {
              if (MistVideo2.socket.readyState == 1) {
                MistVideo2.socket.send(JSON.stringify(d));
              }
            },
            reportStats: function() {
              var d = {};
              var report = false;
              var newlogs = MistVideo2.logs.slice(this.stats.last.nLog);
              for (var i2 in this.stats.d) {
                if (this.stats.d[i2] != this.stats.last[i2]) {
                  d[i2] = this.stats.d[i2];
                  this.stats.last[i2] = d[i2];
                  report = true;
                }
              }
              if (report) {
                if (newlogs.length) {
                  d.logs = [];
                  for (var i2 in newlogs) {
                    d.logs.push(newlogs[i2].message);
                  }
                }
                this.report(d);
              }
              MistVideo2.timers.start(function() {
                if (MistVideo2.reporting) {
                  MistVideo2.reporting.reportStats();
                }
              }, 5e3);
            },
            init: function() {
              var video = MistVideo2.video;
              var firstPlay = MistUtil.event.addListener(video, "playing", function() {
                MistVideo2.reporting.stats.set("firstPlayback", (/* @__PURE__ */ new Date()).getTime() - MistVideo2.bootMs);
                MistUtil.event.removeListener(firstPlay);
              });
              MistUtil.event.addListener(video, "waiting", function() {
                if (!MistVideo2 || !MistVideo2.reporting) {
                  return;
                }
                MistVideo2.reporting.stats.add("nWaiting");
              });
              MistUtil.event.addListener(video, "stalled", function() {
                if (!MistVideo2 || !MistVideo2.reporting) {
                  return;
                }
                MistVideo2.reporting.stats.add("nStalled");
              });
              MistUtil.event.addListener(MistVideo2.options.target, "error", function(e2) {
                if (!MistVideo2 || !MistVideo2.reporting) {
                  return;
                }
                MistVideo2.reporting.stats.add("nError");
                MistVideo2.reporting.stats.set("lastError", e2.message);
              }, video);
              if (Object && Object.defineProperty) {
                var timeWaiting = 0;
                var waitingSince = false;
                var timeStalled = 0;
                var stalledSince = false;
                var timeUnpaused = 0;
                var unpausedSince = false;
                var d = MistVideo2.reporting.stats.d;
                Object.defineProperty(d, "timeWaiting", {
                  get: function() {
                    return timeWaiting + (waitingSince ? (/* @__PURE__ */ new Date()).getTime() - waitingSince : 0);
                  }
                });
                Object.defineProperty(d, "timeStalled", {
                  get: function() {
                    return timeStalled + (stalledSince ? (/* @__PURE__ */ new Date()).getTime() - stalledSince : 0);
                  }
                });
                Object.defineProperty(d, "timeUnpaused", {
                  get: function() {
                    return timeUnpaused + (unpausedSince ? (/* @__PURE__ */ new Date()).getTime() - unpausedSince : 0);
                  }
                });
                Object.defineProperty(d, "nLog", {
                  get: function() {
                    return MistVideo2.logs.length;
                  }
                });
                Object.defineProperty(d, "videoHeight", {
                  get: function() {
                    return MistVideo2.video ? MistVideo2.video.videoHeight : null;
                  }
                });
                Object.defineProperty(d, "videoWidth", {
                  get: function() {
                    return MistVideo2.video ? MistVideo2.video.videoWidth : null;
                  }
                });
                Object.defineProperty(d, "playerHeight", {
                  get: function() {
                    return MistVideo2.video ? MistVideo2.video.clientHeight : null;
                  }
                });
                Object.defineProperty(d, "playerWidth", {
                  get: function() {
                    return MistVideo2.video ? MistVideo2.video.clientWidth : null;
                  }
                });
                MistUtil.event.addListener(video, "waiting", function() {
                  timeWaiting = d.timeWaiting;
                  waitingSince = (/* @__PURE__ */ new Date()).getTime();
                });
                MistUtil.event.addListener(video, "stalled", function() {
                  timeStalled = d.timeStalled;
                  stalledSince = (/* @__PURE__ */ new Date()).getTime();
                });
                var events = ["playing", "pause"];
                for (var i2 in events) {
                  MistUtil.event.addListener(video, events[i2], function() {
                    timeWaiting = d.timeWaiting;
                    timeStalled = d.timeStalled;
                    waitingSince = false;
                    stalledSince = false;
                  });
                }
                MistUtil.event.addListener(video, "playing", function() {
                  timeUnpaused = d.timeUnpaused;
                  unpausedSince = (/* @__PURE__ */ new Date()).getTime();
                });
                MistUtil.event.addListener(video, "pause", function() {
                  timeUnpaused = d.timeUnpaused;
                  unpausedSince = false;
                });
              }
              this.reportStats();
            }
          };
        }
      };
      socket.onclose = function(e) {
        if (this.die) {
          return;
        }
        if (this.wasConnected) {
          MistVideo2.log("Reopening websocket..");
          openSocket();
          return;
        }
        openWithGet();
      };
      var on_ended_show_state = false;
      var on_waiting_show_state = false;
      socket.addEventListener("message", function(e) {
        if (socket.timeOut) {
          MistVideo2.timers.stop(socket.timeOut);
          socket.timeOut = false;
        }
        var data = JSON.parse(e.data);
        if (!data) {
          MistVideo2.showError("Error while parsing stream status stream. Obtained: " + e.data.toString(), { reload: true });
        }
        if ("error" in data) {
          var e = data.error;
          if ("on_error" in data) {
            MistVideo2.log(e);
            e = data.on_error;
          } else if ("perc" in data) {
            e += " (" + Math.round(data.perc * 10) / 10 + "%)";
          }
          MistVideo2.state = data.error;
          if (MistVideo2.playerState) MistVideo2.playerState.set("streamState", data.error);
          var buttons;
          switch (data.error) {
            case "Stream is offline":
              MistVideo2.info = false;
              if (MistVideo2.player && MistVideo2.player.api && MistVideo2.player.api.currentTime) {
                MistVideo2.resumeTime = MistVideo2.player.api.currentTime;
              }
            case "Stream is initializing":
            case "Stream is booting":
            case "Stream is waiting for data":
            case "Stream is shutting down":
            case "Stream status is invalid?!":
              if (MistVideo2.showIdleScreen) {
                var perc = "perc" in data ? data.perc : void 0;
                if (MistVideo2.player && MistVideo2.player.api && !MistVideo2.player.api.paused) {
                  MistVideo2.log(data.error, "error");
                  if (!on_ended_show_state) {
                    on_ended_show_state = MistUtil.event.addListener(MistVideo2.video, "ended", function() {
                      MistVideo2.showIdleScreen(data.error, perc);
                    });
                  }
                  if (!on_waiting_show_state) {
                    on_waiting_show_state = MistUtil.event.addListener(MistVideo2.video, "waiting", function() {
                      MistVideo2.showIdleScreen(data.error, perc);
                    });
                  }
                  return;
                }
                MistVideo2.showIdleScreen(e, perc);
                return;
              }
              if (MistVideo2.player && MistVideo2.player.api && !MistVideo2.player.api.paused) {
                MistVideo2.log(data.error, "error");
                if (!on_ended_show_state) {
                  on_ended_show_state = MistUtil.event.addListener(MistVideo2.video, "ended", function() {
                    MistVideo2.showError(data.error, { polling: true });
                  });
                }
                if (!on_waiting_show_state) {
                  on_waiting_show_state = MistUtil.event.addListener(MistVideo2.video, "waiting", function() {
                    MistVideo2.showError(data.error, { polling: true });
                  });
                }
                return;
              }
              buttons = { polling: true };
              break;
            default:
              buttons = { reload: true };
          }
          MistVideo2.showError(e, buttons);
        } else {
          let getCurrentStream = function(info) {
            if (!info.redirected) return MistVideo2.stream;
            return info.redirected[info.redirected.length - 1];
          }, difference = function(a2, b) {
            if (a2 == b) {
              return false;
            }
            if (typeof a2 == "object" && typeof b != "undefined") {
              var results = {};
              for (var i3 in a2) {
                if (MistUtil.array.indexOf(["lastms", "hasVideo"], i3) >= 0) {
                  continue;
                }
                var d = difference(a2[i3], b[i3]);
                if (d) {
                  if (d === true) {
                    results[i3] = [a2[i3], b[i3]];
                  } else {
                    results[i3] = d;
                  }
                }
              }
              for (var i3 in b) {
                if (MistUtil.array.indexOf(["lastms", "hasVideo"], i3) >= 0) {
                  continue;
                }
                if (!(i3 in a2)) {
                  results[i3] = [a2[i3], b[i3]];
                }
              }
              if (MistUtil.object.keys(results).length) {
                return results;
              }
              return false;
            }
            return true;
          };
          MistVideo2.state = "Stream is online";
          if (MistVideo2.playerState) MistVideo2.playerState.set("streamState", "Stream is online");
          MistVideo2.clearError();
          if (MistVideo2.hideIdleScreen) MistVideo2.hideIdleScreen();
          if (on_ended_show_state) {
            MistUtil.event.removeListener(on_ended_show_state);
          }
          if (on_waiting_show_state) {
            MistUtil.event.removeListener(on_waiting_show_state);
          }
          if (!MistVideo2.info) {
            onStreamInfo(data);
            return;
          }
          if (getCurrentStream(data) != getCurrentStream(MistVideo2.info)) {
            MistVideo2.log("Redirecting to " + getCurrentStream(data) + "..");
            onStreamInfo(data);
            return;
          }
          var diff = difference(data, MistVideo2.info);
          if (diff) {
            if ("source" in diff && "error" in MistVideo2.info) {
              MistVideo2.reload("Reloading, stream info has error");
              return;
            }
            MistVideo2.info = MistUtil.object.extend(MistVideo2.info, data);
            MistVideo2.info.updated = /* @__PURE__ */ new Date();
            var resized = false;
            for (var i2 in diff) {
              switch (i2) {
                case "meta": {
                  for (var j in diff[i2]) {
                    switch (j) {
                      case "tracks": {
                        var newtracks = JSON.stringify(Object.keys(data.meta.tracks).sort());
                        var oldtracks = JSON.stringify(Object.keys(MistVideo2.info.meta.tracks).sort());
                        if (newtracks != oldtracks) {
                          var v = hasVideo(MistVideo2.info);
                          var a = hasAudio(MistVideo2.info);
                          if (MistVideo2.info.hasVideo != v || MistVideo2.info.hasAudio != a) {
                            MistVideo2.info.hasVideo = v;
                            MistVideo2.info.hasAudio = a;
                          }
                          MistUtil.event.send("metaUpdate_tracks", data, MistVideo2.video);
                        }
                        break;
                      }
                    }
                    break;
                  }
                }
                case "width":
                case "height": {
                  resized = true;
                  break;
                }
              }
            }
            if (resized && MistVideo2.player && MistVideo2.player.resize) {
              MistVideo2.player.resize();
            }
          } else {
            MistVideo2.log("Metachange: no differences detected");
          }
        }
      });
    };
    openSocket();
  } else {
    openWithGet();
  }
  this.unload = function(reason) {
    if (this.destroyed) {
      return;
    }
    this.log("Unloading..");
    this.destroyed = true;
    this.timers.stop("all");
    if (this.playerState) {
      this.playerState.destroy();
    }
    for (var i2 in this.errorListeners) {
      var listener = this.errorListeners[i2];
      if (listener.src in MistUtil.scripts.list) {
        var index = MistUtil.array.indexOf(MistUtil.scripts.list[listener.src].subscribers);
        if (index >= 0) {
          MistUtil.scripts.list[listener.src].subscribers.splice(index, 1);
        }
      }
    }
    if ("monitor" in MistVideo2 && "destroy" in MistVideo2.monitor) {
      MistVideo2.monitor.destroy();
    }
    if (this.socket) {
      if (this.reporting) {
        this.reporting.reportStats();
        this.reporting.report({ unload: reason ? reason : null });
      }
      this.socket.destroy();
    }
    if (this.player && this.player.api) {
      if ("pause" in this.player.api) {
        this.player.api.pause();
      }
      if ("setSource" in this.player.api) {
        this.player.api.setSource("");
      }
      if ("unload" in this.player.api) {
        try {
          this.player.api.unload();
        } catch (e2) {
          MistVideo2.log("Error while unloading player: " + e2.message);
        }
      }
    }
    if (this.metaTrackSubscriptions && this.metaTrackSubscriptions.socket) {
      this.metaTrackSubscriptions.destroy();
    }
    if (this.UI && this.UI.elements) {
      for (var i2 in this.UI.elements) {
        var e = this.UI.elements[i2];
        if ("attachedListeners" in e) {
          for (var i2 in e.attachedListeners) {
            MistUtil.event.removeListener(e.attachedListeners[i2]);
          }
        }
        if (e.parentNode) {
          e.parentNode.removeChild(e);
        }
      }
    }
    if (this.video) {
      MistUtil.empty(this.video);
    }
    if ("container" in this) {
      MistUtil.empty(this.container);
      delete this.container;
    }
    MistUtil.empty(this.options.target);
    delete this.video;
  };
  this.reload = function(reason) {
    var time = "player" in this && "api" in this.player ? this.player.api.currentTime : false;
    this.unload(reason);
    var NewMistVideo = mistPlay2(this.stream, this.options);
    if (time && this.info.type != "live") {
      var f = function() {
        if (NewMistVideo.player && NewMistVideo.player.api) {
          NewMistVideo.player.api.currentTime = time;
        }
        this.removeEventListener("initialized", f);
      };
      MistUtil.event.addListener(this.options.target, "initialized", f);
    }
    return MistVideo2;
  };
  this.nextCombo = function() {
    var time = false;
    if ("player" in this && "api" in this.player) {
      time = this.player.api.currentTime;
    }
    var startCombo = this.source ? {
      source: this.source.index,
      player: this.playerName
    } : false;
    if (!this.checkCombo({ startCombo }, true)) {
      if (this.checkCombo({ startCombo: false }, true)) {
        startCombo = false;
      } else {
        return;
      }
    }
    this.unload("nextCombo");
    var opts = this.options;
    opts.startCombo = startCombo;
    MistVideo2 = mistPlay2(this.stream, opts);
    if (time && (isFinite(time) && this.info.type != "live")) {
      var f = function() {
        if ("player" in MistVideo2 && "api" in MistVideo2.player) {
          MistVideo2.player.api.currentTime = time;
        }
        this.removeEventListener("initialized", f);
      };
      MistUtil.event.addListener(opts.target, "initialized", f);
    }
  };
  this.onPlayerBuilt = function() {
  };
  if (options.MistVideoObject) {
    options.MistVideoObject.reference = this;
  }
  return this;
}

// src/core/shared.js
function ControlChannel(channel, MistVideo2, externalListenersObj) {
  var control = this;
  this.channel = channel;
  this.debugging = false;
  this.was_connected = false;
  var queue = [];
  var listeners = externalListenersObj || {};
  this.addListener = function(type, callback) {
    if (!(type in listeners)) {
      listeners[type] = {};
    }
    var eid = Object.keys(listeners[type]).length;
    if (callback) {
      listeners[type][eid] = callback;
      return eid;
    } else {
      return new Promise(function(resolve) {
        listeners[type][eid] = function(data) {
          delete listeners[type][eid];
          resolve(data);
        };
      });
    }
  };
  function callListeners(type, data) {
    if (type in listeners) {
      for (var eid in listeners[type]) {
        try {
          listeners[type][eid].apply(control, [data]);
        } catch (err) {
          MistVideo2.log("Error in " + type + " listener " + eid + ": " + err, "error");
        }
      }
    }
  }
  this.channel.addEventListener("open", function(ev) {
    control.was_connected = true;
    callListeners("channel_open", ev);
    if (queue.length) {
      for (var i2 = 0; i2 <= queue.length; i2++) {
        control.send(queue[i2]);
      }
      queue = [];
    }
    if (control.timeout) {
      MistVideo2.timers.stop(control.timeout);
    }
  });
  this.timeout = MistVideo2.timers.start(function() {
    if (control.readyState == "connecting") {
      MistVideo2.log("Control socket timeout", "error");
      if (control.debugging) console.log("The control channel timed out");
      callListeners("channel_timeout", control.channel);
    }
  }, 5e3);
  this.channel.addEventListener("message", function(e) {
    var message;
    try {
      message = JSON.parse(e.data);
    } catch (err) {
      MistVideo2.log("Received invalid control message: " + err + " in " + e.data, "error");
    }
    if (message) {
      var data = "data" in message ? message.data : message;
      if (!message.type) {
        MistVideo2.log("Received invalid control message: missing type in " + e.data, "error");
      }
      if (control.debugging) console.log("Received:", message.type, data);
      callListeners(message.type, data);
    }
  });
  this.channel.addEventListener("close", function(ev) {
    callListeners("channel_close", ev);
    if (control.debugging) console.log("The control channel was closed", ev);
    MistVideo2.log("The control channel was closed");
  });
  this.channel.addEventListener("error", function(ev) {
    callListeners("channel_error", ev);
    if (control.debugging) console.log("The control channel threw an error", ev);
    MistVideo2.log("The control channel threw an error: " + ev);
  });
  Object.defineProperty(this, "readyState", {
    get: function() {
      var state = this.channel.readyState;
      if (typeof state == "string") return state;
      switch (state) {
        case 0:
          return "connecting";
        case 1:
          return "open";
        case 2:
          return "closing";
        case 3:
          return "closed";
      }
      return state;
    }
  });
  this.send = function(cmdObj) {
    if (!this.channel || this.readyState != "open") {
      queue.push(cmdObj);
      if (this.debugging) console.warn("Want to send but control channel is " + this.readyState + ". Queue: " + queue.length);
      return;
    }
    var str;
    try {
      str = JSON.stringify(cmdObj);
    } catch (e) {
      MistVideo2.log("Tried to send invalid command: " + e, "error");
    }
    if (str) {
      this.channel.send(str);
      if (this.debugging) console.warn("Sent:", cmdObj.type, cmdObj);
    }
  };
  this.addListener("on_error", function(msg) {
    callListeners("on_stop", msg);
    MistVideo2.showError(msg.message);
  });
}
function DataChannel2WebSocket() {
  this.origin = {};
  this.CONNECTING = 0;
  this.OPEN = 1;
  this.CLOSING = 2;
  this.CLOSED = 3;
  this.debugging = false;
  this.readyState = 0;
  this.listeners = [];
  var converter = this;
  this.init = function(datachannel) {
    this.origin = datachannel || {};
    if (this.origin._processed) {
      return true;
    }
    if ("readyState" in this.origin) {
      let onopen = function() {
        converter.readyState = converter.OPEN;
        converter.onopen();
      };
      this.origin._processed = true;
      this.origin.addEventListener("open", function() {
        onopen();
      });
      this.origin.onmessage = function(e) {
        if (converter.debugging) console.log("Received metadata:", JSON.parse(e.data));
      };
      this.origin.addEventListener("close", function() {
        converter.readyState = converter.CLOSED;
        converter.onclose();
      });
      if (this.origin.readyState == "open") {
        onopen();
      }
      return true;
    } else {
      return false;
    }
  };
  this.open = function() {
    if (this.readyState == this.OPEN) return;
    switch (this.origin.readyState) {
      case "connecting": {
        this.readyState = this.CONNECTING;
        break;
      }
      case "open": {
        this.readyState = this.OPEN;
        break;
      }
      case "closing": {
        this.readyState = this.CLOSING;
        break;
      }
      case "closed": {
        this.readyState = this.CLOSED;
        break;
      }
    }
    for (var i2 in this.listeners) {
      this.origin.addEventListener.apply(this.origin, this.listeners[i2]);
    }
  };
  this.close = function() {
    if (this.readyState >= this.CLOSING) return;
    this.readyState = this.CLOSED;
    for (var i2 in this.listeners) {
      this.removeEventListener.apply(this, this.listeners[i2]);
    }
  };
  this.send = function() {
    return false;
  };
  this.onopen = function() {
  };
  this.onclose = function() {
  };
  this.addEventListener = function() {
    this.listeners.push(arguments);
    return this.origin.addEventListener.apply(this.origin, arguments);
  };
  this.removeEventListener = function(name, func) {
    for (var i2 = this.listeners.length - 1; i2 >= 0; i2--) {
      if (name == this.listeners[i2][0] && func == this.listeners[i2][1]) {
        this.listeners.splice(i2, 1);
        break;
      }
    }
    return this.origin.removeEventListener.apply(this.origin, arguments);
  };
  this.init();
  return this;
}
function ControlChannelAPI(controller, MistVideo2, video) {
  var api = this;
  var control = controller.control;
  video.setAttribute("playsinline", "");
  var attrs = ["autoplay", "loop", "poster"];
  for (var i2 in attrs) {
    var attr = attrs[i2];
    if (MistVideo2.options[attr]) {
      video.setAttribute(attr, MistVideo2.options[attr] === true ? "" : MistVideo2.options[attr]);
    }
  }
  if (MistVideo2.options.muted) {
    video.muted = true;
  }
  if (MistVideo2.info.type == "live") {
    video.loop = false;
  }
  if (MistVideo2.options.controls == "stock") {
    video.setAttribute("controls", "");
  }
  video.setAttribute("crossorigin", "anonymous");
  [
    "volume",
    "muted",
    "loop",
    "paused",
    "error",
    "textTracks",
    "webkitDroppedFrameCount",
    "webkitDecodedFrameCount"
  ].forEach(function(item) {
    Object.defineProperty(api, item, {
      get: function() {
        return video[item];
      },
      set: function(value) {
        return video[item] = value;
      }
    });
  });
  ["load", "getVideoPlaybackQuality"].forEach(function(item) {
    if (item in video) {
      api[item] = function() {
        return video[item].call(video, arguments);
      };
    }
  });
  this.play = function() {
    if (controller.connection) {
      switch (controller.connection.connectionState) {
        case "connected": {
          controller.control.send({ type: "play" });
          return video.play();
        }
        case "failed":
        case "closed": {
          return new Promise(function(resolve, reject) {
            controller.connect().then(function() {
              video.play().then(resolve).catch(reject);
            }).catch(reject);
          });
        }
        default: {
        }
      }
    }
    return new Promise(function(resolve, reject) {
      if (!controller.connecting) {
        MistVideo2.log("Received call to play while not connected, connecting..");
        controller.connect();
      } else {
        MistVideo2.log("Received call to play while still connecting, waiting..");
      }
      if (controller.connecting) {
        controller.connecting.then(resolve).catch(reject);
      } else {
        reject();
      }
    });
  };
  this.pause = function() {
    return new Promise(function(resolve, reject) {
      try {
        video.pause();
        controller.control.send({ type: "hold" });
        resolve();
      } catch (err) {
        reject(err);
      }
    });
  };
  this.stop = function() {
    return new Promise(function(resolve, reject) {
      try {
        video.pause();
        controller.control.send({ type: "stop" });
        resolve();
      } catch (err) {
        reject(err);
      }
    });
  };
  var seekoffset = 0, last_on_time, duration, play_rate, currenttracks = [], looping = false;
  controller.control.addListener("on_time", function(msg) {
    last_on_time = msg;
    last_on_time._received = /* @__PURE__ */ new Date();
    seekoffset = msg.current * 1e-3 - video.currentTime;
    var d = msg.end == 0 ? Infinity : msg.end * 1e-3;
    if (d != duration) {
      duration = d;
      MistUtil.event.send("durationchange", d, video);
    }
    if (MistVideo2.info && MistVideo2.info.meta) MistVideo2.info.meta.buffer_window = msg.end - msg.begin;
    play_rate = msg.play_rate_curr;
    if (msg.tracks && currenttracks.join(",") != msg.tracks.join(",")) {
      var tracks = MistVideo2.info ? MistUtil.tracks.parse(MistVideo2.info.meta.tracks) : [];
      for (var i3 in msg.tracks) {
        if (currenttracks.indexOf(msg.tracks[i3]) < 0) {
          var type;
          for (var j2 in tracks) {
            if (msg.tracks[i3] in tracks[j2]) {
              type = j2;
              break;
            }
          }
          if (!type) {
            continue;
          }
          if (type == "subtitle") {
            continue;
          }
          MistUtil.event.send("playerUpdate_trackChanged", {
            type,
            trackid: msg.tracks[i3]
          }, video);
        }
      }
      currenttracks = msg.tracks;
    }
    if (MistVideo2.reporting && msg.tracks) {
      MistVideo2.reporting.stats.d.tracks = msg.tracks.join(",");
    }
  });
  controller.control.addListener("on_stop", function(msg) {
    MistUtil.event.send("ended", null, video);
    if (api.loop) {
      if (!looping) {
        looping = true;
        seekoffset = 0;
        MistVideo2.log("Looping..");
        controller.close().then(function() {
          controller.connect().then(function() {
            looping = false;
          });
        });
      }
    } else {
      video.pause();
    }
  });
  Object.defineProperty(api, "currentTime", {
    get: function() {
      return seekoffset + video.currentTime;
    },
    set: function(value) {
      MistUtil.event.send("seeking", value, video);
      seekoffset = (value == "live" ? Infinity : value) - video.currentTime;
      controller.control.send({
        type: "seek",
        "seek_time": value == "live" ? "live" : value * 1e3
      });
      controller.control.addListener("seek").then(function(msg) {
        return controller.control.addListener("on_time");
      }).then(function(msg) {
        MistUtil.event.send("seeked", seekoffset, video);
        return video.play();
      }).catch(function() {
      });
    }
  });
  Object.defineProperty(api, "duration", {
    get: function() {
      if (MistVideo2.info.type == "live") {
        return duration + (last_on_time ? (/* @__PURE__ */ new Date()).getTime() - last_on_time._received.getTime() : 0) * 1e-3;
      }
      return duration;
    }
  });
  controller.control.addListener("set_speed", function(msg) {
    play_rate = msg.play_rate_curr;
  });
  Object.defineProperty(api, "playbackRate", {
    get: function() {
      if (play_rate) {
        switch (play_rate) {
          case "auto":
          case "fast-forward": {
            return 1;
          }
          default: {
            return play_rate;
          }
        }
      }
      return 1;
    },
    set: function(value) {
      control.send({
        type: "set_speed",
        play_rate: value == 1 ? "auto" : value
      });
    }
  });
  this.setTracks = function(obj) {
    obj.type = "tracks";
    control.send(obj);
  };
  if (window.RTCPeerConnection && controller.connection instanceof RTCPeerConnection) {
    this.getStats = function() {
      if (controller && controller.connection && controller.connection.connectionState == "connected") {
        return controller.connection.getStats().then(function(a) {
          var r = {
            audio: null,
            video: null
          };
          var obj = Object.fromEntries(a);
          for (var i3 in obj) {
            var s = obj[i3];
            switch (s.type) {
              case "inbound-rtp": {
                r[s.kind] = s;
                break;
              }
              case "data-channel": {
                var label;
                switch (s.label) {
                  case "MistControl": {
                    label = "Control Channel";
                    break;
                  }
                  case "*": {
                    label = "Metadata Channel";
                    break;
                  }
                  default: {
                    label = s.label;
                  }
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
    if ("decodingIssues" in MistVideo2.skin.blueprints) {
      var vars = ["nackCount", "pliCount", "packetsLost", "packetsReceived", "bytesReceived", "messagesReceived", "messagesSent"];
      for (var j in vars) {
        api[vars[j]] = {};
      }
      api.jitterDelay = {};
      var last_stats;
      var f = function() {
        MistVideo2.timers.start(function() {
          var stats = api.getStats();
          if (stats) {
            stats.then(function(d) {
              for (var i3 in vars) {
                var v = vars[i3];
                var out = {};
                for (var channel in d) {
                  if (d[channel] && v in d[channel]) out[channel] = d[channel][v];
                }
                if (Object.keys(out).length) {
                  api[v] = out;
                }
              }
              api.jitterDelay = {};
              if (last_stats) {
                for (var channel in d) {
                  if (channel in last_stats && last_stats[channel] && d[channel] && "jitterBufferDelay" in d[channel]) {
                    api.jitterDelay[channel] = (d[channel].jitterBufferDelay - last_stats[channel].jitterBufferDelay) / (d[channel].jitterBufferEmittedCount - last_stats[channel].jitterBufferEmittedCount);
                  }
                }
              }
              last_stats = d;
            });
          }
          f();
        }, 1e3);
      };
      f();
      this.getLatency = function() {
        return api.jitterDelay;
      };
    }
  }
  if (controller.meta && MistVideo2.info && MistVideo2.info.capa && MistVideo2.info.capa.datachannels) {
    this.metaTrackSocket = function() {
      var converter = DataChannel2WebSocket();
      if (controller.meta) converter.init(controller.meta);
      else if (controller.connecting) {
        controller.connecting.then(function() {
          converter.init(controller.meta);
        });
      } else {
        MistVideo2.log("Failed to attach to MetaTrack datachannel", "error");
      }
      Object.defineProperty(converter, "debugging", {
        get: function() {
          return MistVideo2.player.debugging;
        }
      });
      return converter;
    };
  }
  this.ABR_resize = function(size) {
    MistVideo2.log("Requesting the video track with the resolution that best matches the player size");
    this.setTracks({ video: "~" + [size.width, size.height].join("x") });
  };
  this.unload = function() {
    controller.control.send({ type: "stop" });
    controller.connection.close();
  };
}

// src/wrappers/html5.js
registerWrapper("html5", {
  name: "HTML5 video player",
  mimes: ["html5/application/vnd.apple.mpegurl", "html5/application/vnd.apple.mpegurl;version=7", "html5/video/mp4", "html5/video/ogg", "html5/video/webm", "html5/audio/mp3", "html5/audio/webm", "html5/audio/ogg", "html5/audio/wav"],
  isMimeSupported: function(mimetype) {
    return MistUtil.array.indexOf(this.mimes, mimetype) == -1 ? false : true;
  },
  isBrowserSupported: function(mimetype, source, MistVideo2) {
    if (location.protocol != MistUtil.http.url.split(source.url).protocol) {
      if (location.protocol == "file:" && MistUtil.http.url.split(source.url).protocol == "http:") {
        MistVideo2.log("This page was loaded over file://, the player might not behave as intended.");
      } else {
        MistVideo2.log("HTTP/HTTPS mismatch for this source");
        return false;
      }
    }
    if (mimetype == "html5/application/vnd.apple.mpegurl") {
      var android = MistUtil.getAndroid();
      if (android && parseFloat(android) < 7) {
        MistVideo2.log("Skipping native HLS as videojs will do better");
        return false;
      }
    }
    if (mimetype == "html5/video/webm" && MistUtil.getBrowser() == "safari") {
      MistVideo2.log("Skipping html5/webm, as safari and webm are not friends");
      return false;
    }
    var support = false;
    var shortmime = mimetype.split("/");
    shortmime.shift();
    try {
      let translateCodec = function(track) {
        if (track.codecstring) {
          return track.codecstring;
        }
        function bin2hex(index) {
          return ("0" + track.init.charCodeAt(index).toString(16)).slice(-2);
        }
        switch (track.codec) {
          case "AAC":
            return "mp4a.40.2";
          case "MP3":
            return "mp4a.40.34";
          case "AC3":
            return "ec-3";
          case "H264":
            return "avc1." + bin2hex(1) + bin2hex(2) + bin2hex(3);
          case "HEVC":
            return "hev1." + bin2hex(1) + bin2hex(6) + bin2hex(7) + bin2hex(8) + bin2hex(9) + bin2hex(10) + bin2hex(11) + bin2hex(12);
          default:
            return track.codec.toLowerCase();
        }
      }, test = function(codecs2) {
        var v = document.createElement("video");
        if (v && typeof v.canPlayType == "function") {
          var result;
          switch (shortmime) {
            case "video/webm": {
              result = v.canPlayType(shortmime);
              break;
            }
            case "video/mp4":
            case "html5/application/vnd.apple.mpegurl":
            default: {
              result = v.canPlayType(shortmime + ';codecs="' + codecs2 + '"');
              break;
            }
          }
          if (result != "") {
            return result;
          }
        }
        return false;
      };
      shortmime = shortmime.join("/");
      var codecs = {};
      var playabletracks = {};
      var hassubtitles = false;
      for (var i2 in MistVideo2.info.meta.tracks) {
        if (MistVideo2.info.meta.tracks[i2].type != "meta") {
          codecs[translateCodec(MistVideo2.info.meta.tracks[i2])] = MistVideo2.info.meta.tracks[i2];
        } else if (MistVideo2.info.meta.tracks[i2].codec == "subtitle") {
          hassubtitles = true;
        }
      }
      var container = mimetype.split("/")[2];
      source.supportedCodecs = [];
      for (var i2 in codecs) {
        var s = test(i2);
        if (s) {
          source.supportedCodecs.push(codecs[i2].codec);
          playabletracks[codecs[i2].type] = 1;
        }
      }
      if (hassubtitles) {
        for (var i2 in MistVideo2.info.source) {
          if (MistVideo2.info.source[i2].type == "html5/text/vtt") {
            playabletracks.subtitle = 1;
            break;
          }
        }
      }
      support = MistUtil.object.keys(playabletracks);
    } catch (e) {
    }
    return support;
  },
  player: function() {
    this.onreadylist = [];
  },
  mistControls: true
});
var p = mistplayers.html5.player;
p.prototype = new MistPlayer();
p.prototype.build = function(MistVideo2, callback) {
  var shortmime = MistVideo2.source.type.split("/");
  shortmime.shift();
  var video = document.createElement("video");
  video.setAttribute("crossorigin", "anonymous");
  video.setAttribute("playsinline", "");
  var source = document.createElement("source");
  source.setAttribute("src", MistVideo2.source.url);
  video.source = source;
  video.appendChild(source);
  source.type = shortmime.join("/");
  var attrs = ["autoplay", "loop", "poster"];
  for (var i2 in attrs) {
    var attr = attrs[i2];
    if (MistVideo2.options[attr]) {
      video.setAttribute(attr, MistVideo2.options[attr] === true ? "" : MistVideo2.options[attr]);
    }
  }
  if (MistVideo2.options.muted) {
    video.muted = true;
  }
  if (MistVideo2.options.controls == "stock") {
    video.setAttribute("controls", "");
  }
  if (MistVideo2.info.type == "live") {
    video.loop = false;
  }
  if ("Proxy" in window && "Reflect" in window) {
    var overrides = {
      get: {},
      set: {}
    };
    MistVideo2.player.api = new Proxy(video, {
      get: function(target, key, receiver) {
        if (key in overrides.get) {
          return overrides.get[key].apply(target, arguments);
        }
        var method = target[key];
        if (typeof method === "function") {
          return function() {
            return method.apply(target, arguments);
          };
        }
        return method;
      },
      set: function(target, key, value) {
        if (key in overrides.set) {
          overrides.set[key].call(target, value);
          return true;
        }
        target[key] = value;
        return true;
      }
    });
    if (MistVideo2.source.type == "html5/audio/mp3") {
      overrides.set.currentTime = function() {
        MistVideo2.log("Seek attempted, but MistServer does not currently support seeking in MP3.");
        return false;
      };
    }
    if (MistVideo2.info.type == "live") {
      overrides.get.duration = function() {
        var buffer_end = 0;
        if (this.buffered.length) {
          buffer_end = this.buffered.end(this.buffered.length - 1);
        }
        var time_since_buffer = ((/* @__PURE__ */ new Date()).getTime() - MistVideo2.player.api.lastProgress.getTime()) * 1e-3;
        return buffer_end + time_since_buffer - MistVideo2.player.api.liveOffset;
      };
      overrides.set.currentTime = function(value) {
        var offset = value - MistVideo2.player.api.duration;
        if (offset > 0) {
          offset = 0;
        }
        MistVideo2.player.api.liveOffset = offset;
        MistVideo2.log("Seeking to " + MistUtil.format.time(value) + " (" + Math.round(offset * -10) / 10 + "s from live)");
        var params = { startunix: offset };
        if (offset == 0) {
          params = {};
        }
        MistVideo2.player.api.setSource(MistUtil.http.url.addParam(MistVideo2.source.url, params));
      };
      MistUtil.event.addListener(video, "progress", function() {
        MistVideo2.player.api.lastProgress = /* @__PURE__ */ new Date();
      });
      MistVideo2.player.api.lastProgress = /* @__PURE__ */ new Date();
      MistVideo2.player.api.liveOffset = 0;
      MistUtil.event.addListener(video, "pause", function() {
        MistVideo2.player.api.pausedAt = /* @__PURE__ */ new Date();
      });
      overrides.get.play = function() {
        return function() {
          if (MistVideo2.player.api.paused && MistVideo2.player.api.pausedAt && /* @__PURE__ */ new Date() - MistVideo2.player.api.pausedAt > 5e3) {
            video.load();
            MistVideo2.log("Reloading source..");
          }
          return video.play.apply(video, arguments);
        };
      };
      var otherdurationoverride = overrides.get.duration;
      overrides.get.duration = function() {
        return otherdurationoverride.apply(this, arguments) - MistVideo2.player.api.liveOffset + MistVideo2.info.lastms * 1e-3;
      };
      overrides.get.currentTime = function() {
        return this.currentTime - MistVideo2.player.api.liveOffset + MistVideo2.info.lastms * 1e-3;
      };
      overrides.get.buffered = function() {
        var video2 = this;
        return {
          length: video2.buffered.length,
          start: function(i3) {
            return video2.buffered.start(i3) - MistVideo2.player.api.liveOffset + MistVideo2.info.lastms * 1e-3;
          },
          end: function(i3) {
            return video2.buffered.end(i3) - MistVideo2.player.api.liveOffset + MistVideo2.info.lastms * 1e-3;
          }
        };
      };
    } else {
      if (!isFinite(video.duration)) {
        var duration = 0;
        for (var i2 in MistVideo2.info.meta.tracks) {
          duration = Math.max(duration, MistVideo2.info.meta.tracks[i2].lastms);
        }
        overrides.get.duration = function() {
          if (isFinite(this.duration)) {
            return this.duration;
          }
          return duration * 1e-3;
        };
      }
    }
  } else {
    MistVideo2.player.api = video;
  }
  MistVideo2.player.api.setSource = function(url) {
    if (url != this.source.src) {
      this.source.src = url;
      this.load();
    }
  };
  MistVideo2.player.api.setSubtitle = function(trackmeta) {
    var tracks = video.getElementsByTagName("track");
    for (var i3 = tracks.length - 1; i3 >= 0; i3--) {
      video.removeChild(tracks[i3]);
    }
    if (trackmeta) {
      var track = document.createElement("track");
      video.appendChild(track);
      track.kind = "subtitles";
      track.label = trackmeta.label;
      track.srclang = trackmeta.lang;
      track.src = trackmeta.src;
      track.setAttribute("default", "");
    }
  };
  MistVideo2.player.setSize = function(size) {
    this.api.style.width = size.width + "px";
    this.api.style.height = size.height + "px";
  };
  callback(video);
};

// src/wrappers/hlsjs.js
registerWrapper("hlsjs", {
  name: "HLS.js player",
  mimes: ["html5/application/vnd.apple.mpegurl", "html5/application/vnd.apple.mpegurl;version=7"],
  isMimeSupported: function(mimetype) {
    return this.mimes.indexOf(mimetype) == -1 ? false : true;
  },
  isBrowserSupported: function(mimetype, source, MistVideo2) {
    if (location.protocol != MistUtil.http.url.split(source.url).protocol) {
      MistVideo2.log("HTTP/HTTPS mismatch for this source");
      return false;
    }
    if (!("MediaSource" in window)) {
      return false;
    }
    if (!MediaSource.isTypeSupported) {
      return true;
    }
    var playabletracks = {};
    var hassubtitles = false;
    for (var i2 in MistVideo2.info.meta.tracks) {
      if (MistVideo2.info.meta.tracks[i2].type == "meta") {
        if (MistVideo2.info.meta.tracks[i2].codec == "subtitle") {
          hassubtitles = true;
        }
        continue;
      }
      if (!(MistVideo2.info.meta.tracks[i2].type in playabletracks)) {
        playabletracks[MistVideo2.info.meta.tracks[i2].type] = {};
      }
      playabletracks[MistVideo2.info.meta.tracks[i2].type][MistUtil.tracks.translateCodec(MistVideo2.info.meta.tracks[i2])] = 1;
    }
    var tracktypes = [];
    for (var type in playabletracks) {
      var playable = false;
      for (var codec in playabletracks[type]) {
        if (MediaSource.isTypeSupported('video/mp4;codecs="' + codec + '"')) {
          playable = true;
          break;
        }
      }
      if (playable) {
        tracktypes.push(type);
      }
    }
    if (hassubtitles) {
      for (var i2 in MistVideo2.info.source) {
        if (MistVideo2.info.source[i2].type == "html5/text/vtt") {
          tracktypes.push("subtitle");
          break;
        }
      }
    }
    return tracktypes.length ? tracktypes : false;
  },
  player: function() {
  },
  scriptsrc: function(host) {
    return host + "/hlsjs.js";
  }
});
var p2 = mistplayers.hlsjs.player;
p2.prototype = new MistPlayer();
p2.prototype.build = function(MistVideo2, callback) {
  var me = this;
  var video = document.createElement("video");
  video.setAttribute("playsinline", "");
  var attrs = ["autoplay", "loop", "poster"];
  for (var i2 in attrs) {
    var attr = attrs[i2];
    if (MistVideo2.options[attr]) {
      video.setAttribute(attr, MistVideo2.options[attr] === true ? "" : MistVideo2.options[attr]);
    }
  }
  if (MistVideo2.options.muted) {
    video.muted = true;
  }
  if (MistVideo2.info.type == "live") {
    video.loop = false;
  }
  if (MistVideo2.options.controls == "stock") {
    video.setAttribute("controls", "");
  }
  video.setAttribute("crossorigin", "anonymous");
  this.setSize = function(size) {
    video.style.width = size.width + "px";
    video.style.height = size.height + "px";
  };
  this.api = video;
  MistVideo2.player.api.unload = function() {
    if (MistVideo2.player.hls) {
      MistVideo2.player.hls.destroy();
      MistVideo2.player.hls = false;
      MistVideo2.log("hls.js instance disposed");
    }
  };
  function init(url) {
    MistVideo2.player.hls = new Hls({
      maxBufferLength: 15,
      maxMaxBufferLength: 60,
      manifestLoadingTimeOut: 6e4
    });
    MistVideo2.player.hls.attachMedia(video);
    MistVideo2.player.hls.on(Hls.Events.MEDIA_ATTACHED, function() {
      MistVideo2.player.hls.loadSource(url);
    });
  }
  MistVideo2.player.api.setSource = function(url) {
    if (!MistVideo2.player.hls) {
      return;
    }
    if (MistVideo2.player.hls.url != url) {
      MistVideo2.player.hls.destroy();
      init(url);
    }
  };
  MistVideo2.player.api.setSubtitle = function(trackmeta) {
    var tracks = video.getElementsByTagName("track");
    for (var i3 = tracks.length - 1; i3 >= 0; i3--) {
      video.removeChild(tracks[i3]);
    }
    if (trackmeta) {
      var track = document.createElement("track");
      video.appendChild(track);
      track.kind = "subtitles";
      track.label = trackmeta.label;
      track.srclang = trackmeta.lang;
      track.src = trackmeta.src;
      track.setAttribute("default", "");
    }
  };
  function onHLSjsLoad() {
    init(MistVideo2.source.url);
  }
  if ("Hls" in window) {
    onHLSjsLoad();
  } else {
    var scripturl = MistVideo2.urlappend(mistplayers.hlsjs.scriptsrc(MistVideo2.options.host));
    MistUtil.scripts.insert(scripturl, {
      onerror: function(e) {
        var msg = "Failed to load hlsjs.js";
        if (e.message) {
          msg += ": " + e.message;
        }
        MistVideo2.showError(msg);
      },
      onload: onHLSjsLoad
    }, MistVideo2);
  }
  callback(video);
};

// src/wrappers/dashjs.js
registerWrapper("dashjs", {
  name: "Dash.js player",
  mimes: [
    "dash/video/mp4"
    /*,"html5/application/vnd.ms-ss"*/
  ],
  isMimeSupported: function(mimetype) {
    return MistUtil.array.indexOf(this.mimes, mimetype) == -1 ? false : true;
  },
  isBrowserSupported: function(mimetype, source, MistVideo2) {
    if (location.protocol != MistUtil.http.url.split(source.url).protocol) {
      MistVideo2.log("HTTP/HTTPS mismatch for this source");
      return false;
    }
    if (location.protocol == "file:") {
      MistVideo2.log("This source (" + mimetype + ") won't load if the page is run via file://");
      return false;
    }
    if (!("MediaSource" in window)) {
      return false;
    }
    if (!MediaSource.isTypeSupported) {
      return true;
    }
    var playabletracks = {};
    var hassubtitles = false;
    for (var i2 in MistVideo2.info.meta.tracks) {
      if (MistVideo2.info.meta.tracks[i2].type == "meta") {
        if (MistVideo2.info.meta.tracks[i2].codec == "subtitle") {
          hassubtitles = true;
        }
        continue;
      }
      if (!(MistVideo2.info.meta.tracks[i2].type in playabletracks)) {
        playabletracks[MistVideo2.info.meta.tracks[i2].type] = {};
      }
      playabletracks[MistVideo2.info.meta.tracks[i2].type][MistUtil.tracks.translateCodec(MistVideo2.info.meta.tracks[i2])] = 1;
    }
    var tracktypes = [];
    for (var type in playabletracks) {
      var playable = false;
      for (var codec in playabletracks[type]) {
        if (MediaSource.isTypeSupported('video/mp4;codecs="' + codec + '"')) {
          playable = true;
          break;
        }
      }
      if (playable) {
        tracktypes.push(type);
      }
    }
    if (hassubtitles) {
      for (var i2 in MistVideo2.info.source) {
        if (MistVideo2.info.source[i2].type == "html5/text/vtt") {
          tracktypes.push("subtitle");
          break;
        }
      }
    }
    return tracktypes.length ? tracktypes : false;
  },
  player: function() {
    this.onreadylist = [];
  },
  scriptsrc: function(host) {
    return host + "/dashjs.js";
  }
});
var p3 = mistplayers.dashjs.player;
p3.prototype = new MistPlayer();
p3.prototype.build = function(MistVideo2, callback) {
  var me = this;
  this.onDashLoad = function() {
    if (MistVideo2.destroyed) {
      return;
    }
    MistVideo2.log("Building DashJS player..");
    var ele = document.createElement("video");
    if ("Proxy" in window) {
      var overrides = {
        get: {},
        set: {}
      };
      MistVideo2.player.api = new Proxy(ele, {
        get: function(target, key, receiver) {
          if (key in overrides.get) {
            return overrides.get[key].apply(target, arguments);
          }
          var method = target[key];
          if (typeof method === "function") {
            return function() {
              return method.apply(target, arguments);
            };
          }
          return method;
        },
        set: function(target, key, value) {
          if (key in overrides.set) {
            overrides.set[key].call(target, value);
            return true;
          }
          target[key] = value;
          return true;
        }
      });
      if (MistVideo2.info.type == "live") {
        overrides.get.duration = function() {
          var buffer_end = 0;
          if (this.buffered.length) {
            buffer_end = this.buffered.end(this.buffered.length - 1);
          }
          var time_since_buffer = ((/* @__PURE__ */ new Date()).getTime() - MistVideo2.player.api.lastProgress.getTime()) * 1e-3;
          return buffer_end + time_since_buffer;
        };
        MistUtil.event.addListener(ele, "progress", function() {
          MistVideo2.player.api.lastProgress = /* @__PURE__ */ new Date();
        });
        MistVideo2.player.api.lastProgress = /* @__PURE__ */ new Date();
      }
    } else {
      me.api = ele;
    }
    if (MistVideo2.options.autoplay) {
      ele.setAttribute("autoplay", "");
    }
    if (MistVideo2.options.loop && MistVideo2.info.type != "live") {
      ele.setAttribute("loop", "");
    }
    if (MistVideo2.options.poster) {
      ele.setAttribute("poster", MistVideo2.options.poster);
    }
    if (MistVideo2.options.muted) {
      ele.muted = true;
    }
    if (MistVideo2.options.controls == "stock") {
      ele.setAttribute("controls", "");
    }
    var player = dashjs.MediaPlayer().create();
    player.initialize(ele, MistVideo2.source.url, MistVideo2.options.autoplay);
    me.dash = player;
    var skipEvents = ["METRIC_ADDED", "METRIC_UPDATED", "METRIC_CHANGED", "METRICS_CHANGED", "FRAGMENT_LOADING_STARTED", "FRAGMENT_LOADING_COMPLETED", "LOG", "PLAYBACK_TIME_UPDATED", "PLAYBACK_PROGRESS"];
    for (var i2 in dashjs.MediaPlayer.events) {
      if (skipEvents.indexOf(i2) < 0) {
        me.dash.on(dashjs.MediaPlayer.events[i2], function(e) {
          MistVideo2.log("Player event fired: " + e.type);
        });
      }
    }
    MistVideo2.player.setSize = function(size) {
      this.api.style.width = size.width + "px";
      this.api.style.height = size.height + "px";
    };
    MistVideo2.player.api.setSource = function(url) {
      MistVideo2.player.dash.attachSource(url);
    };
    if (MistVideo2.options.controls != "stock") {
      me.dash.updateSettings({ streaming: { text: { defaultEnabled: false } } });
    }
    var subsloaded = false;
    me.dash.on("allTextTracksAdded", function() {
      subsloaded = true;
    });
    MistVideo2.player.api.setSubtitle = function(trackmeta) {
      if (!subsloaded) {
        var f = function() {
          MistVideo2.player.api.setSubtitle(trackmeta);
          me.dash.off("allTextTracksAdded", f);
        };
        me.dash.on("allTextTracksAdded", f);
        return;
      }
      if (!trackmeta) {
        me.dash.enableText(false);
        return;
      }
      var dashsubs = me.dash.getTracksFor("text");
      for (var i3 in dashsubs) {
        var trackid = "idx" in trackmeta ? trackmeta.idx : trackmeta.trackid;
        if (dashsubs[i3].id == trackid) {
          me.dash.setTextTrack(i3);
          if (!me.dash.isTextEnabled()) {
            me.dash.enableText();
          }
          return true;
        }
      }
      return false;
    };
    MistUtil.event.addListener(ele, "progress", function(e) {
      if (MistVideo2.container.getAttribute("data-loading") == "stalled") {
        MistVideo2.container.removeAttribute("data-loading");
      }
    });
    me.api.unload = function() {
      me.dash.reset();
    };
    MistVideo2.log("Built html");
    callback(ele);
  };
  if ("dashjs" in window) {
    this.onDashLoad();
  } else {
    var scripttag = MistUtil.scripts.insert(MistVideo2.urlappend(mistplayers.dashjs.scriptsrc(MistVideo2.options.host)), {
      onerror: function(e) {
        var msg = "Failed to load dashjs.js";
        if (e.message) {
          msg += ": " + e.message;
        }
        MistVideo2.showError(msg);
      },
      onload: me.onDashLoad
    }, MistVideo2);
  }
};

// src/wrappers/videojs.js
registerWrapper("videojs", {
  name: "VideoJS player",
  mimes: ["html5/application/vnd.apple.mpegurl", "html5/application/vnd.apple.mpegurl;version=7"],
  isMimeSupported: function(mimetype) {
    return MistUtil.array.indexOf(this.mimes, mimetype) == -1 ? false : true;
  },
  isBrowserSupported: function(mimetype, source, MistVideo2) {
    if (location.protocol != MistUtil.http.url.split(source.url).protocol) {
      MistVideo2.log("HTTP/HTTPS mismatch for this source");
      return false;
    }
    if (location.protocol == "file:" && mimetype == "html5/application/vnd.apple") {
      MistVideo2.log("This source (" + mimetype + ") won't load if the page is run via file://");
      return false;
    }
    function checkPlaybackOfTrackTypes(mime) {
      if (!MediaSource.isTypeSupported) {
        return true;
      }
      var playabletracks = {};
      var hassubtitles = false;
      for (var i2 in MistVideo2.info.meta.tracks) {
        if (MistVideo2.info.meta.tracks[i2].type == "meta") {
          if (MistVideo2.info.meta.tracks[i2].codec == "subtitle") {
            hassubtitles = true;
          }
          continue;
        }
        if (!(MistVideo2.info.meta.tracks[i2].type in playabletracks)) {
          playabletracks[MistVideo2.info.meta.tracks[i2].type] = {};
        }
        playabletracks[MistVideo2.info.meta.tracks[i2].type][MistUtil.tracks.translateCodec(MistVideo2.info.meta.tracks[i2])] = 1;
      }
      var tracktypes = [];
      for (var type in playabletracks) {
        var playable = false;
        for (var codec in playabletracks[type]) {
          if (MediaSource.isTypeSupported(mime + ';codecs="' + codec + '"')) {
            playable = true;
            break;
          }
        }
        if (playable) {
          tracktypes.push(type);
        }
      }
      if (hassubtitles) {
        for (var i2 in MistVideo2.info.source) {
          if (MistVideo2.info.source[i2].type == "html5/text/vtt") {
            tracktypes.push("subtitle");
            break;
          }
        }
      }
      return tracktypes.length ? tracktypes : false;
    }
    if (document.createElement("video").canPlayType(mimetype.replace("html5/", ""))) {
      if (!("MediaSource" in window)) {
        return true;
      }
      if (!MediaSource.isTypeSupported) {
        return true;
      }
      return checkPlaybackOfTrackTypes(mimetype.replace("html5/", ""));
    }
    if (!("MediaSource" in window)) {
      return false;
    }
    return checkPlaybackOfTrackTypes("video/mp4");
  },
  player: function() {
  },
  scriptsrc: function(host) {
    return host + "/videojs.js";
  }
});
var p4 = mistplayers.videojs.player;
p4.prototype = new MistPlayer();
p4.prototype.build = function(MistVideo2, callback) {
  var me = this;
  var ele;
  function onVideoJSLoad() {
    if (MistVideo2.destroyed) {
      return;
    }
    MistVideo2.log("Building VideoJS player..");
    ele = document.createElement("video");
    if (MistVideo2.source.type != "html5/video/ogg") {
      ele.crossOrigin = "anonymous";
    }
    ele.setAttribute("playsinline", "");
    var shortmime = MistVideo2.source.type.split("/");
    if (shortmime[0] == "html5") {
      shortmime.shift();
    }
    var source = document.createElement("source");
    source.setAttribute("src", MistVideo2.source.url);
    me.source = source;
    ele.appendChild(source);
    source.type = shortmime.join("/");
    MistVideo2.log("Adding " + source.type + " source @ " + MistVideo2.source.url);
    MistUtil.class.add(ele, "video-js");
    var vjsopts = {};
    if (MistVideo2.options.autoplay) {
      vjsopts.autoplay = true;
    }
    if (MistVideo2.options.loop && MistVideo2.info.type != "live") {
      ele.setAttribute("loop", "");
    }
    if (MistVideo2.options.muted) {
      ele.setAttribute("muted", "");
    }
    if (MistVideo2.options.poster) {
      vjsopts.poster = MistVideo2.options.poster;
    }
    if (MistVideo2.options.controls == "stock") {
      ele.setAttribute("controls", "");
      if (!document.getElementById("videojs-css")) {
        var style = document.createElement("link");
        style.rel = "stylesheet";
        style.href = MistVideo2.options.host + "/skins/videojs.css";
        style.id = "videojs-css";
        document.head.appendChild(style);
      }
    } else {
      vjsopts.controls = false;
    }
    var captureErrors = MistUtil.event.addListener(ele, "error", function(e) {
      e.stopImmediatePropagation();
      var msg = e.message;
      if (!msg && ele.error) {
        if ("code" in ele.error && ele.error.code) {
          msg = "Code " + ele.error.code;
          for (var i3 in ele.error) {
            if (i3 == "code") {
              continue;
            }
            if (ele.error[i3] == ele.error.code) {
              msg = i3;
              break;
            }
          }
        } else {
          msg = JSON.stringify(ele.error);
        }
      }
      MistVideo2.log("Error captured and stopped because videojs has not yet loaded: " + msg);
    });
    function androidVersion() {
      var match = navigator.userAgent.toLowerCase().match(/android\s([\d\.]*)/i);
      return match ? match[1] : false;
    }
    var android = MistUtil.getAndroid();
    if (android && parseFloat(android) < 7) {
      MistVideo2.log("Detected android < 7: instructing videojs to override native playback");
      vjsopts.html5 = { hls: { overrideNative: true } };
      vjsopts.nativeAudioTracks = false;
      vjsopts.nativeVideoTracks = false;
    }
    me.onready(function() {
      MistVideo2.log("Building videojs");
      me.videojs = videojs(ele, vjsopts, function() {
        MistUtil.event.removeListener(captureErrors);
        MistVideo2.log("Videojs initialized");
        if (MistVideo2.info.type == "live") {
          MistUtil.event.addListener(ele, "progress", function(e) {
            var i3 = MistVideo2.player.videojs.seekable().length - 1;
            MistVideo2.info.meta.buffer_window = (Math.max(MistVideo2.player.videojs.seekable().end(i3), ele.duration) - MistVideo2.player.videojs.seekable().start(i3)) * 1e3;
          });
        }
      });
      MistUtil.event.addListener(ele, "error", function(e) {
        if (e && e.target && e.target.error && e.target.error.message && MistUtil.array.indexOf(e.target.error.message, "NS_ERROR_DOM_MEDIA_OVERFLOW_ERR") >= 0) {
          MistVideo2.timers.start(function() {
            MistVideo2.log("Reloading player because of NS_ERROR_DOM_MEDIA_OVERFLOW_ERR");
            MistVideo2.reload();
          }, 1e3);
        }
      });
      me.api.unload = function() {
        if (me.videojs) {
          me.videojs.autoplay(false);
          me.videojs.pause();
          me.videojs.dispose();
          me.videojs = false;
          MistVideo2.log("Videojs instance disposed");
        }
      };
    });
    MistVideo2.log("Built html");
    if ("Proxy" in window && "Reflect" in window) {
      var overrides = {
        get: {},
        set: {}
      };
      MistVideo2.player.api = new Proxy(ele, {
        get: function(target, key, receiver) {
          if (key in overrides.get) {
            return overrides.get[key].apply(target, arguments);
          }
          var method = target[key];
          if (typeof method === "function") {
            return function() {
              return method.apply(target, arguments);
            };
          }
          return method;
        },
        set: function(target, key, value) {
          if (key in overrides.set) {
            overrides.set[key].call(target, value);
            return true;
          }
          target[key] = value;
          return true;
        }
      });
      MistVideo2.player.api.load = function() {
      };
      overrides.set.currentTime = function(value) {
        MistVideo2.player.videojs.currentTime(value);
      };
      var lastms = 0;
      var firstms = Infinity;
      for (var i2 in MistVideo2.info.meta.tracks) {
        lastms = Math.max(lastms, MistVideo2.info.meta.tracks[i2].lastms);
        firstms = Math.min(firstms, MistVideo2.info.meta.tracks[i2].firstms);
      }
      var correction = firstms * 1e-3;
      overrides.get.duration = function() {
        if (MistVideo2.info) {
          var duration = ele.duration;
          return duration + correction;
        }
        return 0;
      };
      MistUtil.event.addListener(ele, "progress", function() {
        MistVideo2.player.api.lastProgress = /* @__PURE__ */ new Date();
      });
      overrides.set.currentTime = function(value) {
        var diff = MistVideo2.player.api.currentTime - value;
        var offset = value - MistVideo2.player.api.duration;
        MistVideo2.log("Seeking to " + MistUtil.format.time(value) + " (" + Math.round(offset * -10) / 10 + "s from live)");
        MistVideo2.player.videojs.currentTime(MistVideo2.video.currentTime - diff);
      };
      overrides.get.currentTime = function() {
        var time = MistVideo2.player.videojs ? MistVideo2.player.videojs.currentTime() : ele.currentTime;
        if (isNaN(time)) {
          return 0;
        }
        return time + correction;
      };
      overrides.get.buffered = function() {
        var buffered = MistVideo2.player.videojs ? MistVideo2.player.videojs.buffered() : ele.buffered;
        return {
          length: buffered.length,
          start: function(i3) {
            return buffered.start(i3) + correction;
          },
          end: function(i3) {
            return buffered.end(i3) + correction;
          }
        };
      };
      if (MistVideo2.info.type == "live") {
        MistVideo2.player.api.lastProgress = /* @__PURE__ */ new Date();
        MistVideo2.player.api.liveOffset = 0;
      }
    } else {
      me.api = ele;
    }
    MistVideo2.player.setSize = function(size) {
      if ("videojs" in MistVideo2.player) {
        MistVideo2.player.videojs.dimensions(size.width, size.height);
        ele.parentNode.style.width = size.width + "px";
        ele.parentNode.style.height = size.height + "px";
      }
      this.api.style.width = size.width + "px";
      this.api.style.height = size.height + "px";
    };
    MistVideo2.player.api.setSource = function(url) {
      if (!MistVideo2.player.videojs) {
        return;
      }
      if (MistVideo2.player.videojs.src() != url) {
        MistVideo2.player.videojs.src({
          type: MistVideo2.player.videojs.currentSource().type,
          src: url
        });
      }
    };
    MistVideo2.player.api.setSubtitle = function(trackmeta) {
      var tracks = ele.getElementsByTagName("track");
      for (var i3 = tracks.length - 1; i3 >= 0; i3--) {
        ele.removeChild(tracks[i3]);
      }
      if (trackmeta) {
        var track = document.createElement("track");
        ele.appendChild(track);
        track.kind = "subtitles";
        track.label = trackmeta.label;
        track.srclang = trackmeta.lang;
        track.src = trackmeta.src;
        track.setAttribute("default", "");
      }
    };
    if (MistVideo2.info.type == "live") {
      var loadstart = MistUtil.event.addListener(ele, "loadstart", function(e) {
        MistUtil.event.removeListener(loadstart);
        MistUtil.event.send("canplay", false, this);
      });
      var canplay = MistUtil.event.addListener(ele, "canplay", function(e) {
        if (loadstart) {
          MistUtil.event.removeListener(loadstart);
        }
        MistUtil.event.removeListener(canplay);
      });
    }
    callback(ele);
  }
  if ("videojs" in window) {
    onVideoJSLoad();
  } else {
    let reloadVJSrateLimited = function() {
      try {
        MistVideo2.video.pause();
      } catch (e) {
      }
      MistVideo2.showError("Error in videojs player");
      if (!window.mistplayer_videojs_failures) {
        window.mistplayer_videojs_failures = 1;
        MistVideo2.reload();
      } else {
        if (!timer) {
          var delay = 0.05 * Math.pow(2, window.mistplayer_videojs_failures);
          MistVideo2.log("Rate limiter activated: MistPlayer reload delayed by " + Math.round(delay * 10) / 10 + " seconds.", "error");
          timer = MistVideo2.timers.start(function() {
            timer = false;
            delete window.videojs;
            MistVideo2.reload();
          }, delay * 1e3);
          window.mistplayer_videojs_failures++;
        }
      }
    };
    var timer = false;
    var scripturl = MistVideo2.urlappend(mistplayers.videojs.scriptsrc(MistVideo2.options.host));
    var scripttag;
    var f = function(msg, url, lineNo, columnNo, error) {
      if (!scripttag) {
        return;
      }
      if (url == scripttag.src) {
        window.removeEventListener("error", f);
        reloadVJSrateLimited();
      }
      return false;
    };
    window.addEventListener("error", f);
    scripttag = MistUtil.scripts.insert(scripturl, {
      onerror: function(e) {
        var msg = "Failed to load videojs.js";
        if (e.message) {
          msg += ": " + e.message;
        }
        MistVideo2.showError(msg);
      },
      onload: onVideoJSLoad
    }, MistVideo2);
  }
};

// src/wrappers/webrtc.js
registerWrapper("webrtc", {
  name: "WebRTC player (WS)",
  mimes: ["webrtc"],
  isMimeSupported: function(mimetype) {
    return this.mimes.indexOf(mimetype) == -1 ? false : true;
  },
  isBrowserSupported: function(mimetype, source, MistVideo2) {
    if (!("WebSocket" in window) || (!("RTCPeerConnection" in window) || !("RTCRtpReceiver" in window))) {
      return false;
    }
    if (location.protocol.replace(/^http/, "ws") != MistUtil.http.url.split(source.url.replace(/^http/, "ws")).protocol) {
      MistVideo2.log("HTTP/HTTPS mismatch for this source");
      return false;
    }
    var playabletracks = {};
    var hassubtitles = false;
    for (var i2 in MistVideo2.info.meta.tracks) {
      if (MistVideo2.info.meta.tracks[i2].type == "meta") {
        if (MistVideo2.info.meta.tracks[i2].codec == "subtitle") {
          hassubtitles = true;
        }
        continue;
      }
      if (!(MistVideo2.info.meta.tracks[i2].type in playabletracks)) {
        playabletracks[MistVideo2.info.meta.tracks[i2].type] = {};
      }
      playabletracks[MistVideo2.info.meta.tracks[i2].type][MistVideo2.info.meta.tracks[i2].codec] = 1;
    }
    var tracktypes = [];
    for (var type in playabletracks) {
      var playable = false;
      for (var codec in playabletracks[type]) {
        var supported = RTCRtpReceiver.getCapabilities(type).codecs;
        for (var i2 in supported) {
          if (supported[i2].mimeType.toLowerCase() == (type + "/" + codec).toLowerCase()) {
            playable = true;
            break;
          }
        }
      }
      if (playable) {
        tracktypes.push(type);
      }
    }
    if (hassubtitles) {
      tracktypes.push("subtitle");
    }
    return tracktypes.length ? tracktypes : false;
  },
  player: function() {
  }
});
var p5 = mistplayers.webrtc.player;
p5.prototype = new MistPlayer();
p5.prototype.build = function(MistVideo2, callback) {
  var main = this;
  var video = document.createElement("video");
  this.setSize = function(size) {
    video.style.width = size.width + "px";
    video.style.height = size.height + "px";
  };
  function myRTC() {
    var webrtc = this;
    this.connection = false;
    this.connecting = false;
    this.control = false;
    let was_connected = false;
    this.onmessage = {};
    this.connect = function() {
      if (this.connecting) {
        return this.connecting;
      }
      if (this.connection.connectionState == "connected") {
        return new Promise(function(resolve, reject) {
          resolve();
        });
      }
      MistVideo2.container.setAttribute("data-loading", "");
      var url = MistVideo2.source.url;
      MistVideo2.log("Connecting to " + url);
      this.control = new ControlChannel(new WebSocket(url), MistVideo2, this.onmessage);
      this.control.addListener("channel_timeout").then(function() {
        MistVideo2.log("WebRTC: control channel timeout - try next combo", "error");
        MistVideo2.nextCombo("control channel timeout");
      });
      this.control.addListener("channel_error").then(function() {
        if (webrtc.control.was_connected) {
          MistVideo2.log("Attempting to reconnect control channel");
          this.control = new ControlChannel(new WebSocket(url), MistVideo2, this.onmessage);
        } else {
          MistVideo2.log("WebRTC: control channel error - try next combo", "error");
          MistVideo2.nextCombo("control channel error");
        }
      });
      Object.defineProperty(this.control, "debugging", {
        get: function() {
          return main.debugging;
        }
      });
      this.step = 0;
      if (this.connection) this.connection.close();
      this.connection = new RTCPeerConnection();
      this.connection.onconnectionstatechange = function(e) {
        if (MistVideo2.destroyed) new Promise(function(resolve, reject) {
          reject();
        });
        switch (this.connectionState) {
          case "failed": {
            if (!was_connected) {
              MistVideo2.log("The WebRTC UDP connection failed, trying next combo.", "error");
              MistVideo2.nextCombo();
            } else {
              MistVideo2.log("The WebRTC UDP connection was closed");
            }
            break;
          }
          case "connected":
          case "disconnected":
          case "closed":
          case "new":
          case "connecting":
          default: {
            MistVideo2.log("The WebRTC UDP connection state changed to " + this.connectionState);
            break;
          }
        }
      };
      this.connection.oniceconnectionstatechange = function(e) {
        if (MistVideo2.destroyed) {
          return;
        }
        switch (this.iceConnectionState) {
          case "failed": {
            MistVideo2.showError("The WebRTC ICE connection " + this.iceConnectionState);
            break;
          }
          case "disconnected":
          case "closed":
          case "new":
          case "checking":
          case "connected":
          case "completed":
          default: {
            MistVideo2.log("The WebRTC ICE connection state changed to " + this.iceConnectionState);
            break;
          }
        }
      };
      this.connection.addEventListener("signalingstatechange", function() {
        MistVideo2.log("The WebRTC signaling state changed to " + this.signalingState);
      });
      this.connection.addTransceiver("audio", { direction: "recvonly" });
      this.connection.addTransceiver("video", { direction: "recvonly" });
      this.connection.ontrack = function(e) {
        if (MistVideo2.destroyed) {
          return;
        }
        if (main.debugging) console.log("Received media track", e.track);
        video.srcObject = e.streams[0];
      };
      this.meta = this.connection.createDataChannel("*", { "protocol": "JSON" });
      var offer, answer;
      this.connecting = this.connection.createOffer({ offerToReceiveVideo: true, offerToReceiveAudio: true }).then(function(o) {
        webrtc.step++;
        offer = o;
        if (main.debugging) console.log("Offer:", MistUtil.format.offer2human(offer.sdp), "State:", webrtc.connection.connectionState);
        return webrtc.connection.setLocalDescription(offer);
      }).then(function() {
        webrtc.step++;
        webrtc.control.send({ type: "offer_sdp", offer_sdp: offer.sdp });
        return webrtc.control.addListener("on_answer_sdp");
      }).then(function(a) {
        webrtc.step++;
        answer = a.answer_sdp;
        if (main.debugging) console.log("Answer:", MistUtil.format.offer2human(answer));
        return webrtc.connection.setRemoteDescription({ type: "answer", sdp: answer });
      }).then(function() {
        webrtc.step++;
        MistVideo2.log("Connected to " + url);
        webrtc.connecting = false;
        was_connected = true;
        return answer;
      }).catch(function(e) {
        webrtc.connecting = false;
        MistVideo2.showError("WebRTC connection failed: " + e);
      });
      return this.connecting;
    };
    this.close = function() {
      return new Promise(function(resolve, reject) {
        if (!webrtc.connection || webrtc.connection.connectionState == "closed") {
          resolve();
        }
        webrtc.connection.close();
        var func = function() {
          if (!webrtc.connection || webrtc.connection.connectionState == "closed") {
            resolve();
          } else {
            console.warn("not yet", webrtc.connection.connectionState);
            MistVideo2.timers.start(function() {
              func();
            }, 100);
          }
        };
        func();
      });
    };
    this.connect();
  }
  this.webrtc = new myRTC();
  this.api = new ControlChannelAPI(main.webrtc, MistVideo2, video);
  callback(video);
};

// src/wrappers/wheprtc.js
registerWrapper("wheprtc", {
  name: "WebRTC player (WHEP)",
  mimes: ["whep"],
  isMimeSupported: function(mimetype) {
    return this.mimes.indexOf(mimetype) == -1 ? false : true;
  },
  isBrowserSupported: function(mimetype, source, MistVideo2) {
    if (!("RTCPeerConnection" in window) || !("RTCRtpReceiver" in window)) {
      return false;
    }
    if (!("capa" in MistVideo2.info) || !("datachannels" in MistVideo2.info.capa) || !MistVideo2.info.capa.datachannels) {
      return false;
    }
    if (location.protocol != MistUtil.http.url.split(source.url).protocol) {
      MistVideo2.log("HTTP/HTTPS mismatch for this source");
      return false;
    }
    var playabletracks = {};
    var hassubtitles = false;
    for (var i2 in MistVideo2.info.meta.tracks) {
      if (MistVideo2.info.meta.tracks[i2].type == "meta") {
        if (MistVideo2.info.meta.tracks[i2].codec == "subtitle") {
          hassubtitles = true;
        }
        continue;
      }
      if (!(MistVideo2.info.meta.tracks[i2].type in playabletracks)) {
        playabletracks[MistVideo2.info.meta.tracks[i2].type] = {};
      }
      playabletracks[MistVideo2.info.meta.tracks[i2].type][MistVideo2.info.meta.tracks[i2].codec] = 1;
    }
    var tracktypes = [];
    for (var type in playabletracks) {
      var playable = false;
      for (var codec in playabletracks[type]) {
        var supported = RTCRtpReceiver.getCapabilities(type).codecs;
        for (var i2 in supported) {
          if (supported[i2].mimeType.toLowerCase() == (type + "/" + codec).toLowerCase()) {
            playable = true;
            break;
          }
        }
      }
      if (playable) {
        tracktypes.push(type);
      }
    }
    if (hassubtitles) {
      tracktypes.push("subtitle");
    }
    return tracktypes.length ? tracktypes : false;
  },
  player: function() {
  }
});
var p6 = mistplayers.wheprtc.player;
p6.prototype = new MistPlayer();
p6.prototype.build = function(MistVideo2, callback) {
  var main = this;
  var video = document.createElement("video");
  this.setSize = function(size) {
    video.style.width = size.width + "px";
    video.style.height = size.height + "px";
  };
  function myWHEP() {
    var whep = this;
    this.connection = { connectionState: "new" };
    this.connecting = false;
    this.control = false;
    let was_connected = false;
    this.onmessage = {};
    this.connect = function() {
      if (MistVideo2.destroyed) new Promise(function(resolve, reject) {
        reject();
      });
      if (this.connecting) {
        return this.connecting;
      }
      if (this.connection.connectionState == "connected") {
        return new Promise(function(resolve, reject) {
          resolve();
        });
      }
      MistVideo2.container.setAttribute("data-loading", "");
      var url = MistVideo2.source.url;
      MistVideo2.log("Connecting to " + url);
      this.connection = new RTCPeerConnection();
      this.connection.onconnectionstatechange = function(e) {
        if (MistVideo2.destroyed) {
          return;
        }
        switch (this.connectionState) {
          case "failed": {
            if (!was_connected) {
              MistVideo2.log("The WebRTC UDP connection failed, trying next combo.", "error");
              MistVideo2.nextCombo();
            } else {
              MistVideo2.log("The WebRTC UDP connection was closed");
            }
            break;
          }
          case "connected":
          case "disconnected":
          case "closed":
          case "new":
          case "connecting":
          default: {
            MistVideo2.log("The WebRTC UDP connection state changed to " + this.connectionState);
            break;
          }
        }
      };
      this.connection.oniceconnectionstatechange = function(e) {
        if (MistVideo2.destroyed) {
          return;
        }
        switch (this.iceConnectionState) {
          case "failed": {
            MistVideo2.showError("The WebRTC ICE connection " + this.iceConnectionState);
            break;
          }
          case "disconnected":
          case "closed":
          case "new":
          case "checking":
          case "connected":
          case "completed":
          default: {
            MistVideo2.log("The WebRTC ICE connection state changed to " + this.iceConnectionState);
            break;
          }
        }
      };
      this.connection.addEventListener("signalingstatechange", function() {
        MistVideo2.log("The WebRTC signaling state changed to " + this.signalingState);
      });
      this.connection.addTransceiver("audio", { direction: "recvonly" });
      this.connection.addTransceiver("video", { direction: "recvonly" });
      this.connection.ontrack = function(e) {
        if (MistVideo2.destroyed) {
          return;
        }
        if (main.debugging) console.log("Received media track", e.track);
        video.srcObject = e.streams[0];
      };
      this.control = new ControlChannel(this.connection.createDataChannel("MistControl"), MistVideo2, this.onmessage);
      this.control.addListener("channel_timeout").then(function() {
        MistVideo2.log("WebRTC: control channel timeout - try next combo", "error");
        MistVideo2.nextCombo("control channel timeout");
      });
      this.control.addListener("channel_error").then(function() {
        if (whep.control.was_connected) {
          MistVideo2.log("Attempting to reconnect control channel");
          this.control = new ControlChannel(whep.connection.createDataChannel("MistControl"), MistVideo2, this.onmessage);
        }
      });
      Object.defineProperty(this.control, "debugging", {
        get: function() {
          return main.debugging;
        }
      });
      this.meta = this.connection.createDataChannel("*", { "protocol": "JSON" });
      var offer, answer;
      this.connecting = this.connection.createOffer({ offerToReceiveVideo: true, offerToReceiveAudio: true }).then(function(o) {
        offer = o;
        if (main.debugging) console.log("Offer:", MistUtil.format.offer2human(offer.sdp));
        return whep.connection.setLocalDescription(offer);
      }).then(function() {
        return fetch(url, { method: "POST", headers: { "Content-Type": "application/sdp" }, body: offer.sdp });
      }).then(function(response) {
        return response.text();
      }).then(function(a) {
        answer = a;
        if (main.debugging) console.log("Answer:", MistUtil.format.offer2human(answer));
        return whep.connection.setRemoteDescription({ type: "answer", sdp: answer });
      }).then(function() {
        MistVideo2.log("Connected to " + url);
        whep.connecting = false;
        was_connected = true;
        return answer;
      }).catch(function(e) {
        whep.connecting = false;
        MistVideo2.showError("WHEP connection failed: " + e);
      });
      return this.connecting;
    };
    this.close = function() {
      return new Promise(function(resolve, reject) {
        if (!whep.connection || whep.connection.connectionState == "closed") {
          resolve();
        }
        whep.connection.close();
        var func = function() {
          if (!whep.connection || whep.connection.connectionState == "closed") {
            resolve();
          } else {
            console.warn("not yet", whep.connection.connectionState);
            MistVideo2.timers.start(function() {
              func();
            }, 100);
          }
        };
        func();
      });
    };
    this.connect();
  }
  this.WHEP = new myWHEP();
  this.api = new ControlChannelAPI(main.WHEP, MistVideo2, video);
  callback(video);
};

// src/wrappers/mews.js
registerWrapper("mews", {
  name: "MSE websocket player",
  mimes: ["ws/video/mp4", "ws/video/webm"],
  isMimeSupported: function(mimetype) {
    return this.mimes.indexOf(mimetype) == -1 ? false : true;
  },
  isBrowserSupported: function(mimetype, source, MistVideo2) {
    if (!("WebSocket" in window) || !("MediaSource" in window) || !("Promise" in window)) {
      return false;
    }
    if (MistVideo2.info.capa && !MistVideo2.info.capa.ssl) {
      MistVideo2.log("This player requires websocket support");
      return false;
    }
    if (location.protocol.replace(/^http/, "ws") != MistUtil.http.url.split(source.url.replace(/^http/, "ws")).protocol) {
      MistVideo2.log("HTTP/HTTPS mismatch for this source");
      return false;
    }
    if (navigator.platform.toUpperCase().indexOf("MAC") >= 0) {
      return false;
    }
    function translateCodec(track) {
      if (track.codecstring) {
        return track.codecstring;
      }
      function bin2hex(index) {
        return ("0" + track.init.charCodeAt(index).toString(16)).slice(-2);
      }
      switch (track.codec) {
        case "AAC":
          return "mp4a.40.2";
        case "MP3":
          return "mp4a.40.34";
        case "AC3":
          return "ec-3";
        case "H264":
          return "avc1." + bin2hex(1) + bin2hex(2) + bin2hex(3);
        case "HEVC":
          return "hev1." + bin2hex(1) + bin2hex(6) + bin2hex(7) + bin2hex(8) + bin2hex(9) + bin2hex(10) + bin2hex(11) + bin2hex(12);
        default:
          return track.codec.toLowerCase();
      }
    }
    var codecs = {};
    var playabletracks = {};
    var hassubtitles = false;
    for (var i2 in MistVideo2.info.meta.tracks) {
      if (MistVideo2.info.meta.tracks[i2].type != "meta") {
        codecs[translateCodec(MistVideo2.info.meta.tracks[i2])] = MistVideo2.info.meta.tracks[i2];
      } else if (MistVideo2.info.meta.tracks[i2].codec == "subtitle") {
        hassubtitles = true;
      }
    }
    var container = mimetype.split("/")[2];
    function test(codecs2) {
      return MediaSource.isTypeSupported("video/" + container + ';codecs="' + codecs2 + '"');
    }
    source.supportedCodecs = [];
    for (var i2 in codecs) {
      var s = test(i2);
      if (s) {
        source.supportedCodecs.push(codecs[i2].codec);
        playabletracks[codecs[i2].type] = 1;
      }
    }
    if (hassubtitles) {
      for (var i2 in MistVideo2.info.source) {
        if (MistVideo2.info.source[i2].type == "html5/text/vtt") {
          playabletracks.subtitle = 1;
          break;
        }
      }
    }
    return MistUtil.object.keys(playabletracks);
  },
  player: function() {
  }
});
var p7 = mistplayers.mews.player;
p7.prototype = new MistPlayer();
p7.prototype.build = function(MistVideo2, callback) {
  var video = document.createElement("video");
  video.setAttribute("playsinline", "");
  var attrs = ["autoplay", "loop", "poster"];
  for (var i2 in attrs) {
    var attr = attrs[i2];
    if (MistVideo2.options[attr]) {
      video.setAttribute(attr, MistVideo2.options[attr] === true ? "" : MistVideo2.options[attr]);
    }
  }
  if (MistVideo2.options.muted) {
    video.muted = true;
  }
  if (MistVideo2.info.type == "live") {
    video.loop = false;
  }
  if (MistVideo2.options.controls == "stock") {
    video.setAttribute("controls", "");
  }
  video.setAttribute("crossorigin", "anonymous");
  this.setSize = function(size) {
    video.style.width = size.width + "px";
    video.style.height = size.height + "px";
  };
  var player = this;
  player.built = false;
  function checkReady() {
    if (player.ws.readyState == player.ws.OPEN && player.ms.readyState == "open" && player.sb) {
      if (!player.built) {
        callback(video);
        player.built = true;
      }
      if (MistVideo2.options.autoplay) {
        player.api.play().catch(function() {
        });
      }
      return true;
    }
  }
  this.msoninit = [];
  this.msinit = function() {
    return new Promise(function(resolve, reject) {
      player.ms = new MediaSource();
      video.src = URL.createObjectURL(player.ms);
      player.ms.onsourceopen = function() {
        for (var i3 in player.msoninit) {
          player.msoninit[i3]();
        }
        player.msoninit = [];
        resolve();
      };
      player.ms.onsourceclose = function(e) {
        if (player.debugging) console.error("ms close", e);
        send({ type: "stop" });
      };
      player.ms.onsourceended = function(e) {
        if (player.debugging) console.error("ms ended", e);
        if (player.debugging == "dl") {
          let downloadBlob = function(data, fileName, mimeType) {
            var blob, url;
            blob = new Blob([data], {
              type: mimeType
            });
            url = window.URL.createObjectURL(blob);
            downloadURL(url, fileName);
            setTimeout(function() {
              return window.URL.revokeObjectURL(url);
            }, 1e3);
          }, downloadURL = function(data, fileName) {
            var a;
            a = document.createElement("a");
            a.href = data;
            a.download = fileName;
            document.body.appendChild(a);
            a.style = "display: none";
            a.click();
            a.remove();
          };
          ;
          ;
          var l = 0;
          for (var i3 = 0; i3 < player.sb.appended.length; i3++) {
            l += player.sb.appended[i3].length;
          }
          var d = new Uint8Array(l);
          var l = 0;
          for (var i3 = 0; i3 < player.sb.appended.length; i3++) {
            d.set(player.sb.appended[i3], l);
            l += player.sb.appended[i3].length;
          }
          downloadBlob(d, "appended.mp4.bin", "application/octet-stream");
        }
        send({ type: "stop" });
      };
    });
  };
  this.msinit().then(function() {
    if (player.sb) {
      MistVideo2.log("Not creating source buffer as one already exists.");
      checkReady();
      return;
    }
  });
  this.onsbinit = [];
  this.sbinit = function(codecs) {
    if (!codecs) {
      MistVideo2.showError("Did not receive any codec: nothing to initialize.");
      return;
    }
    player.sb = player.ms.addSourceBuffer("video/" + MistVideo2.source.type.split("/")[2] + ';codecs="' + codecs.join(",") + '"');
    player.sb.mode = "segments";
    player.sb._codecs = codecs;
    player.sb._size = 0;
    player.sb.queue = [];
    var do_on_updateend = [];
    player.sb.do_on_updateend = do_on_updateend;
    player.sb.appending = null;
    player.sb.appended = [];
    var n = 0;
    player.sb.addEventListener("updateend", function() {
      if (!player.sb) {
        MistVideo2.log("Reached updateend but the source buffer is " + JSON.stringify(player.sb) + ". ");
        return;
      }
      if (player.debugging) {
        if (player.sb.appending) player.sb.appended.push(player.sb.appending);
        player.sb.appending = null;
      }
      if (n >= 500) {
        n = 0;
        player.sb._clean(10);
      } else {
        n++;
      }
      var do_funcs = do_on_updateend.slice();
      do_on_updateend = [];
      for (var i4 in do_funcs) {
        if (!player.sb) {
          if (player.debugging) {
            console.warn("I was doing on_updateend but the sb was reset");
          }
          break;
        }
        if (player.sb.updating) {
          do_on_updateend.concat(do_funcs.slice(i4));
          if (player.debugging) {
            console.warn("I was doing on_updateend but was interrupted");
          }
          break;
        }
        do_funcs[i4](i4 < do_funcs.length - 1 ? do_funcs.slice(i4) : []);
      }
      if (!player.sb) {
        return;
      }
      player.sb._busy = false;
      if (player.sb && player.sb.queue.length > 0 && !player.sb.updating && !video.error) {
        player.sb._append(this.queue.shift());
      }
    });
    player.sb.error = function(e) {
      console.error("sb error", e);
    };
    player.sb.abort = function(e) {
      console.error("sb abort", e);
    };
    player.sb._doNext = function(func) {
      do_on_updateend.push(func);
    };
    player.sb._do = function(func) {
      if (this.updating || this._busy) {
        this._doNext(func);
      } else {
        func();
      }
    };
    player.sb._append = function(data) {
      if (!data) {
        return;
      }
      if (!data.buffer) {
        return;
      }
      if (player.debugging) {
        player.sb.appending = new Uint8Array(data);
      }
      if (player.sb._busy) {
        if (player.debugging) console.warn("I wanted to append data, but now I won't because the thingy was still busy. Putting it back in the queue.");
        player.sb.queue.unshift(data);
        return;
      }
      player.sb._busy = true;
      try {
        player.sb.appendBuffer(data);
      } catch (e) {
        switch (e.name) {
          case "QuotaExceededError": {
            if (video.buffered.length) {
              if (video.currentTime - video.buffered.start(0) > 1) {
                MistVideo2.log("Triggered QuotaExceededError: cleaning up " + Math.round((video.currentTime - video.buffered.start(0) - 1) * 10) / 10 + "s");
                player.sb._clean(1);
              } else {
                var bufferEnd = video.buffered.end(video.buffered.length - 1);
                MistVideo2.log("Triggered QuotaExceededError but there is nothing to clean: skipping ahead " + Math.round((bufferEnd - video.currentTime) * 10) / 10 + "s");
                video.currentTime = bufferEnd;
              }
              player.sb._busy = false;
              player.sb._append(data);
              return;
            }
            break;
          }
          case "InvalidStateError": {
            player.api.pause();
            if (MistVideo2.video.error) {
              return;
            }
            break;
          }
        }
        MistVideo2.showError(e.message);
      }
    };
    if (player.msgqueue) {
      if (player.msgqueue[0]) {
        var do_do = false;
        if (player.msgqueue[0].length) {
          for (var i3 in player.msgqueue[0]) {
            if (player.sb.updating || player.sb.queue.length || player.sb._busy) {
              player.sb.queue.push(player.msgqueue[0][i3]);
            } else {
              player.sb._append(player.msgqueue[0][i3]);
            }
          }
        } else {
          do_do = true;
        }
        player.msgqueue.shift();
        if (player.msgqueue.length == 0) {
          player.msgqueue = false;
        }
        MistVideo2.log("The newly initialized source buffer was filled with data from a separate message queue." + (player.msgqueue ? " " + player.msgqueue.length + " more message queue(s) remain." : ""));
        if (do_do) {
          MistVideo2.log("The separate message queue was empty; manually triggering any onupdateend functions");
          player.sb.dispatchEvent(new Event("updateend"));
        }
      }
    }
    player.sb._clean = function(keepaway) {
      if (!keepaway) keepaway = 180;
      if (video.currentTime > keepaway) {
        player.sb._do(function() {
          player.sb.remove(0, Math.max(0.1, video.currentTime - keepaway));
        });
      }
    };
    if (player.onsbinit.length) {
      player.onsbinit.shift()();
    }
    checkReady();
  };
  this.wsconnect = function() {
    return new Promise(function(resolve, reject) {
      this.ws = new WebSocket(MistVideo2.source.url);
      this.ws.binaryType = "arraybuffer";
      this.ws.s = this.ws.send;
      this.ws.send = function() {
        if (this.readyState == 1) {
          this.s.apply(this, arguments);
          return true;
        }
        return false;
      };
      this.ws.onopen = function() {
        this.wasConnected = true;
        resolve();
      };
      this.ws.onerror = function(e) {
        MistVideo2.showError("MP4 over WS: websocket error");
      };
      this.ws.onclose = function(e) {
        MistVideo2.log("MP4 over WS: websocket closed");
        if (this.wasConnected && !MistVideo2.destroyed && (!player.sb || !player.sb.paused) && MistVideo2.state == "Stream is online" && !(MistVideo2.video && MistVideo2.video.error)) {
          MistVideo2.log("MP4 over WS: reopening websocket");
          player.wsconnect().then(function() {
            if (!player.sb) {
              var f = function(msg) {
                if (!player.sb) {
                  player.sbinit(msg.data.codecs);
                } else {
                  player.api.play().catch(function() {
                  });
                }
                player.ws.removeListener("codec_data", f);
              };
              player.ws.addListener("codec_data", f);
              send({ type: "request_codec_data", supported_codecs: MistVideo2.source.supportedCodecs });
            } else {
              player.api.play();
            }
          }, function() {
            Mistvideo.error("Lost connection to the Media Server");
          });
        }
      };
      this.ws.timeOut = MistVideo2.timers.start(function() {
        if (player.ws.readyState == 0) {
          MistVideo2.log("MP4 over WS: socket timeout - try next combo");
          MistVideo2.nextCombo();
        }
      }, 5e3);
      this.ws.listeners = {};
      this.ws.addListener = function(type, f) {
        if (!(type in this.listeners)) {
          this.listeners[type] = [];
        }
        this.listeners[type].push(f);
      };
      this.ws.removeListener = function(type, f) {
        if (!(type in this.listeners)) {
          return;
        }
        var i3 = this.listeners[type].indexOf(f);
        if (i3 < 0) {
          return;
        }
        this.listeners[type].splice(i3, 1);
        return true;
      };
      player.msgqueue = false;
      var requested_rate = 1;
      var serverdelay = [];
      var currenttracks = [];
      var browser = MistUtil.getBrowser();
      this.ws.onmessage = function(e) {
        if (!e.data) {
          throw "Received invalid data";
        }
        if (typeof e.data == "string") {
          var msg = JSON.parse(e.data);
          if (player.debugging && msg.type != "on_time") {
            console.log("ws message", msg);
          }
          switch (msg.type) {
            case "on_stop": {
              var eObj;
              eObj = MistUtil.event.addListener(video, "waiting", function(e2) {
                player.sb.paused = true;
                MistUtil.event.send("ended", null, video);
                MistUtil.event.removeListener(eObj);
              });
              player.ws.onclose = function() {
              };
              break;
            }
            case "on_time": {
              var buffer = msg.data.current - video.currentTime * 1e3;
              var serverDelay = player.ws.serverDelay.get();
              var desiredBuffer = browser == "chrome" ? Math.max(1e3 + serverDelay, serverDelay * 2) : Math.max(100 + serverDelay, serverDelay * 2);
              var desiredBufferwithJitter = desiredBuffer + (msg.data.jitter ? msg.data.jitter : 0);
              if (MistVideo2.info.type != "live") {
                desiredBuffer += 2e3;
              }
              if (player.debugging) {
                console.log(
                  "on_time received",
                  msg.data.current / 1e3,
                  "currtime",
                  video.currentTime,
                  requested_rate + "x",
                  "buffer",
                  Math.round(buffer),
                  "/",
                  Math.round(desiredBufferwithJitter),
                  "htmlbuffer",
                  Math.round((video.buffered.end(0) - video.currentTime) * 1e3),
                  "jitter",
                  msg.data.jitter,
                  MistVideo2.info.type == "live" ? "latency:" + Math.round(msg.data.end - video.currentTime * 1e3) + "ms" : "",
                  player.monitor ? "bitrate:" + MistUtil.format.bits(player.monitor.currentBps) + "/s" : "",
                  "listeners",
                  player.ws.listeners && player.ws.listeners.on_time ? player.ws.listeners.on_time : 0,
                  "msgqueue",
                  player.msgqueue ? player.msgqueue.length : 0,
                  "readyState",
                  MistVideo2.video.readyState,
                  msg.data
                );
              }
              if (!player.sb) {
                MistVideo2.log("Received on_time, but the source buffer is being cleared right now. Ignoring.");
                break;
              }
              if (lastduration != msg.data.end * 1e-3) {
                lastduration = msg.data.end * 1e-3;
                MistUtil.event.send("durationchange", null, MistVideo2.video);
              }
              MistVideo2.info.meta.buffer_window = msg.data.end - msg.data.begin;
              player.sb.paused = false;
              if (MistVideo2.info.type == "live") {
                if (requested_rate == 1) {
                  if (msg.data.play_rate_curr == "auto") {
                    if (video.currentTime > 0) {
                      if (buffer > desiredBufferwithJitter * 2) {
                        requested_rate = 1 + Math.min(1, (buffer - desiredBufferwithJitter) / desiredBufferwithJitter) * 0.08;
                        video.playbackRate *= requested_rate;
                        MistVideo2.log("Our buffer (" + Math.round(buffer) + "ms) is big (>" + Math.round(desiredBufferwithJitter * 2) + "ms), so increase the playback speed to " + Math.round(requested_rate * 100) / 100 + " to catch up.");
                      } else if (buffer < 0) {
                        requested_rate = 0.8;
                        video.playbackRate *= requested_rate;
                        MistVideo2.log("Our buffer (" + Math.round(buffer) + "ms) is negative so decrease the playback speed to " + Math.round(requested_rate * 100) / 100 + " to let it catch up.");
                      } else if (buffer < desiredBuffer / 2) {
                        requested_rate = 1 + Math.min(1, (buffer - desiredBuffer) / desiredBuffer) * 0.08;
                        video.playbackRate *= requested_rate;
                        MistVideo2.log("Our buffer (" + Math.round(buffer) + "ms) is small (<" + Math.round(desiredBuffer / 2) + "ms), so decrease the playback speed to " + Math.round(requested_rate * 100) / 100 + " to catch up.");
                      }
                    }
                  }
                } else if (requested_rate > 1) {
                  if (buffer < desiredBufferwithJitter) {
                    video.playbackRate /= requested_rate;
                    requested_rate = 1;
                    MistVideo2.log("Our buffer (" + Math.round(buffer) + "ms) is small enough (<" + Math.round(desiredBufferwithJitter) + "ms), so return to real time playback.");
                  }
                } else {
                  if (buffer > desiredBufferwithJitter) {
                    video.playbackRate /= requested_rate;
                    requested_rate = 1;
                    MistVideo2.log("Our buffer (" + Math.round(buffer) + "ms) is big enough (>" + Math.round(desiredBufferwithJitter) + "ms), so return to real time playback.");
                  }
                }
              } else {
                if (requested_rate == 1) {
                  if (msg.data.play_rate_curr == "auto") {
                    if (buffer < desiredBuffer / 2) {
                      if (buffer < -1e4) {
                        send({ type: "seek", seek_time: Math.round(video.currentTime * 1e3) });
                      } else {
                        requested_rate = 2;
                        MistVideo2.log("Our buffer is negative, so request a faster download rate.");
                        send({ type: "set_speed", play_rate: requested_rate });
                      }
                    } else if (buffer - desiredBuffer > desiredBuffer) {
                      MistVideo2.log("Our buffer is big, so request a slower download rate.");
                      requested_rate = 0.5;
                      send({ type: "set_speed", play_rate: requested_rate });
                    }
                  }
                } else if (requested_rate > 1) {
                  if (buffer > desiredBuffer) {
                    send({ type: "set_speed", play_rate: "auto" });
                    requested_rate = 1;
                    MistVideo2.log("The buffer is big enough, so ask for realtime download rate.");
                  }
                } else {
                  if (buffer < desiredBuffer) {
                    send({ type: "set_speed", play_rate: "auto" });
                    requested_rate = 1;
                    MistVideo2.log("The buffer is small enough, so ask for realtime download rate.");
                  }
                }
              }
              if (MistVideo2.reporting && msg.data.tracks) {
                MistVideo2.reporting.stats.d.tracks = msg.data.tracks.join(",");
              }
              if (msg.data.tracks && currenttracks != msg.data.tracks) {
                var tracks = MistVideo2.info ? MistUtil.tracks.parse(MistVideo2.info.meta.tracks) : [];
                for (var i3 in msg.data.tracks) {
                  if (currenttracks.indexOf(msg.data.tracks[i3]) < 0) {
                    var type;
                    for (var j in tracks) {
                      if (msg.data.tracks[i3] in tracks[j]) {
                        type = j;
                        break;
                      }
                    }
                    if (!type) {
                      continue;
                    }
                    MistUtil.event.send("playerUpdate_trackChanged", {
                      type,
                      trackid: msg.data.tracks[i3]
                    }, MistVideo2.video);
                  }
                }
                currenttracks = msg.data.tracks;
              }
              break;
            }
            case "tracks": {
              let checkEqual = function(arr1, arr2) {
                if (!arr2) {
                  return false;
                }
                if (arr1.length != arr2.length) {
                  return false;
                }
                for (var i4 in arr1) {
                  if (arr2.indexOf(arr1[i4]) < 0) {
                    return false;
                  }
                }
                return true;
              }, setSeekingPosition = function(t) {
                var currPos = video.currentTime.toFixed(3);
                if (currPos > t) {
                  t = currPos;
                }
                if (!video.buffered.length || video.buffered.end(video.buffered.length - 1) < t) {
                  if (player.debugging) {
                    console.log("Desired seeking position (" + MistUtil.format.time(t, { ms: true }) + ") not yet in buffer (" + (video.buffered.length ? MistUtil.format.time(video.buffered.end(video.buffered.length - 1), { ms: true }) : "null") + ")");
                  }
                  player.sb._doNext(function() {
                    setSeekingPosition(t);
                  });
                  return;
                }
                video.currentTime = t;
                MistVideo2.log("Setting playback position to " + MistUtil.format.time(t, { ms: true }));
                if (video.currentTime.toFixed(3) < t) {
                  player.sb._doNext(function() {
                    setSeekingPosition(t);
                  });
                  if (player.debugging) {
                    console.log("Could not set playback position");
                  }
                } else {
                  if (player.debugging) {
                    console.log("Set playback position to " + MistUtil.format.time(t, { ms: true }));
                  }
                  var p11 = function() {
                    player.sb._doNext(function() {
                      if (video.buffered.length) {
                        if (video.buffered.start(0) > video.currentTime) {
                          var b = video.buffered.start(0);
                          video.currentTime = b;
                          if (video.currentTime != b) {
                            p11();
                          }
                        }
                      } else {
                        p11();
                      }
                    });
                  };
                  p11();
                }
              };
              if (checkEqual(player.last_codecs ? player.last_codecs : player.sb._codecs, msg.data.codecs)) {
                MistVideo2.log("Player switched tracks, keeping source buffer as codecs are the same as before.");
                if (video.currentTime == 0 && msg.data.current != 0) {
                  setSeekingPosition((msg.data.current * 1e-3).toFixed(3));
                }
              } else {
                let reachedSwitchingPoint = function(msg2) {
                  if (player.debugging) {
                    console.warn("reached switching point", msg2.data.current * 1e-3, MistUtil.format.time(msg2.data.current * 1e-3));
                  }
                  MistVideo2.log("Track switch: reached switching point");
                  clear();
                };
                if (player.debugging) {
                  console.warn("Different codecs!");
                  console.warn("video time", video.currentTime, "switch startpoint", msg.data.current * 1e-3);
                }
                player.last_codecs = msg.data.codecs;
                if (player.msgqueue) {
                  player.msgqueue.push([]);
                } else {
                  player.msgqueue = [[]];
                }
                var clear = function() {
                  if (player && player.sb) {
                    player.sb._do(function(remaining_do_on_updateend) {
                      if (!player.sb.updating) {
                        if (!isNaN(player.ms.duration)) player.sb.remove(0, Infinity);
                        player.sb.queue = [];
                        player.ms.removeSourceBuffer(player.sb);
                        player.sb = null;
                        video.src = "";
                        player.ms.onsourceclose = null;
                        player.ms.onsourceended = null;
                        if (player.debugging && remaining_do_on_updateend && remaining_do_on_updateend.length) {
                          console.warn("There are do_on_updateend functions queued, which I will re-apply after clearing the sb.");
                        }
                        player.msinit().then(function() {
                          player.sbinit(msg.data.codecs);
                          if (!player.sb) MistVideo2.log("Failed to reinitialize source buffer", "error");
                          return;
                          player.sb.do_on_updateend = remaining_do_on_updateend;
                          var e2 = MistUtil.event.addListener(video, "loadedmetadata", function() {
                            MistVideo2.log("Buffer cleared");
                            setSeekingPosition((msg.data.current * 1e-3).toFixed(3));
                            MistUtil.event.removeListener(e2);
                          });
                        });
                      } else {
                        clear();
                      }
                    });
                  } else {
                    if (player.debugging) {
                      console.warn("sb not available to do clear");
                    }
                    player.onsbinit.push(clear);
                  }
                };
                if (!msg.data.codecs || !msg.data.codecs.length) {
                  MistVideo2.showError("Track switch does not contain any codecs, aborting.");
                  MistVideo2.options.setTracks = false;
                  clear();
                  break;
                }
                if (video.currentTime == 0) {
                  reachedSwitchingPoint(msg);
                } else {
                  if (msg.data.current >= video.currentTime * 1e3) {
                    MistVideo2.log("Track switch: waiting for playback to reach the switching point (" + MistUtil.format.time(msg.data.current * 1e-3, { ms: true }) + ")");
                    var ontime = MistUtil.event.addListener(video, "timeupdate", function() {
                      if (msg.data.current < video.currentTime * 1e3) {
                        if (player.debugging) {
                          console.log("Track switch: video.currentTime has reached switching point.");
                        }
                        reachedSwitchingPoint(msg);
                        MistUtil.event.removeListener(ontime);
                        MistUtil.event.removeListener(onwait);
                      }
                    });
                    var onwait = MistUtil.event.addListener(video, "waiting", function() {
                      if (player.debugging) {
                        console.log("Track switch: video has reached end of buffer.", "Gap:", Math.round(msg.data.current - video.currentTime * 1e3), "ms");
                      }
                      reachedSwitchingPoint(msg);
                      MistUtil.event.removeListener(ontime);
                      MistUtil.event.removeListener(onwait);
                    });
                  } else {
                    MistVideo2.log("Track switch: waiting for the received data to reach the current playback point");
                    var ontime = function(newmsg) {
                      if (newmsg.data.current >= video.currentTime * 1e3) {
                        reachedSwitchingPoint(newmsg);
                        player.ws.removeListener("on_time", ontime);
                      }
                    };
                    player.ws.addListener("on_time", ontime);
                  }
                }
              }
              break;
            }
            case "pause": {
              if (player.sb) {
                player.sb.paused = true;
              }
              break;
            }
          }
          if (msg.type in this.listeners) {
            for (var i3 = this.listeners[msg.type].length - 1; i3 >= 0; i3--) {
              this.listeners[msg.type][i3](msg);
            }
          }
          return;
        }
        var data = new Uint8Array(e.data);
        if (data) {
          if (player.monitor && player.monitor.bitCounter) {
            for (var i3 in player.monitor.bitCounter) {
              player.monitor.bitCounter[i3] += e.data.byteLength * 8;
            }
          }
          if (player.sb && !player.msgqueue) {
            if (player.sb.updating || player.sb.queue.length || player.sb._busy) {
              player.sb.queue.push(data);
            } else {
              player.sb._append(data);
            }
          } else {
            if (!player.msgqueue) {
              player.msgqueue = [[]];
            }
            player.msgqueue[player.msgqueue.length - 1].push(data);
          }
        } else {
          MistVideo2.log("Expecting data from websocket, but received none?!");
        }
      };
      this.ws.serverDelay = {
        delays: [],
        log: function(type) {
          var responseType = false;
          switch (type) {
            case "seek":
            case "set_speed": {
              responseType = type;
              break;
            }
            case "request_codec_data": {
              responseType = "codec_data";
              break;
            }
            default: {
              return;
            }
          }
          if (responseType) {
            let onResponse = function() {
              if (!player.ws || !player.ws.serverDelay) {
                return;
              }
              player.ws.serverDelay.add((/* @__PURE__ */ new Date()).getTime() - starttime);
              player.ws.removeListener(responseType, onResponse);
            };
            var starttime = (/* @__PURE__ */ new Date()).getTime();
            player.ws.addListener(responseType, onResponse);
          }
        },
        add: function(delay) {
          this.delays.unshift(delay);
          if (this.delays.length > 5) {
            this.delays.splice(5);
          }
        },
        get: function() {
          if (this.delays.length) {
            let sum = 0;
            let i3 = 0;
            for (null; i3 < this.delays.length; i3++) {
              if (i3 >= 3) {
                break;
              }
              sum += this.delays[i3];
            }
            return sum / i3;
          }
          return 500;
        }
      };
    }.bind(this));
  };
  this.wsconnect().then(function() {
    var f = function(msg) {
      if (player.ms && player.ms.readyState == "open") {
        player.sbinit(msg.data.codecs);
      } else {
        player.msoninit.push(function() {
          player.sbinit(msg.data.codecs);
        });
      }
      player.ws.removeListener("codec_data", f);
    };
    this.ws.addListener("codec_data", f);
    send({ type: "request_codec_data", supported_codecs: MistVideo2.source.supportedCodecs });
  }.bind(this));
  function send(cmd, retry) {
    if (!player.ws) {
      throw "No websocket to send to";
    }
    if (retry > 5) {
      throw "Too many retries, giving up";
    }
    if (player.ws.readyState < player.ws.OPEN) {
      MistVideo2.timers.start(function() {
        send(cmd, ++retry);
      }, 500);
      return;
    }
    if (player.ws.readyState >= player.ws.CLOSING) {
      if (MistVideo2.destroyed) {
        return;
      }
      MistVideo2.log("MP4 over WS: reopening websocket");
      player.wsconnect().then(function() {
        if (!player.sb) {
          var f = function(msg) {
            if (!player.sb) {
              player.sbinit(msg.data.codecs);
            } else {
              player.api.play().catch(function() {
              });
            }
            player.ws.removeListener("codec_data", f);
          };
          player.ws.addListener("codec_data", f);
          send({ type: "request_codec_data", supported_codecs: MistVideo2.source.supportedCodecs });
        } else {
          player.api.play();
        }
        send(cmd);
      }, function() {
        Mistvideo.error("Lost connection to the Media Server");
      });
      return;
    }
    if (player.debugging) {
      console.log("ws send", cmd);
    }
    player.ws.serverDelay.log(cmd.type);
    if (!player.ws.send(JSON.stringify(cmd))) {
      return send(cmd, ++retry);
    }
  }
  player.findBuffer = function(position) {
    var buffern = false;
    for (var i3 = 0; i3 < video.buffered.length; i3++) {
      if (video.buffered.start(i3) <= position && video.buffered.end(i3) >= position) {
        buffern = i3;
        break;
      }
    }
    return buffern;
  };
  this.api = {
    play: function(skipToLive) {
      return new Promise(function(resolve, reject) {
        if (!video.paused) {
          resolve();
          return;
        }
        if ("paused" in player.sb && !player.sb.paused) {
          video.play().then(resolve).catch(reject);
          return;
        }
        var f = function(e) {
          if (!player.sb) {
            MistVideo2.log("Attempting to play, but the source buffer is being cleared. Waiting for next on_time.");
            return;
          }
          if (MistVideo2.info.type == "live") {
            if (skipToLive || video.currentTime == 0) {
              var g = function() {
                if (video.buffered.length) {
                  var buffern = player.findBuffer(e.data.current * 1e-3);
                  if (buffern !== false) {
                    if (video.buffered.start(buffern) > video.currentTime || video.buffered.end(buffern) < video.currentTime) {
                      video.currentTime = e.data.current * 1e-3;
                      MistVideo2.log("Setting live playback position to " + MistUtil.format.time(video.currentTime));
                    }
                    video.play().then(resolve).catch(function() {
                      return reject.apply(this, arguments);
                    });
                    player.sb.paused = false;
                    player.sb.removeEventListener("updateend", g);
                  }
                }
              };
              player.sb.addEventListener("updateend", g);
            } else {
              player.sb.paused = false;
              video.play().then(resolve).catch(function() {
                player.api.pause();
                return reject.apply(this, arguments);
              });
            }
            player.ws.removeListener("on_time", f);
          } else if (e.data.current > video.currentTime) {
            player.sb.paused = false;
            if (video.buffered.length && video.buffered.start(0) > video.currentTime) {
              video.currentTime = video.buffered.start(0);
            }
            video.play().then(resolve).catch(reject);
            player.ws.removeListener("on_time", f);
          }
        };
        player.ws.addListener("on_time", f);
        var cmd = { type: "play" };
        if (skipToLive) {
          cmd.seek_time = "live";
        }
        send(cmd);
      });
    },
    pause: function() {
      video.pause();
      send({ type: "hold" });
      if (player.sb) {
        player.sb.paused = true;
      }
    },
    setTracks: function(obj) {
      if (!MistUtil.object.keys(obj).length) {
        return;
      }
      obj.type = "tracks";
      obj = MistUtil.object.extend({
        type: "tracks"
        //seek_time:  Math.round(Math.max(0,video.currentTime*1e3-(500+player.ws.serverDelay.get())))
      }, obj);
      send(obj);
    },
    unload: function() {
      player.api.pause();
      if (player.sb) {
        player.sb._do(function() {
          player.sb.remove(0, Infinity);
          try {
            player.ms.endOfStream();
          } catch (e) {
          }
        });
      }
      player.ws.close();
    },
    setSubtitle: function(trackmeta) {
      var tracks = video.getElementsByTagName("track");
      for (var i3 = tracks.length - 1; i3 >= 0; i3--) {
        video.removeChild(tracks[i3]);
      }
      if (trackmeta) {
        var track = document.createElement("track");
        video.appendChild(track);
        track.kind = "subtitles";
        track.label = trackmeta.label;
        track.srclang = trackmeta.lang;
        track.src = trackmeta.src;
        track.setAttribute("default", "");
      }
    }
  };
  Object.defineProperty(this.api, "currentTime", {
    get: function() {
      return video.currentTime;
    },
    set: function(value) {
      if (isNaN(value) || value < 0) {
        MistVideo2.log("Ignoring seek to " + value + " because ewww.");
        return;
      }
      MistUtil.event.send("seeking", value, video);
      send({ type: "seek", seek_time: Math.round(Math.max(0, value * 1e3 - (250 + player.ws.serverDelay.get()))) });
      var onseek = function(e) {
        player.ws.removeListener("seek", onseek);
        var ontime = function(e2) {
          player.ws.removeListener("on_time", ontime);
          value = e2.data.current * 1e-3;
          value = value.toFixed(3);
          var retries = 10;
          var f = function() {
            video.currentTime = value;
            if (video.currentTime.toFixed(3) < value) {
              MistVideo2.log("Failed to seek, wanted: " + value + " got: " + video.currentTime.toFixed(3));
              if (retries >= 0) {
                retries--;
                player.sb._doNext(f);
              }
            }
          };
          f();
        };
        player.ws.addListener("on_time", ontime);
      };
      player.ws.addListener("seek", onseek);
      video.currentTime = value;
      MistVideo2.log("Seeking to " + MistUtil.format.time(value, { ms: true }) + " (" + value + ")");
    }
  });
  var lastduration = Infinity;
  Object.defineProperty(this.api, "duration", {
    get: function() {
      return lastduration;
    }
  });
  Object.defineProperty(this.api, "playbackRate", {
    get: function() {
      return video.playbackRate;
    },
    set: function(value) {
      var f = function(msg) {
        video.playbackRate = msg.data.play_rate_curr == "auto" ? 1 : msg.data.play_rate_curr;
      };
      player.ws.addListener("set_speed", f);
      send({ type: "set_speed", play_rate: value == 1 ? "auto" : value });
    }
  });
  function reroute(item) {
    Object.defineProperty(player.api, item, {
      get: function() {
        return video[item];
      },
      set: function(value) {
        return video[item] = value;
      }
    });
  }
  var list = [
    "volume",
    "buffered",
    "muted",
    "loop",
    "paused",
    ,
    "error",
    "textTracks",
    "webkitDroppedFrameCount",
    "webkitDecodedFrameCount"
  ];
  for (var i2 in list) {
    reroute(list[i2]);
  }
  MistUtil.event.addListener(video, "ended", function() {
    if (player.api.loop) {
      player.api.currentTime = 0;
      player.sb._do(function() {
        try {
          player.sb.remove(0, Infinity);
        } catch (e) {
        }
      });
    }
  });
  var seeking = false;
  MistUtil.event.addListener(video, "seeking", function() {
    seeking = true;
    var seeked = MistUtil.event.addListener(video, "seeked", function() {
      seeking = false;
      MistUtil.event.removeListener(seeked);
    });
  });
  MistUtil.event.addListener(video, "waiting", function() {
    if (seeking) {
      return;
    }
    var buffern = player.findBuffer(video.currentTime);
    if (buffern !== false) {
      if (buffern + 1 < video.buffered.length && video.buffered.start(buffern + 1) - video.currentTime < 1e4) {
        MistVideo2.log("Skipped over buffer gap (from " + MistUtil.format.time(video.currentTime) + " to " + MistUtil.format.time(video.buffered.start(buffern + 1)) + ")");
        video.currentTime = video.buffered.start(buffern + 1);
      }
    }
  });
  MistUtil.event.addListener(video, "pause", function() {
    if (player.sb && !player.sb.paused) {
      MistVideo2.log("The browser paused the vid - probably because it has no audio and the tab is no longer visible. Pausing download.");
      send({ type: "hold" });
      player.sb.paused = true;
      var p11 = MistUtil.event.addListener(video, "play", function() {
        if (player.sb && player.sb.paused) {
          send({ type: "play" });
        }
        MistUtil.event.removeListener(p11);
      });
    }
  });
  if (player.debugging) {
    MistUtil.event.addListener(video, "waiting", function() {
      var buffers = [];
      var contained = false;
      for (var i3 = 0; i3 < video.buffered.length; i3++) {
        if (video.currentTime >= video.buffered.start(i3) && video.currentTime <= video.buffered.end(i3)) {
          contained = true;
        }
        buffers.push([
          video.buffered.start(i3),
          video.buffered.end(i3)
        ]);
      }
      console.log("waiting", "currentTime", video.currentTime, "buffers", buffers, contained ? "contained" : "outside of buffer", "readystate", video.readyState, "networkstate", video.networkState);
      if (video.readyState >= 2 && video.networkState >= 2) {
        console.error("Why am I waiting?!", video.currentTime);
      }
    });
  }
  this.ABR = {
    size: null,
    bitrate: null,
    generateString: function(type, raw) {
      switch (type) {
        case "size": {
          return "~" + [raw.width, raw.height].join("x");
        }
        case "bitrate": {
          return "<" + Math.round(raw) + "bps,minbps";
        }
        default: {
          throw "Unknown ABR type";
        }
      }
    },
    request: function(type, value) {
      this[type] = value;
      var request = [];
      if (this.bitrate !== null) {
        request.push(this.generateString("bitrate", this.bitrate));
      }
      if (this.size !== null) {
        request.push(this.generateString("size", this.size));
      } else {
        request.push("maxbps");
      }
      return player.api.setTracks({
        video: request.join(",|")
      });
    }
  };
  this.api.ABR_resize = function(size) {
    MistVideo2.log("Requesting the video track with the resolution that best matches the player size");
    player.ABR.request("size", size);
  };
  this.monitor = {
    bitCounter: [],
    bitsSince: [],
    currentBps: null,
    nWaiting: 0,
    nWaitingThreshold: 3,
    listener: MistVideo2.options.ABR_bitrate ? MistUtil.event.addListener(video, "waiting", function() {
      player.monitor.nWaiting++;
      if (player.monitor.nWaiting >= player.monitor.nWaitingThreshold) {
        player.monitor.nWaiting = 0;
        player.monitor.action();
      }
    }) : null,
    getBitRate: function() {
      if (player.sb && !player.sb.paused) {
        this.bitCounter.push(0);
        this.bitsSince.push((/* @__PURE__ */ new Date()).getTime());
        var bits, since;
        if (this.bitCounter.length > 5) {
          bits = player.monitor.bitCounter.shift();
          since = this.bitsSince.shift();
        } else {
          bits = player.monitor.bitCounter[0];
          since = this.bitsSince[0];
        }
        var dt = (/* @__PURE__ */ new Date()).getTime() - since;
        this.currentBps = bits / (dt * 1e-3);
      }
      MistVideo2.timers.start(function() {
        player.monitor.getBitRate();
      }, 500);
    },
    action: function() {
      if (MistVideo2.options.setTracks && MistVideo2.options.setTracks.video) {
        return;
      }
      MistVideo2.log("ABR threshold triggered, requesting lower quality");
      player.ABR.request("bitrate", this.currentBps);
    }
  };
  this.monitor.getBitRate();
};

// src/wrappers/rawwscanvas.js
registerWrapper("rawwscanvas", {
  name: "RAW to Canvas",
  mimes: ["ws/video/raw"],
  isMimeSupported: function(mimetype) {
    return MistUtil.array.indexOf(this.mimes, mimetype) == -1 ? false : true;
  },
  isBrowserSupported: function(mimetype, source, MistVideo2) {
    if (!("WebSocket" in window) || MistVideo2.info.capa && !MistVideo2.info.capa.ssl) {
      MistVideo2.log("This player requires websocket support");
      return false;
    }
    if (location.protocol != MistUtil.http.url.split(source.url.replace(/^ws/, "http")).protocol) {
      if (location.protocol == "file:" && MistUtil.http.url.split(source.url.replace(/^ws/, "http")).protocol == "http:") {
        MistVideo2.log("This page was loaded over file://, the player might not behave as intended.");
      } else {
        MistVideo2.log("HTTP/HTTPS mismatch for this source");
        return false;
      }
    }
    for (var i2 in MistVideo2.info.meta.tracks) {
      if (MistVideo2.info.meta.tracks[i2].codec == "HEVC") {
        return ["video"];
      }
    }
    return false;
  },
  player: function() {
    this.onreadylist = [];
  },
  scriptsrc: function(host) {
    return host + "/libde265.js";
  }
});
var p8 = mistplayers.rawwscanvas.player;
p8.prototype = new MistPlayer();
p8.prototype.build = function(MistVideo2, callback) {
  var player = this;
  player.onDecoderLoad = function() {
    if (MistVideo2.destroyed) {
      return;
    }
    MistVideo2.log("Building rawws player..");
    var api = {};
    MistVideo2.player.api = api;
    var ele = document.createElement("canvas");
    var ctx = ele.getContext("2d");
    ele.style.objectFit = "contain";
    player.vars = {};
    if (MistVideo2.options.autoplay) {
      player.vars.wantToPlay = true;
    }
    player.dropping = false;
    player.frames = {
      //contains helper functions and statistics
      received: 0,
      bitsReceived: 0,
      decoded: 0,
      dropped: 0,
      behind: function() {
        return this.received - this.decoded - this.dropped;
      },
      timestamps: {},
      frame2time: function(frame, clean) {
        if (frame in this.timestamps) {
          if (clean) {
            for (var i3 in this.timestamps) {
              if (i3 == frame) {
                break;
              }
              delete this.timestamps[i3];
            }
          }
          return this.timestamps[frame] * 1e-3;
        }
        return 0;
      },
      history: {
        log: [],
        add: function() {
          this.log.unshift({
            time: (/* @__PURE__ */ new Date()).getTime(),
            received: player.frames.received,
            bitsReceived: player.frames.bitsReceived,
            decoded: player.frames.decoded
          });
          if (this.log.length > 3) {
            this.log.splice(3);
          }
        }
      },
      framerate_in: function() {
        var l = this.history.log.length - 1;
        if (l < 1) {
          return 0;
        }
        var dframe = this.history.log[0].received - this.history.log[l].received;
        var dt = (this.history.log[0].time - this.history.log[l].time) * 1e-3;
        return dframe / dt;
      },
      bitrate_in: function() {
        var l = this.history.log.length - 1;
        if (l < 1) {
          return 0;
        }
        var dbits = this.history.log[0].bitsReceived - this.history.log[l].bitsReceived;
        var dt = (this.history.log[0].time - this.history.log[l].time) * 1e-3;
        return dbits / dt;
      },
      framerate_out: function() {
        var l = this.history.log.length - 1;
        if (l < 1) {
          return 0;
        }
        var dframe = this.history.log[0].decoded - this.history.log[l].decoded;
        var dt = (this.history.log[0].time - this.history.log[l].time) * 1e-3;
        return dframe / dt;
      },
      framerate: function() {
        if ("rate_theoretical" in this) {
          return this.rate_theoretical;
        }
        return this.framerate_in();
      },
      keepingUp: function() {
        var l = this.history.log.length - 1;
        if (l < 1) {
          return 0;
        }
        var dBehind = this.history.log[l].received - this.history.log[l].decoded - (this.history.log[0].received - this.history.log[0].decoded);
        var dt = (this.history.log[0].time - this.history.log[l].time) * 1e-3;
        var keepingUp_frames = dBehind / dt;
        return keepingUp_frames / this.framerate();
      }
    };
    api.framerate_in = function() {
      return player.frames.framerate_in();
    };
    api.framerate_out = function() {
      return player.frames.framerate_out();
    };
    api.currentBps = function() {
      return player.frames.bitrate_in();
    };
    api.loop = MistVideo2.options.loop;
    Object.defineProperty(MistVideo2.player.api, "webkitDecodedFrameCount", {
      get: function() {
        return player.frames.decoded;
      }
    });
    Object.defineProperty(MistVideo2.player.api, "webkitDroppedFrameCount", {
      get: function() {
        return player.frames.dropped;
      }
    });
    var decoder;
    this.decoder = null;
    function emitEvent(type) {
      MistUtil.event.send(type, void 0, ele);
    }
    function init() {
      function init_decoder() {
        decoder = new libde265.Decoder();
        MistVideo2.player.decoder = decoder;
        var onDecode = [];
        decoder.addListener = function(func) {
          onDecode.push(func);
        };
        decoder.removeListener = function(func) {
          var i3 = onDecode.indexOf(func);
          if (i3 < 0) {
            return;
          }
          onDecode.splice(i3, 1);
          return true;
        };
        var displayImage;
        if (window.requestAnimationFrame) {
          displayImage = function(display_image_data) {
            decoder.pending_image_data = display_image_data;
            window.requestAnimationFrame(function() {
              if (decoder.pending_image_data) {
                ctx.putImageData(decoder.pending_image_data, 0, 0);
                decoder.pending_image_data = null;
              }
            });
          };
        } else {
          displayImage = function(display_image_data) {
            ctx.putImageData(display_image_data, 0, 0);
          };
        }
        decoder.set_image_callback(function(image) {
          player.frames.decoded++;
          if (player.vars.wantToPlay && player.state != "seeking") {
            emitEvent("timeupdate");
          }
          if (!decoder.image_data) {
            var w = image.get_width();
            var h = image.get_height();
            if (w != ele.width || h != ele.height || !this.image_data) {
              ele.width = w;
              ele.height = h;
              var img = ctx.createImageData(w, h);
              decoder.image_data = img;
            }
          }
          if (player.state != "seeking") {
            image.display(this.image_data, function(display_image_data) {
              decoder.decoding = false;
              displayImage(display_image_data);
            });
          }
          image.free();
          switch (player.state) {
            case "play":
            case "waiting": {
              if (!player.dropping) {
                emitEvent("canplay");
                emitEvent("playing");
                player.state = "playing";
                if (!player.vars.wantToPlay) {
                  MistVideo2.player.send({ type: "hold" });
                }
              }
              break;
            }
            case "seeking": {
              var t = player.frames.frame2time(player.frames.decoded + player.frames.dropped);
              if (t >= player.vars.seekTo) {
                emitEvent("seeked");
                player.vars.seekTo = null;
                player.state = "playing";
                if (!player.vars.wantToPlay) {
                  emitEvent("timeupdate");
                  MistVideo2.player.send({ type: "hold" });
                }
              }
              break;
            }
            default: {
              player.state = "playing";
            }
          }
          for (var i3 in onDecode) {
            onDecode[i3]();
          }
        });
      }
      init_decoder();
      function isKeyFrame(infoBytes) {
        return !!infoBytes[1];
      }
      function toTimestamp(infoBytes) {
        var v = new DataView(new ArrayBuffer(8));
        for (var i3 = 0; i3 < 8; i3++) {
          v.setUint8(i3, infoBytes[i3 + 2]);
        }
        return v.getInt32(4);
      }
      function connect() {
        emitEvent("loadstart");
        var url = MistUtil.http.url.addParam(MistVideo2.source.url, { buffer: 0, video: "hevc,|minbps" });
        var socket = new WebSocket(url);
        MistVideo2.player.ws = socket;
        socket.binaryType = "arraybuffer";
        function send(obj) {
          if (!MistVideo2.player.ws) {
            throw "No websocket to send to";
          }
          if (socket.readyState == 1) {
            socket.send(JSON.stringify(obj));
          }
          return false;
        }
        MistVideo2.player.send = send;
        socket.wasConnected = false;
        socket.onopen = function() {
          if (!MistVideo2.player.built) {
            MistVideo2.player.built = true;
            callback(ele);
          }
          send({ type: "request_codec_data", supported_codecs: ["HEVC"] });
          socket.wasConnected = true;
        };
        socket.onclose = function() {
          if (this.wasConnected && !MistVideo2.destroyed && MistVideo2.state == "Stream is online") {
            MistVideo2.log("Raw over WS: reopening websocket");
            connect(url);
          } else {
            MistVideo2.showError("Raw over WS: websocket closed");
          }
        };
        socket.onerror = function(e) {
          MistVideo2.showError("Raw over WS: websocket error");
        };
        socket.onmessage = function(e) {
          if (typeof e.data == "string") {
            var d = JSON.parse(e.data);
            switch (d.type) {
              case "on_time": {
                player.vars.paused = false;
                player.frames.history.add();
                if (player.vars.duration != d.data.end * 1e-3) {
                  player.vars.duration = d.data.end * 1e-3;
                  emitEvent("durationchange");
                }
                break;
              }
              case "seek": {
                MistVideo2.player.frames.timestamps = {};
                if (MistVideo2.player.dropping) {
                  MistVideo2.log("Emptying drop queue for seek");
                  MistVideo2.player.frames.dropped += MistVideo2.player.dropping.length;
                  MistVideo2.player.dropping = [];
                }
                break;
              }
              case "codec_data": {
                emitEvent("loadedmetadata");
                send({ type: "play" });
                player.state = "play";
                break;
              }
              case "info": {
                var tracks = MistVideo2.info.meta.tracks;
                var track;
                for (var i3 in tracks) {
                  if (tracks[i3].idx == d.data.tracks[0]) {
                    track = tracks[i3];
                    break;
                  }
                }
                if (typeof track != void 0 && track.fpks > 0) {
                  player.frames.rate_theoretical = track.fpks * 1e-3;
                }
                break;
              }
              case "pause": {
                player.vars.paused = d.paused;
                if (d.paused) {
                  player.decoder.flush();
                  emitEvent("pause");
                }
                break;
              }
              case "on_stop": {
                if (player.state == "ended") {
                  return;
                }
                player.state = "ended";
                player.vars.paused = true;
                socket.onclose = function() {
                };
                socket.close();
                player.decoder.flush();
                emitEvent("ended");
                break;
              }
              default: {
              }
            }
          } else {
            let prepare = function(data2, infoBytes2) {
              setTimeout(function() {
                if (player.dropping) {
                  if (player.state != "waiting") {
                    emitEvent("waiting");
                    player.state = "waiting";
                  }
                  if (isKeyFrame(infoBytes2)) {
                    if (player.dropping.length) {
                      player.frames.dropped += player.dropping.length;
                      MistVideo2.log("Dropped " + player.dropping.length + " frames");
                      player.dropping = [];
                    } else {
                      MistVideo2.log("Caught up! no longer dropping");
                      player.dropping = false;
                    }
                  } else {
                    player.dropping.push([infoBytes2, data2]);
                    if (!decoder.decoding) {
                      var d2 = player.dropping.shift();
                      MistVideo2.player.process(d2[1], d2[0]);
                    }
                    return;
                  }
                } else {
                  if (player.frames.behind() > 20) {
                    player.dropping = [];
                    MistVideo2.log("Falling behind, dropping files..");
                  }
                }
                MistVideo2.player.process(data2, infoBytes2);
              }, 0);
            };
            player.frames.received++;
            player.frames.bitsReceived += e.data.byteLength * 8;
            var l = 12;
            var infoBytes = new Uint8Array(e.data.slice(0, l));
            var data = new Uint8Array(e.data.slice(l, e.data.byteLength));
            player.frames.timestamps[player.frames.received] = toTimestamp(infoBytes);
            prepare(data, infoBytes);
          }
        };
        socket.listeners = {};
        socket.addListener = function(type, f) {
          if (!(type in this.listeners)) {
            this.listeners[type] = [];
          }
          this.listeners[type].push(f);
        };
        socket.removeListener = function(type, f) {
          if (!(type in this.listeners)) {
            return;
          }
          var i3 = this.listeners[type].indexOf(f);
          if (i3 < 0) {
            return;
          }
          this.listeners[type].splice(i3, 1);
          return true;
        };
      }
      MistVideo2.player.connect = connect;
      MistVideo2.player.process = function(data, infoBytes) {
        decoder.decoding = true;
        var err = decoder.push_data(data);
        if (player.state == "play") {
          emitEvent("loadeddata");
          player.state = "waiting";
        }
        if (player.vars.wantToPlay && player.state != "seeking") {
          emitEvent("progress");
        }
        function onerror(err2) {
          if (err2 == 0) {
            return;
          }
          if (err2 == libde265.DE265_ERROR_WAITING_FOR_INPUT_DATA) {
            player.state = "waiting";
            return;
          }
          if (!libde265.de265_isOK(err2)) {
            ele.error = "Decode error: " + libde265.de265_get_error_text(err2);
            emitEvent("error");
            return true;
          }
        }
        if (!onerror(err)) {
          decoder.decode(onerror);
        } else {
          decoder.free();
        }
      };
      connect();
    }
    init();
    function reroute(item) {
      Object.defineProperty(MistVideo2.player.api, item, {
        get: function() {
          return player.vars[item];
        },
        set: function(value) {
          return player.vars[item] = value;
        }
      });
    }
    var list = [
      "duration",
      "paused",
      "error"
    ];
    for (var i2 in list) {
      reroute(list[i2]);
    }
    api.play = function() {
      return new Promise(function(resolve, reject) {
        player.vars.wantToPlay = true;
        var f = function() {
          resolve();
          MistVideo2.player.decoder.removeListener(f);
        };
        MistVideo2.player.decoder.addListener(f);
        if (MistVideo2.player.ws.readyState > MistVideo2.player.ws.OPEN) {
          MistVideo2.player.connect();
          MistVideo2.log("Websocket was closed: reconnecting to resume playback");
          return;
        }
        if (api.paused) MistVideo2.player.send({ type: "play" });
        player.state = "play";
      });
    };
    api.pause = function() {
      player.vars.wantToPlay = false;
      MistVideo2.player.send({ type: "hold" });
    };
    MistVideo2.player.api.unload = function() {
      if (MistVideo2.player.ws) {
        MistVideo2.player.ws.onclose = function() {
        };
        MistVideo2.player.ws.close();
      }
      if (MistVideo2.player.decoder) {
        MistVideo2.player.decoder.push_data = function() {
        };
        MistVideo2.player.decoder.flush();
        MistVideo2.player.decoder.free();
      }
    };
    MistVideo2.player.setSize = function(size) {
      ele.style.width = size.width + "px";
      ele.style.height = size.height + "px";
    };
    Object.defineProperty(MistVideo2.player.api, "currentTime", {
      get: function() {
        var n = player.frames.decoded + player.frames.dropped;
        if (player.state == "seeking") {
          return player.vars.seekTo;
        }
        if (n in player.frames.timestamps) {
          return player.frames.frame2time(n);
        }
        return 0;
      },
      set: function(value) {
        emitEvent("seeking");
        player.state = "seeking";
        player.vars.seekTo = value;
        MistVideo2.player.send({ type: "seek", seek_time: value * 1e3 });
        return value;
      }
    });
    Object.defineProperty(MistVideo2.player.api, "buffered", {
      get: function() {
        return {
          start: function(i3) {
            if (this.length && i3 == 0) {
              return player.frames.frame2time(player.frames.decoded + player.frames.dropped);
            }
          },
          end: function(i3) {
            if (this.length && i3 == 0) {
              return player.frames.frame2time(player.frames.received);
            }
          },
          length: player.frames.received - player.frames.decoded > 0 ? 1 : 0
        };
      }
    });
    if (MistVideo2.info.type != "live") {
      MistUtil.event.addListener(ele, "ended", function() {
        if (player.api.loop) {
          player.api.play();
          player.api.currentTime = 0;
        }
      });
    }
  };
  if ("libde265" in window) {
    this.onDecoderLoad();
  } else {
    var scripttag = MistUtil.scripts.insert(MistVideo2.urlappend(mistplayers.rawwscanvas.scriptsrc(MistVideo2.options.host)), {
      onerror: function(e) {
        var msg = "Failed to load H265 decoder";
        if (e.message) {
          msg += ": " + e.message;
        }
        MistVideo2.showError(msg);
      },
      onload: MistVideo2.player.onDecoderLoad
    }, MistVideo2);
  }
};

// src/wrappers/rawws.js
registerWrapper("rawws", {
  name: "WebCodec Player",
  mimes: ["ws/video/raw"],
  priority: MistUtil.object.keys(mistplayers).length + 1,
  isMimeSupported: function(mimetype) {
    return MistUtil.array.indexOf(this.mimes, mimetype) == -1 ? false : true;
  },
  isBrowserSupported: function(mimetype, source, MistVideo2) {
    if (!("WebSocket" in window) || MistVideo2.info.capa && !MistVideo2.info.capa.ssl) {
      MistVideo2.log("This player requires websocket support");
      return false;
    }
    if (!window.VideoDecoder || !window.AudioDecoder) {
      return false;
    }
    if (!window.Worker) return false;
    if (location.protocol != MistUtil.http.url.split(source.url.replace(/^ws/, "http")).protocol) {
      if (location.protocol == "file:" && MistUtil.http.url.split(source.url.replace(/^ws/, "http")).protocol == "http:") {
        MistVideo2.log("This page was loaded over file://, the player might not behave as intended.");
      } else {
        MistVideo2.log("HTTP/HTTPS mismatch for this source");
        return false;
      }
    }
    var cache = {};
    if (this.cacheSupported && MistVideo2.stream in this.cacheSupported) {
      cache = this.cacheSupported[MistVideo2.stream];
    } else {
      this.cacheSupported[MistVideo2.stream] = {};
    }
    if (MistVideo2.info && MistVideo2.info.meta && MistVideo2.info.meta.tracks) {
      var playabletracks = {};
      for (var i2 in MistVideo2.info.meta.tracks) {
        var track = MistVideo2.info.meta.tracks[i2];
        switch (track.type) {
          case "audio":
          case "video": {
            if (i2 in cache) {
              if (cache[i2]) playabletracks[track.type] = true;
            } else {
              playabletracks[track.type] = "maybe";
            }
            break;
          }
          case "meta": {
            if (track.codec == "subtitle") playabletracks.subtitle = true;
            break;
          }
        }
      }
      this.cacheSupported[MistVideo2.stream].last = playabletracks;
      return Object.keys(playabletracks);
    }
    return [];
  },
  player: function() {
    this.onreadylist = [];
  },
  scriptsrc: function(host) {
    return host + "/webcodecsworker.js";
  },
  cacheSupported: {
    //<streamname>: { last: <playabletracks>, trackId: true/false }
  },
  isTrackSupported: function(track) {
    function str2bin(str) {
      var out = new Uint8Array(str.length);
      for (i = 0; i < str.length; i++) {
        out[i] = str.charCodeAt(i);
      }
      return out;
    }
    var decoder;
    var config = {
      codec: track.codecstring ? track.codecstring : ("" + track.codec).toLowerCase()
    };
    if (track.init != "") {
      config.description = str2bin(track.init);
    }
    switch (track.type) {
      case "video": {
        if (track.codec == "JPEG") {
          if (!window.ImageDecoder) {
            return new Promise(function(resolve, reject) {
              resolve({
                supported: false,
                config: { codec: "image/jpeg" }
              });
            });
          }
          decoder = ImageDecoder;
          decoder.isConfigSupported = function() {
            return new Promise(function(resolve, reject) {
              decoder.isTypeSupported("image/jpeg").then(function(isSupported) {
                resolve({
                  supported: isSupported,
                  config: { codec: "image/jpeg" }
                });
              }).catch(reject);
            });
          };
        } else decoder = VideoDecoder;
        break;
      }
      case "audio": {
        decoder = AudioDecoder;
        config.numberOfChannels = track.channels;
        config.sampleRate = track.rate;
        break;
      }
      default: {
        return false;
      }
    }
    return new Promise(function(resolve, reject) {
      decoder.isConfigSupported(config).then(function(result) {
        resolve(result);
      }).catch(reject);
    });
  }
});
var p9 = mistplayers.rawws.player;
p9.prototype = new MistPlayer();
p9.prototype.build = function(MistVideo2, callback) {
  var main = this;
  var video = document.createElement("video");
  this.setSize = function(size) {
    video.style.width = size.width + "px";
    video.style.height = size.height + "px";
  };
  var debugging;
  debugging = false;
  Object.defineProperty(this, "debugging", {
    get: function() {
      return debugging;
    },
    set: function(v) {
      if (v !== debugging) {
        debugging = v;
        if (this.controller && this.controller.worker) {
          this.controller.worker.post({
            type: "debugging",
            value: v
          });
        }
      }
      return debugging;
    }
  });
  this.debugging = debugging;
  if (!window.MediaStreamTrackGenerator) {
    var MediaStreamTrackGenerator = class MediaStreamTrackGenerator {
      constructor({ kind }) {
        if (kind == "video") {
          const canvas = document.createElement("canvas");
          const ctx = canvas.getContext("2d", { desynchronized: true });
          const track = canvas.captureStream().getVideoTracks()[0];
          track.writable = new WritableStream({
            write(frame) {
              canvas.width = frame.displayWidth;
              canvas.height = frame.displayHeight;
              ctx.drawImage(frame, 0, 0, canvas.width, canvas.height);
              frame.close();
            }
          });
          return track;
        } else if (kind == "audio") {
          let makeMono = function(audioData) {
            const array_all = new Float32Array(audioData.numberOfFrames * audioData.numberOfChannels);
            const array_mono = new Float32Array(audioData.numberOfFrames);
            audioData.copyTo(array_all, { planeIndex: 0 });
            for (var i2 in array_mono) {
              array_mono[i2] = array_all[i2 * audioData.numberOfChannels];
            }
            return array_mono;
          }, keepChannels = function(audioData) {
            const array_all = new Float32Array(audioData.numberOfFrames * audioData.numberOfChannels);
            audioData.copyTo(array_all, { planeIndex: 0 });
            return array_all;
          };
          const ac = new AudioContext({
            latencyHint: "interactive"
          });
          const dest = ac.createMediaStreamDestination();
          const [track] = dest.stream.getAudioTracks();
          const self = this;
          track.writable = new WritableStream({
            async start(controller) {
              this.arrays = [];
              function worklet() {
                registerProcessor("mstg-shim", class Processor extends AudioWorkletProcessor {
                  constructor() {
                    super();
                    this.arrays = [];
                    this.arrayOffset = 0;
                    this.port.onmessage = ({ data }) => this.arrays.push(data);
                    this.emptyArray = new Float32Array(0);
                  }
                  process(inputs, [[output]]) {
                    for (let i2 = 0; i2 < output.length; i2++) {
                      if (!this.array || this.arrayOffset >= this.array.length) {
                        this.array = this.arrays.shift() || this.emptyArray;
                        this.arrayOffset = 0;
                      }
                      output[i2] = this.array[this.arrayOffset++] || 0;
                    }
                    return true;
                  }
                });
              }
              await ac.audioWorklet.addModule(`data:text/javascript,(${worklet.toString()})()`);
              this.node = new AudioWorkletNode(ac, "mstg-shim");
              this.node.connect(dest);
              return track;
            },
            write(audioData) {
              const array = makeMono(audioData);
              ((a) => {
                this.node.port.postMessage(a, [a.buffer]);
              })(array);
              audioData.close();
            }
          });
          return track;
        }
      }
    };
  }
  var supportedCodecs = [];
  function RawPlayer(url) {
    var controller = this;
    this.connecting = false;
    this.createControlChannel = function() {
      this.control = new MistUtil.shared.ControlChannel(function() {
        var ws = new WebSocket(url);
        ws.binaryType = "arraybuffer";
        return ws;
      }, MistVideo2);
      this.connection = this.control;
      this.control.addListener("channel_timeout").then(function() {
        MistVideo2.log("Control channel timeout - try next combo", "error");
        MistVideo2.nextCombo(void 0, "control channel timeout");
      });
      this.control.addListener("channel_error").then(function() {
        if (controller.control.was_connected) {
          MistVideo2.log("Attempting to reconnect control channel");
          controller.createControlChannel();
        }
      });
      Object.defineProperty(this.control, "debugging", {
        get: function() {
          return main.debugging;
        }
      });
      var control = this.control;
      var requestingMoreBuffer = false;
      this.control.addListener("on_time", function(msg) {
        for (var i2 in msg.tracks) {
          var tid = msg.tracks[i2];
          if (!(tid in controller.pipelines)) {
            if (main.debugging) console.warn("\u25B6\uFE0F", "Received on_time with track " + tid + " but there is no pipeline for it, attempting to create it");
            var p11 = new Pipeline(tid);
          }
        }
        for (var tid in controller.pipelines) {
          if (msg.tracks.indexOf(Number(tid)) < 0) {
            controller.pipelines[tid].close(true);
          }
        }
        var last = controller.frameTiming.server;
        if (last && msg.current != last.current) {
          for (var tid in controller.pipelines) {
            var pipeline = controller.pipelines[tid];
            if (pipeline.waitingTimer) {
              clearTimeout(pipeline.waitingTimer);
              pipeline.waitingTimer = false;
            }
          }
        }
        var bounds = {
          // if the buffer <> desiredBuffer*bounds[low,high], take action
          low: 0.6,
          high: 2
        };
        var tweaks = {
          //action to take when the bounds are reached
          faster: 1.05,
          slower: 0.98
        };
        var buffer = null;
        var playbackPoint = controller.frameTiming.out;
        var receivePoint = controller.frameTiming.in;
        var decodePoint = controller.frameTiming.decoded;
        if (playbackPoint && decodePoint) {
          buffer = Math.round(decodePoint * 1e-3 - playbackPoint * 1e-3);
          decodingTime = Math.round(receivePoint * 1e-3 - decodePoint * 1e-3);
        }
        if (buffer !== null && !controller.frameTiming.seeking && !requestingMoreBuffer) {
          var desiredBuffer = controller.desiredBuffer();
          if (main.debugging) {
            var args = [
              "\u25B6\uFE0F",
              "Buffer:",
              (function() {
                if (buffer > desiredBuffer * bounds.high) {
                  return "\u2B06\uFE0F";
                }
                if (buffer < desiredBuffer * bounds.low) {
                  return "\u2B07\uFE0F";
                }
                return "\u{1F7E2}";
              })(),
              buffer,
              "/",
              desiredBuffer,
              {
                keepAway,
                serverDelay: Math.round(controller.control.serverDelay.get()),
                jitter: Math.round(controller.jitter.get())
              },
              //TODO "Decoding time:",Math.round(Math.max.apply(null,Object.values(main.api.decodeTime))),
              //TODO "Decoding queues:",Math.max.apply(null,Object.values(main.api.decodeQueue)),
              //TODO "Earliness:",Math.round(Math.min.apply(null,Object.values(main.api.earliness))),
              //"Server jitter:",msg.jitter,
              "Available:",
              msg.end - msg.current,
              "Tweak:",
              (function(t) {
                if (t > 1) {
                  return "\u2197\uFE0F";
                }
                if (t < 1) {
                  return "\u2198\uFE0F";
                }
                return "\u{1F7E2}";
              })(controller.frameTiming.speed.tweak),
              controller.frameTiming.speed.tweak
            ];
            if (MistVideo2.info.type == "live") {
              args.unshift("From live:", Math.round(msg.end * 0.01 - playbackPoint * 1e-5) / 10, "s");
            }
            console.log.apply(null, args);
          }
          if (buffer < desiredBuffer * bounds.low && msg.play_rate_curr != "fast-forward" && controller.frameTiming.speed.tweak >= 1) {
            var underflow = 0;
            if (buffer < 10) {
              var early = main.api.earliness;
              if (early) underflow = -1 * Math.min.apply(null, Object.values(early));
              underflow = Math.max(0, underflow);
            }
            if (msg.current < msg.end) {
              requestingMoreBuffer = true;
              control.send({
                type: "fast_forward",
                ff_add: desiredBuffer + underflow
              });
              if (controller.frameTiming.speed.tweak > 1) {
                controller.frameTiming.tweakSpeed(1);
              }
              MistVideo2.log("Our buffer (" + buffer + "ms) is small (<" + Math.round(desiredBuffer * bounds.low) + "ms), requesting more data (+" + Math.round(desiredBuffer + underflow) + "ms)..");
              var gotsetspeed = false;
              control.addListener("set_speed").then(function(m) {
                gotsetspeed = true;
                if (m.play_rate_prev == "fast-forward") {
                  control.addListener("on_time").then(function(m2) {
                    var newbuffer = 0;
                    var playbackPoint2 = controller.frameTiming.out;
                    var decodePoint2 = controller.frameTiming.in;
                    if (playbackPoint2 && decodePoint2) {
                      newbuffer = Math.round(decodePoint2 * 1e-3 - playbackPoint2 * 1e-3);
                    }
                    var increase = m2.current - msg.current - (m2._received - msg._received);
                    if (main.debugging) console.warn("\u25B6\uFE0F", "Extra buffer received:", m2.current - msg.current, "ms", "Time taken:", m2._received - msg._received, "ms", "Increase:", increase, "ms");
                    if (buffer + increase < desiredBuffer * bounds.low) {
                      controller.frameTiming.tweakSpeed(tweaks.slower);
                      keepAway += 100;
                      MistVideo2.log("Didn't receive enough extra data to increase our buffer (" + increase + "/" + Math.round(desiredBuffer * bounds.low - buffer) + "ms): slowing down..");
                    } else {
                      MistVideo2.log("Received +" + increase + "ms extra data");
                    }
                    requestingMoreBuffer = false;
                  });
                } else {
                  requestingMoreBuffer = false;
                }
              });
              control.addListener("on_time").then(function(m) {
                if (gotsetspeed) return;
                if (requestingMoreBuffer && m.play_rate_curr != "fast-forward") {
                  requestingMoreBuffer = false;
                  controller.frameTiming.tweakSpeed(tweaks.slower);
                  keepAway += 100;
                  MistVideo2.log("Didn't receive extra data: slowing down..");
                }
              });
            } else {
              if (controller.frameTiming.speed.main > 1) {
                control.send({ type: "set_speed", play_rate: "auto" });
              }
              controller.frameTiming.tweakSpeed(tweaks.slower);
              MistVideo2.log("Our buffer (" + buffer + "ms) is small (<" + Math.round(desiredBuffer * bounds.low) + "ms), but can't request more data: slowing down..");
            }
          } else {
            if (controller.frameTiming.speed.tweak < 1 && buffer >= desiredBuffer) {
              controller.frameTiming.tweakSpeed(1);
              MistVideo2.log("Our buffer (" + buffer + "ms) is large enough (>" + Math.round(desiredBuffer) + "ms), so return to normal playback.");
            } else {
              if (MistVideo2.info.type == "live" && MistVideo2.options.liveCatchup) {
                if (msg.play_rate_curr == "auto" && controller.frameTiming) {
                  var max_frame_duration = 0;
                  for (var i2 in controller.pipelines) {
                    max_frame_duration = Math.max(max_frame_duration, controller.pipelines[i2].stats.frame_duration);
                  }
                  desiredBuffer = Math.max(desiredBuffer, max_frame_duration * 1e-3);
                  if (controller.frameTiming.speed.tweak <= 1 && buffer > desiredBuffer * bounds.high) {
                    controller.frameTiming.tweakSpeed(tweaks.faster);
                    MistVideo2.log("Our buffer (" + buffer + "ms) is big (>" + Math.round(desiredBuffer * bounds.high) + "ms), so tweak the playback speed to catch up.");
                  } else if (controller.frameTiming.speed.tweak > 1 && buffer <= desiredBuffer) {
                    controller.frameTiming.tweakSpeed(1);
                    MistVideo2.log("Our buffer (" + buffer + "ms) is small enough (<" + Math.round(desiredBuffer) + "ms), so return to normal playback.");
                  }
                }
                if (msg.play_rate_curr != "fast-forward") {
                  var distanceToLive = msg.end - msg.current;
                  if (distanceToLive < MistVideo2.options.liveCatchup * 1e3 && distanceToLive > Math.max(msg.jitter * 1.1, msg.jitter + 250) && buffer - desiredBuffer < 1e3) {
                    control.send({
                      type: "fast_forward",
                      ff_add: 5e3
                      //request an additional 5 seconds of data
                    });
                    MistVideo2.log("We're away (" + distanceToLive + "ms) from the live point, requesting more data..");
                  }
                }
              }
            }
          }
        }
        controller.frameTiming.server = msg;
      });
      this.control.addListener("set_speed", function(msg) {
        var speed;
        switch (msg.play_rate_curr) {
          case "auto":
          case "fast-forward": {
            speed = 1;
            break;
          }
          default: {
            speed = msg.play_rate_curr;
            break;
          }
        }
        controller.frameTiming.setSpeed(speed);
        controller.jitter.setSpeed(msg.play_rate_curr);
      });
      this.control.addSendListener("seek", function(msg) {
        var seekTo = msg.seek_time;
        controller.worker.post({ type: "seek", seek_time: seekTo });
        MistUtil.event.send("seeking", seekTo * 1e-3, video);
        controller.jitter.reset();
        if (main.debugging) console.warn("\u25B6\uFE0F", "Seeking to [" + MistUtil.format.time(seekTo * 1e-3) + "]: Emptying decoding and display queues");
      });
      this.control.addListener("pause", function(msg) {
        if (msg.paused) controller.frameTiming.paused = true;
      });
      this.control.addSendListener("play", function() {
        controller.frameTiming.reset();
      });
      return this.control;
    };
    this.control = this.createControlChannel();
    function Differentiate(get_func) {
      var lastval, lasttime;
      this.get = function() {
        var newval = get_func();
        var now = performance.now();
        var out;
        if (typeof lastval != "undefined") {
          var dx = newval - lastval;
          var dt = now - lasttime;
          dt *= 1e-3;
          out = dx / dt;
        }
        lastval = newval;
        lasttime = now;
        return out;
      };
    }
    ;
    function FrameTracker() {
      var pending = {};
      this.complete = [];
      this.last_in = void 0;
      this.last_out = void 0;
      this.shift_array = [];
      this.start = function(timestamp) {
        this.last_in = timestamp;
        pending[timestamp] = performance.now();
      };
      this.end = function(timestamp) {
        this.last_out = timestamp;
        var shift = 0;
        if (!(timestamp in pending)) {
          timestamp = (function(a) {
            for (var i2 in a) return i2;
            return void 0;
          })(pending);
          if (timestamp === void 0) return;
          shift = timestamp - this.last_out;
        }
        this.complete.push(performance.now() - pending[timestamp]);
        this.shift_array.push(shift);
        delete pending[timestamp];
        while (this.complete.length > 16) {
          this.complete.shift();
        }
        while (this.shift_array.length > 8) {
          this.shift_array.shift();
        }
      };
      this.getCopy = function() {
        return {
          complete: this.complete,
          last_in: this.last_in,
          last_out: this.last_out,
          shift_array: this.shift_array
        };
      };
      Object.defineProperty(this, "delay", {
        get: function() {
          if (this.complete.length) {
            return this.complete.reduce(function(partialsum, a) {
              return partialsum + a;
            }) / this.complete.length;
          }
          return void 0;
        }
      });
      Object.defineProperty(this, "delta", {
        get: function() {
          if (this.last_out !== void 0) return this.last_in - this.last_out;
          return void 0;
        }
      });
      Object.defineProperty(this, "shift", {
        get: function() {
          if (this.shift_array.length) {
            return this.shift_array.reduce(function(partialsum, a) {
              return partialsum + a;
            }) / this.shift_array.length;
          }
          return void 0;
        }
      });
    }
    function RawChunk(data) {
      var l = 12;
      var info = new Uint8Array(data.slice(0, l));
      this.data = new Uint8Array(data.slice(l, data.byteLength));
      this.track = info[0];
      this.type = (function(b) {
        switch (b) {
          case 1:
            return "key";
          case 2:
            return "init";
          default:
            return "delta";
        }
      })(info[1]);
      this.timestamp = (function(infoBytes) {
        var v = new DataView(infoBytes.slice(2, 10).buffer);
        return Number(v.getBigUint64());
      })(info);
      this.offset = (function(infoBytes) {
        return new DataView(infoBytes.slice(10, 12).buffer).getInt16();
      })(info);
      if (main.debugging == "verbose") console.log("\u25B6\uFE0F", "Parsed binary data received from WS:", MistUtil.format.time((this.timestamp + this.offset) * 1e-3, { ms: true }), this);
    }
    function getTrack(trackIndex) {
      if (!MistVideo2.info || !MistVideo2.info.meta || !MistVideo2.info.meta.tracks) return;
      for (var i2 in MistVideo2.info.meta.tracks) {
        var track = MistVideo2.info.meta.tracks[i2];
        if (track.idx == trackIndex) {
          return track;
        }
      }
      throw "Missing metadata for track " + trackIndex;
    }
    this.data_queue = [];
    function JitterTracker() {
      let chunks = [];
      let speed = 1;
      let lastcalced = 0;
      let curJitter = 0;
      let peaks = [];
      let maxJitter = 120;
      this.addChunk = function(chunk) {
        chunks.push([
          performance.now(),
          chunk.timestamp
        ]);
        if (chunks.length > 8) chunks.shift();
        let jitter = this.calcJitter();
        if (jitter > curJitter) curJitter = jitter;
        if (performance.now() > lastcalced + 1e3) {
          peaks.push(curJitter);
          if (peaks.length > 8) peaks.shift();
          let maxPeak = Math.max.apply(null, peaks);
          let avgPeak = peaks.reduce(function(partialsum, a) {
            return partialsum + a;
          }) / peaks.length;
          let weighted = (avgPeak + maxPeak * 2) / 3 + 1;
          if (maxJitter > weighted + 500) {
            weighted = maxJitter - 500;
          }
          maxJitter = (maxJitter + weighted) / 2;
          curJitter = 0;
          lastcalced = performance.now();
        }
      };
      this.calcJitter = function() {
        if (chunks.length <= 1) return 0;
        if (speed == "fast_forward") return 0;
        let oldest = chunks[0];
        let newest = chunks[chunks.length - 1];
        let clock_time_passed = newest[0] - oldest[0];
        let media_time_passed = newest[1] - oldest[1];
        let jitter = media_time_passed / speed - clock_time_passed;
        return Math.max(0, jitter);
      };
      this.get = function() {
        return maxJitter;
      };
      this.setSpeed = function(newspeed) {
        if (speed != newspeed) {
          if (newspeed == "auto") newspeed = 1;
          speed = newspeed;
          this.reset();
        }
      };
      this.reset = function() {
        chunks = [];
      };
    }
    this.jitter = new JitterTracker();
    this.receiver = function(data) {
      var chunk = new RawChunk(data);
      if (!(chunk.track in controller.pipelines)) {
        if (main.debugging) console.warn("\u25B6\uFE0F", "Received data for track " + chunk.track + " but there is no pipeline for it, attempting to create it");
        new Pipeline(chunk.track);
      }
      var pipeline = controller.pipelines[chunk.track];
      if (chunk.type == "init") {
        pipeline.configure(chunk.data);
        return;
      }
      try {
        pipeline.receive({
          type: chunk.type,
          //Indicates if the chunk is a key chunk that does not rely on other frames for encoding. One of: "key" The data is a key chunk. "delta" The data is not a key chunk.
          timestamp: (chunk.timestamp + chunk.offset) * 1e3,
          //An integer representing the timestamp of the video in microseconds.
          data: chunk.data
          //An ArrayBuffer, a TypedArray, or a DataView containing the video data.
          //transfer: [chunk.data]                           //An array of ArrayBuffers that EncodedVideoChunk will detach and take ownership of. If the array contains the ArrayBuffer backing data, EncodedVideoChunk will use that buffer directly instead of copying from it.
        });
        MistUtil.event.send("progress", null, video);
      } catch (err) {
        if (main.debugging) console.error("\u25B6\uFE0F", "Error while decoding track " + chunk.track + " (" + pipeline.track.codec + " " + pipeline.track.type + ")", chunk, err);
        MistVideo2.log(err + " while decoding track " + chunk.track + " (" + pipeline.track.codec + " " + pipeline.track.type + ")");
        pipeline.reset();
      }
      controller.jitter.addChunk(chunk);
    };
    this.pipelines = {};
    Object.defineProperty(this.pipelines, "audio", {
      enumerable: false,
      get: function() {
        for (var i2 in this) {
          if (this[i2].track && this[i2].track.type == "audio") {
            return this[i2];
          }
        }
      }
    });
    Object.defineProperty(this.pipelines, "video", {
      enumerable: false,
      get: function() {
        for (var i2 in this) {
          if (this[i2].track && this[i2].track.type == "video") {
            return this[i2];
          }
        }
      }
    });
    function WebCodecsWorker() {
      var self = this;
      this.worker = new Worker(mistplayers.rawws.scriptsrc(MistVideo2.options.host));
      this.worker.onmessage = function(e) {
        var msg = e.data;
        var key = [msg.type, msg.idx, msg.uid].join("_");
        if (msg.stats) {
          if (msg.stats.FrameTiming) {
            Object.assign(controller.frameTiming, msg.stats.FrameTiming);
          }
          if (msg.stats.pipelines) {
            for (var i2 in msg.stats.pipelines) {
              if (i2 in controller.pipelines) {
                var p11 = controller.pipelines[i2];
                var stats = msg.stats.pipelines[i2];
                p11.stats.early = stats.early;
                p11.stats.frame_duration = stats.frame_duration;
                Object.assign(p11.stats.frames, stats.frames);
                Object.assign(p11.stats.queues, stats.queues);
                Object.assign(p11.stats.timing.decoder, stats.timing.decoder);
                Object.assign(p11.stats.timing.writable, stats.timing.writable);
              }
            }
          }
        }
        var listened = false;
        if (key in self.listeners) {
          self.listeners[key](msg);
          if (msg.uid) delete self.listeners[key];
          listened = true;
        }
        key = [msg.type, msg.idx, ""].join("_");
        if (key in self.listeners) {
          self.listeners[key](msg);
          listened = true;
        }
        key = [msg.type, "", ""].join("_");
        if (key in self.listeners) {
          self.listeners[key](msg);
          listened = true;
        }
        if (main.debugging) {
          switch (msg.type) {
            case "receive":
            case "writeframe":
            case "log": {
              break;
            }
            case "sendevent": {
              if (msg.kind == "timeupdate") break;
            }
            default: {
              console.log("\u{1F4BC}", "Worker sent:", msg);
            }
          }
        }
        if (!listened) console.warn("\u25B6\uFE0F", "Received unhandled msg from worker", msg);
      };
      this.listeners = {};
      var uid = 0;
      this.addListener = function(opts, callback2) {
        var key = [opts.type, opts.idx, opts.uid].join("_");
        if (callback2) {
          this.listeners[key] = callback2;
        } else {
          var self2 = this;
          return new Promise(function(resolve, reject) {
            self2.listeners[key] = function(msg) {
              if (msg.status == "error") return reject(msg);
              resolve(msg);
            };
          });
        }
      };
      this.post = function(msg, opts) {
        if (!("uid" in msg)) {
          msg.uid = uid++;
        }
        if (opts && opts.transfer && !Array.isArray(opts.transfer)) {
          opts.transfer = [opts.transfer];
        }
        this.worker.postMessage(msg, opts);
        return new Promise(function(resolve, reject) {
          self.addListener(msg).then(resolve).catch(reject);
        });
      };
      this.terminate = function() {
        this.worker.terminate();
      };
      self.post({
        type: "debugging",
        value: main.debugging
      });
    }
    this.worker = new WebCodecsWorker();
    this.worker.addListener({ type: "writeframe" }, function(msg) {
      var pipeline = controller.pipelines[msg.idx];
      if (pipeline) {
        var uid = msg.frame.timestamp;
        pipeline.trackWriter.write(msg.frame).then(function() {
          controller.worker.post({
            type: "writeframe",
            idx: msg.idx,
            uid,
            status: "ok"
          });
        }).catch(function(err) {
          controller.worker.post({
            type: "writeframe",
            idx: msg.idx,
            uid,
            status: "error",
            error: err
          });
        });
      } else {
        controller.worker.post({
          type: "writeframe",
          idx: msg.idx,
          uid: msg.frame.timestamp,
          status: "error",
          error: "Pipeline not active"
        });
      }
    });
    this.worker.addListener({ type: "log" }, function(msg) {
      MistVideo2.log("WebCodecsWorker: " + msg.msg);
    });
    this.worker.addListener({ type: "sendevent" }, function(msg) {
      MistUtil.event.send(
        msg.kind,
        msg.message ? msg.message : "idx" in msg ? "track " + msg.idx : null,
        video
      );
    });
    this.worker.addListener({ type: "addtrack" }, function(msg) {
      var pipeline = controller.pipelines[msg.idx];
      if (pipeline) {
        if ("track" in msg) {
          pipeline.trackGenerator = msg.track;
        } else {
          if (!pipeline.trackGenerator) {
            pipeline.createTrackGenerator();
          }
        }
        if (pipeline.trackGenerator) controller.stream.addTrack(pipeline.trackGenerator);
      }
    });
    this.worker.addListener({ type: "removetrack" }, function(msg) {
      var pipeline = controller.pipelines[msg.idx];
      if (pipeline) {
        controller.stream.removeTrack(pipeline.trackGenerator);
        pipeline.trackGenerator = null;
      }
    });
    this.worker.addListener({ type: "setplaybackrate" }, function(msg) {
      MistVideo2.video.playbackRate = msg.speed;
    });
    this.worker.addListener({ type: "closed" }, function(msg) {
      if (msg.idx in controller.pipelines) {
        delete controller.pipelines[msg.idx];
      }
    });
    function Pipeline(track) {
      if (!(typeof track == "object")) {
        track = getTrack(track);
      }
      if (track.type != "audio" && track.type != "video") {
        controller.control.send({ type: "hold" });
        return;
      }
      function post(obj, transfer) {
        obj.idx = track.idx;
        if (transfer) return controller.worker.post(obj, { transfer });
        return controller.worker.post(obj);
      }
      var pipeline = this;
      pipeline.track = track;
      pipeline.stats = {
        early: null,
        frames: {
          in: 0,
          decoded: 0,
          out: 0
        },
        framerate: {
          in: new Differentiate(function() {
            return pipeline.stats.frames.in;
          }),
          decoded: new Differentiate(function() {
            return pipeline.stats.frames.decoded;
          }),
          out: new Differentiate(function() {
            return pipeline.stats.frames.out;
          })
        },
        queues: {
          in: 0,
          decoder: 0,
          out: 0
        },
        timing: {
          decoder: new FrameTracker(),
          writable: new FrameTracker()
        }
      };
      pipeline.createTrackGenerator = function() {
        if (!window.MediaStreamTrackGenerator) {
          if (track.type == "video") {
            post({
              type: "creategenerator"
            });
          } else if (track.type == "audio") {
            pipeline.trackGenerator = new MediaStreamTrackGenerator({ kind: "audio" });
            pipeline.trackWriter = pipeline.trackGenerator.writable.getWriter();
            post({
              //will create a dummy trackwriter that postMessages the frames
              type: "creategenerator"
            });
          }
        } else {
          pipeline.trackGenerator = MediaStreamTrackGenerator ? new MediaStreamTrackGenerator({ kind: track.type }) : new window.MediaStreamTrackGenerator({ kind: track.type });
          post({
            type: "setwritable",
            writable: pipeline.trackGenerator.writable
          }, pipeline.trackGenerator.writable);
        }
      };
      pipeline.configure = function(header) {
        post({
          type: "configure",
          header
        }, "buffer" in header ? header.buffer : void 0);
      };
      pipeline.receive = function(chunkOpts) {
        post({
          type: "receive",
          chunk: chunkOpts
        }, chunkOpts.data.buffer);
      };
      pipeline.close = function(waitEmpty) {
        post({
          type: "close",
          waitEmpty
        });
      };
      MistVideo2.log("Creating pipeline for track " + track.idx + " (" + track.codec + " " + track.type + ")");
      post({
        type: "create",
        track,
        opts: { optimizeForLatency: MistVideo2.info.type == "live" }
      });
      pipeline.createTrackGenerator();
      controller.pipelines[track.idx] = pipeline;
    }
    function FrameTiming() {
      this.tweakSpeed = function(tweak) {
        this.setSpeed(this.speed.main, tweak);
      };
      this.setSpeed = function(speed, tweak) {
        controller.worker.post({
          type: "frametiming",
          action: "setSpeed",
          speed,
          tweak
        });
      };
      this.reset = function() {
        controller.worker.post({
          type: "frametiming",
          action: "reset"
        });
        this.in = null;
        this.decoded = null;
        this.out = null;
        this.paused = false;
        this.speed = {
          main: 1,
          tweak: 1,
          combined: 1
        };
      };
      this.reset();
    }
    this.frameTiming = new FrameTiming();
    var keepAway = MistVideo2.info.type == "live" ? 100 : 500;
    this.desiredBuffer = function() {
      var out = keepAway + controller.control.serverDelay.get() + controller.jitter.get();
      return Math.round(out);
    };
    this.connect = function() {
      if (this.connecting) {
        return this.connecting;
      }
      this.connecting = new Promise(function(resolve, reject) {
        var control = controller.control;
        if (control.connectionState == "closing" || control.connectionState == "closed") {
          control.reconnect();
        }
        controller.stream = new MediaStream();
        video.srcObject = controller.stream;
        controller.frameTiming.reset();
        return controller.control.addListener("channel_open").then(function() {
          var codecs = [Object.values(supportedCodecs2).filter(function(a) {
            return a.length;
          })];
          control.send({ type: "request_codec_data", supported_combinations: codecs });
          MistVideo2.log("Requesting codecs: " + codecs.join(", "));
          controller.connecting = false;
          return control.addListener("codec_data");
        }).then(function(msg) {
          if (!msg.codecs || !msg.codecs.length) {
            throw "No playable codecs available";
          }
          for (var i2 in msg.tracks) {
            var trackIndex = msg.tracks[i2];
            if (!(i2 in msg.codecs)) throw "No codec data for track " + trackIndex;
            controller.pipelines[trackIndex] = new Pipeline(trackIndex);
          }
          control.send({ type: "play" });
          control.addListener("binary", controller.receiver);
          if (!MistVideo2.options.autoplay) {
            try {
              video.play();
            } catch (e) {
            }
            setTimeout(function() {
              control.send({ type: "hold" });
            }, 100);
          }
          resolve();
        }).catch(function(err) {
          MistVideo2.showError(err);
          if (main.debugging) {
            console.error("\u25B6\uFE0F", err);
          }
        });
      });
      return this.connecting;
    };
    this.close = function() {
      this.control.removeListener("binary", controller.receiver);
      this.control.close();
      if (controller.stream) {
        for (var i2 in controller.pipelines) {
          controller.pipelines[i2].close();
        }
      }
    };
    var supportedCodecs2 = { audio: [], video: [] };
    function checkTracksAndConnect() {
      var promises = [], thePlayer = mistplayers.rawws;
      if (!(MistVideo2.stream in thePlayer.cacheSupported)) {
        thePlayer.cacheSupported[MistVideo2.stream] = {};
      }
      var theCache = thePlayer.cacheSupported[MistVideo2.stream];
      function checkTrack(i3) {
        function pushIfUnique(array, entry) {
          if (array.indexOf(entry) < 0) array.push(entry);
        }
        var track = MistVideo2.info.meta.tracks[i3];
        if (track.type != "audio" && track.type != "video") {
          return;
        }
        if (i3 in theCache) {
          if (theCache[i3]) {
            pushIfUnique(supportedCodecs2[track.type], track.codec);
          }
        } else {
          promises.push(mistplayers.rawws.isTrackSupported(track).then(function(result) {
            theCache[i3] = result.supported;
            if (result.supported) {
              pushIfUnique(supportedCodecs2[track.type], track.codec);
            }
          }));
        }
      }
      for (var i2 in MistVideo2.info.meta.tracks) {
        checkTrack(i2);
      }
      if (promises.length) {
        Promise.all(promises).then(connectIfEnough).catch(console.err);
      } else {
        connectIfEnough();
      }
    }
    function connectIfEnough() {
      var skip = false;
      if (skip) {
        supportedCodecs2 = { audio: [], video: [] };
        for (var i2 in MistVideo2.info.meta.tracks) {
          var track = MistVideo2.info.meta.tracks[i2];
          switch (track.type) {
            case "audio":
            case "video": {
              if (supportedCodecs2[track.type].indexOf(track.codec) < 0) supportedCodecs2[track.type].push(track.codec);
            }
          }
        }
      } else {
        var oldtracks = mistplayers.rawws.cacheSupported[MistVideo2.stream].last;
        var myscore = MistVideo2.scoreCombo(mistplayers.rawws.isBrowserSupported(MistVideo2.source.type, MistVideo2.source, MistVideo2));
        if (myscore < 1) {
          MistVideo2.log("After testing decoders, " + mistplayers[MistVideo2.playerName].name + " cannot play any tracks after all: trying next combo..");
          MistVideo2.nextCombo(false, "rawws cannot play any tracks");
          return;
        }
        var newtracks = mistplayers.rawws.cacheSupported[MistVideo2.stream].last;
        for (var kind in oldtracks) {
          if (!(kind in newtracks)) {
            var firstCombo = MistVideo2.checkCombo({ startCombo: false }, true);
            if (firstCombo.score > myscore && firstCombo.player != MistVideo2.playerName) {
              MistVideo2.log("After testing decoders, " + mistplayers[MistVideo2.playerName].name + " is no longer the best scoring player, trying next combo..");
              MistVideo2.nextCombo(false, "after testing, rawws is no longer the best player");
              return;
            }
            break;
          }
        }
        MistVideo2.log("Confirmed " + mistplayers[MistVideo2.playerName].name + " is still the best scoring player, connecting..");
      }
      controller.connect();
    }
    checkTracksAndConnect();
  }
  this.controller = new RawPlayer(MistVideo2.source.url);
  var custom_funcs = {};
  custom_funcs.currentTime = {
    get: function() {
      var frameTiming = main.controller.frameTiming;
      if (frameTiming) {
        var ts = frameTiming.out;
        return ts ? ts * 1e-6 : 0;
      } else return 0;
    },
    set: function(value) {
      main.controller.control.send({
        type: "seek",
        seek_time: value == "live" ? "live" : Math.round(value * 1e3),
        ff_add: main.controller.desiredBuffer()
      });
    }
  };
  custom_funcs.buffered = {
    get: function() {
      return new function TimeRanges() {
        var frameTiming = main.controller.frameTiming;
        Object.defineProperty(this, "length", {
          get: function() {
            if (frameTiming && frameTiming.in && frameTiming.out) return 2;
            return 0;
          }
        });
        this.start = function(index) {
          if (frameTiming) {
            var info;
            switch (index) {
              case 0:
                info = frameTiming.out;
                break;
              case 1:
                info = frameTiming.decoded;
                break;
              default:
                throw new Error("Index out of bounds");
            }
            return info ? info * 1e-6 : 0;
          }
        };
        this.end = function(index) {
          if (frameTiming) {
            var info;
            switch (index) {
              case 0:
                info = frameTiming.decoded;
                break;
              case 1:
                info = frameTiming.in;
                break;
              default:
                throw new Error("Index out of bounds");
            }
            return info ? info * 1e-6 : 0;
          }
        };
      }();
    }
  };
  custom_funcs.duration = {
    get: function() {
      var frameTiming = main.controller.frameTiming;
      if (frameTiming) {
        var info = frameTiming.server;
        if (info && "end" in info) {
          if (MistVideo2.info && MistVideo2.info.type == "live") {
            return (info.end + (/* @__PURE__ */ new Date()).getTime() - info._received.getTime()) * 1e-3;
          } else return info.end * 1e-3;
        }
      }
      return 0;
    }
  };
  custom_funcs.stream_end = function() {
    var promises = [];
    for (var tid in main.controller.pipelines) {
      promises.push(main.controller.pipelines[tid].close(true));
    }
    Promise.all(promises).then(function() {
      MistUtil.event.send("ended", null, video);
    });
  };
  var looping = false;
  custom_funcs.restart = function() {
    if (looping) return;
    looping = true;
    var promises = [];
    for (var tid in main.controller.pipelines) {
      promises.push(main.controller.pipelines[tid].close(true));
    }
    Promise.all(promises).then(function() {
      MistVideo2.log("Looping..");
      var result = main.controller.close();
      if (result instanceof Promise) {
        result.then(function() {
          main.controller.connect().then(function() {
            looping = false;
          });
        });
      } else {
        main.controller.connect().then(function() {
          looping = false;
        });
      }
    });
  };
  custom_funcs.receiveQueue = {
    //queue of chunks received over the control channel, but not yet passed to the decoder (because it is not yet configured) -> should be 0
    get: function() {
      var out = {};
      for (var i2 in main.controller.pipelines) {
        var pipeline = main.controller.pipelines[i2];
        if (pipeline.track) {
          out[pipeline.track.type] = pipeline.stats.queues.in;
        }
      }
      return out;
    }
  };
  custom_funcs.decodeQueue = {
    //browsers internal decoder queue -> should be 0
    get: function() {
      var out = {};
      for (var i2 in main.controller.pipelines) {
        var pipeline = main.controller.pipelines[i2];
        if (pipeline.track) {
          out[pipeline.track.type] = pipeline.stats.queues.decoder;
        }
      }
      return out;
    }
  };
  custom_funcs.displayQueue = {
    //queue of frames that have been decoded, but are not yet passed to the video element (track writer) -> should be > 0
    get: function() {
      var out = {};
      for (var i2 in main.controller.pipelines) {
        var pipeline = main.controller.pipelines[i2];
        if (pipeline.track) {
          out[pipeline.track.type] = pipeline.stats.queues.out;
        }
      }
      return out;
    }
  };
  custom_funcs.displayBuffer = {
    //duration of frames that have been decoded, but are not yet passed to the video element (track writer) -> should be desired buffer size
    get: function() {
      var out = {};
      for (var i2 in main.controller.pipelines) {
        var pipeline = main.controller.pipelines[i2];
        if (pipeline.track) {
          out[pipeline.track.type] = (pipeline.stats.timing.decoder.last_out - pipeline.stats.timing.writable.last_in) * 1e-3;
        } else {
          out[pipeline.track.type] = 0;
        }
      }
      return out;
    }
  };
  custom_funcs.earliness = {
    get: function() {
      var out = {};
      for (var i2 in main.controller.pipelines) {
        var pipeline = main.controller.pipelines[i2];
        if (pipeline.track) {
          if ("early" in pipeline.stats) {
            out[pipeline.track.type] = pipeline.stats.early;
          } else {
            out[pipeline.track.type] = 0;
          }
        }
      }
      return out;
    }
  };
  custom_funcs.sync = {
    //difference in output timestamps between video and audio tracks, in ms
    get: function() {
      var audio;
      var video2;
      for (var i2 in main.controller.pipelines) {
        var pipeline = main.controller.pipelines[i2];
        if (pipeline.track) {
          if (pipeline.closeWhenEmpty) continue;
          switch (pipeline.track.type) {
            case "audio": {
              audio = pipeline;
              break;
            }
            case "video": {
              video2 = pipeline;
              break;
            }
          }
        }
      }
      function get(p11) {
        return p11.stats.timing.writable.last_out;
      }
      try {
        return Math.abs(get(audio) - get(video2)) * 1e-3;
      } catch {
        return 0;
      }
    }
  };
  custom_funcs.framerate_in = {
    get: function() {
      return function() {
        var out = {};
        for (var i2 in main.controller.pipelines) {
          var pipeline = main.controller.pipelines[i2];
          if (pipeline.track) {
            if ("in" in pipeline.stats.framerate) {
              out[pipeline.track.type] = pipeline.stats.framerate.in.get();
            } else {
              out[pipeline.track.type] = 0;
            }
          }
        }
        return out;
      };
    }
  };
  custom_funcs.framerate_decoder = {
    get: function() {
      return function() {
        var out = {};
        for (var i2 in main.controller.pipelines) {
          var pipeline = main.controller.pipelines[i2];
          if (pipeline.track) {
            if ("decoded" in pipeline.stats.framerate) {
              out[pipeline.track.type] = pipeline.stats.framerate.decoded.get();
            } else {
              out[pipeline.track.type] = 0;
            }
          }
        }
        return out;
      };
    }
  };
  custom_funcs.framerate_out = {
    get: function() {
      return function() {
        var out = {};
        for (var i2 in main.controller.pipelines) {
          var pipeline = main.controller.pipelines[i2];
          if (pipeline.track) {
            if ("out" in pipeline.stats.framerate) {
              out[pipeline.track.type] = pipeline.stats.framerate.out.get();
            } else {
              out[pipeline.track.type] = 0;
            }
          }
        }
        return out;
      };
    }
  };
  custom_funcs.local_jitter = {
    get: function() {
      return main.controller.jitter.get();
    }
  };
  custom_funcs.decodeTime = {
    //averaged time it takes for frames to decode [ms]
    get: function() {
      var out = {};
      for (var i2 in main.controller.pipelines) {
        var pipeline = main.controller.pipelines[i2];
        if (pipeline.track) {
          if (pipeline.stats.timing.decoder) {
            out[pipeline.track.type] = pipeline.stats.timing.decoder.delay;
          } else {
            out[pipeline.track.type] = 0;
          }
        }
      }
      return out;
    }
  };
  custom_funcs.displayTime = {
    //averaged time it takes for frames to pass the track writer [ms]
    get: function() {
      var out = {};
      for (var i2 in main.controller.pipelines) {
        var pipeline = main.controller.pipelines[i2];
        if (pipeline.track) {
          if (pipeline.stats.timing.writable) {
            out[pipeline.track.type] = pipeline.stats.timing.writable.delay;
          } else {
            out[pipeline.track.type] = 0;
          }
        }
      }
      return out;
    }
  };
  custom_funcs.shift = {
    //timestamp difference after exiting the decoder
    get: function() {
      var out = {};
      for (var i2 in main.controller.pipelines) {
        var pipeline = main.controller.pipelines[i2];
        if (pipeline.track) {
          if (pipeline.stats.timing.decoder) {
            out[pipeline.track.type] = pipeline.stats.timing.decoder.shift;
          } else {
            out[pipeline.track.type] = 0;
          }
        }
      }
      return out;
    }
  };
  this.api = new MistUtil.shared.ControlChannelAPI(main.controller, MistVideo2, video, custom_funcs);
  this.api.unload = function() {
    main.controller.worker.terminate();
    main.controller.close();
  };
  callback(video);
};

// src/wrappers/flv.js
registerWrapper("flv", {
  name: "HTML5 FLV Player",
  mimes: ["flash/7"],
  isMimeSupported: function(mimetype) {
    return MistUtil.array.indexOf(this.mimes, mimetype) == -1 ? false : true;
  },
  isBrowserSupported: function(mimetype, source, MistVideo2) {
    if (location.protocol != MistUtil.http.url.split(source.url).protocol) {
      if (location.protocol == "file:" && MistUtil.http.url.split(source.url).protocol == "http:") {
        MistVideo2.log("This page was loaded over file://, the player might not behave as intended.");
      } else {
        MistVideo2.log("HTTP/HTTPS mismatch for this source");
        return false;
      }
    }
    if (!window.MediaSource) {
      return false;
    }
    if (!MediaSource.isTypeSupported) {
      return true;
    }
    try {
      var playabletracks = {};
      for (var i2 in MistVideo2.info.meta.tracks) {
        if (MistVideo2.info.meta.tracks[i2].type == "meta") {
          continue;
        }
        if (!(MistVideo2.info.meta.tracks[i2].type in playabletracks)) {
          playabletracks[MistVideo2.info.meta.tracks[i2].type] = {};
        }
        playabletracks[MistVideo2.info.meta.tracks[i2].type][MistUtil.tracks.translateCodec(MistVideo2.info.meta.tracks[i2])] = 1;
      }
      var tracktypes = [];
      for (var type in playabletracks) {
        var playable = false;
        for (var codec in playabletracks[type]) {
          if (MediaSource.isTypeSupported('video/mp4;codecs="' + codec + '"')) {
            playable = true;
            break;
          }
        }
        if (playable) {
          tracktypes.push(type);
        }
      }
      source.supportedCodecs = tracktypes;
      return tracktypes.length ? tracktypes : false;
    } catch (e) {
    }
    return false;
  },
  player: function() {
    this.onreadylist = [];
  },
  scriptsrc: function(host) {
    return host + "/flv.js";
  }
});
var p10 = mistplayers.flv.player;
p10.prototype = new MistPlayer();
p10.prototype.build = function(MistVideo2, callback) {
  this.onFLVLoad = function() {
    if (MistVideo2.destroyed) {
      return;
    }
    MistVideo2.log("Building flv.js player..");
    var video = document.createElement("video");
    video.setAttribute("playsinline", "");
    var attrs = ["autoplay", "loop", "poster"];
    for (var i2 in attrs) {
      var attr = attrs[i2];
      if (MistVideo2.options[attr]) {
        video.setAttribute(attr, MistVideo2.options[attr] === true ? "" : MistVideo2.options[attr]);
      }
    }
    if (MistVideo2.options.muted) {
      video.muted = true;
    }
    if (MistVideo2.options.controls == "stock") {
      video.setAttribute("controls", "");
    }
    if (MistVideo2.info.type == "live") {
      video.loop = false;
    }
    flvjs.LoggingControl.applyConfig({
      enableVerbose: false
    });
    flvjs.LoggingControl.addLogListener(function(loglevel, message) {
      MistVideo2.log("[flvjs] " + message);
    });
    var opts = {
      type: "flv",
      url: MistVideo2.source.url,
      //isLive: true, //not needed apparently
      hasAudio: false,
      hasVideo: false
    };
    for (var i2 in MistVideo2.source.supportedCodecs) {
      opts["has" + MistVideo2.source.supportedCodecs[i2].charAt(0).toUpperCase() + MistVideo2.source.supportedCodecs[i2].slice(1)] = true;
    }
    MistVideo2.player.create = function(o) {
      o = MistUtil.object.extend({}, o);
      MistVideo2.player.flvPlayer = flvjs.createPlayer(o, {
        lazyLoad: false
        //if we let it lazyLoad, once it resumes, it will try to seek and fail miserably :)
      });
      MistVideo2.player.flvPlayer.attachMediaElement(video);
      MistVideo2.player.flvPlayer.load();
      MistVideo2.player.flvPlayer.play();
      if (!MistVideo2.options.autoplay) {
        video.pause();
      }
    };
    MistVideo2.player.create(opts);
    MistVideo2.player.api = {};
    function reroute(item) {
      Object.defineProperty(MistVideo2.player.api, item, {
        get: function() {
          return video[item];
        },
        set: function(value) {
          return video[item] = value;
        }
      });
    }
    var list = [
      "volume",
      "buffered",
      "muted",
      "loop",
      "paused",
      ,
      "error",
      "textTracks",
      "webkitDroppedFrameCount",
      "webkitDecodedFrameCount"
    ];
    if (MistVideo2.info.type != "live") {
      list.push("duration");
    } else {
      Object.defineProperty(MistVideo2.player.api, "duration", {
        get: function() {
          if (!video.buffered.length) {
            return 0;
          }
          return video.buffered.end(video.buffered.length - 1);
        }
      });
    }
    for (var i2 in list) {
      reroute(list[i2]);
    }
    function redirect(item) {
      if (item in video) {
        MistVideo2.player.api[item] = function() {
          return video[item].call(video, arguments);
        };
      }
    }
    var list = ["load", "getVideoPlaybackQuality", "play", "pause"];
    for (var i2 in list) {
      redirect(list[i2]);
    }
    MistVideo2.player.api.setSource = function(url) {
      if (url != opts.url && url != "") {
        MistVideo2.player.flvPlayer.unload();
        MistVideo2.player.flvPlayer.detachMediaElement();
        MistVideo2.player.flvPlayer.destroy();
        opts.url = url;
        MistVideo2.player.create(opts);
      }
    };
    MistVideo2.player.api.unload = function() {
      MistVideo2.player.flvPlayer.unload();
      MistVideo2.player.flvPlayer.detachMediaElement();
      MistVideo2.player.flvPlayer.destroy();
    };
    MistVideo2.player.setSize = function(size) {
      video.style.width = size.width + "px";
      video.style.height = size.height + "px";
    };
    Object.defineProperty(MistVideo2.player.api, "currentTime", {
      get: function() {
        return video.currentTime;
      },
      set: function(value) {
        var keepaway = 0.5;
        for (var i3 = 0; i3 < video.buffered.length; i3++) {
          if (value >= video.buffered.start(i3) && value <= video.buffered.end(i3) - keepaway) {
            return video.currentTime = value;
          }
        }
        MistVideo2.log("Seek attempted outside of buffer, but MistServer does not support seeking in progressive flash. Setting to closest available instead");
        return video.currentTime = video.buffered.length ? video.buffered.end(video.buffered.length - 1) - keepaway : 0;
      }
    });
    callback(video);
  };
  if ("flvjs" in window) {
    this.onFLVLoad();
  } else {
    var scripttag = MistUtil.scripts.insert(MistVideo2.urlappend(mistplayers.flv.scriptsrc(MistVideo2.options.host)), {
      onerror: function(e) {
        var msg = "Failed to load flv.js";
        if (e.message) {
          msg += ": " + e.message;
        }
        MistVideo2.showError(msg);
      },
      onload: MistVideo2.player.onFLVLoad
    }, MistVideo2);
  }
};

// src/index.js
function createPlayer(options) {
  if (!options || !options.target) {
    throw new Error("createPlayer requires a target element in options");
  }
  if (!options.stream) {
    throw new Error("createPlayer requires a stream name in options");
  }
  var streamName = options.stream;
  delete options.stream;
  var mv = new MistVideo(streamName, options);
  return {
    // ── Queries ──
    get video() {
      return mv.video;
    },
    get info() {
      return mv.info;
    },
    get source() {
      return mv.source;
    },
    get playerName() {
      return mv.playerName;
    },
    get logs() {
      return mv.logs;
    },
    get options() {
      return mv.options;
    },
    get currentTime() {
      return mv.api ? mv.api.currentTime : 0;
    },
    get duration() {
      return mv.api ? mv.api.duration : 0;
    },
    get volume() {
      return mv.api ? mv.api.volume : 1;
    },
    get muted() {
      return mv.api ? mv.api.muted : false;
    },
    get paused() {
      return mv.api ? mv.api.paused : true;
    },
    get playbackRate() {
      return mv.api ? mv.api.playbackRate : 1;
    },
    get loop() {
      return mv.api ? mv.api.loop : false;
    },
    get buffered() {
      return mv.api ? mv.api.buffered : null;
    },
    get fullscreen() {
      return mv.fullscreen ? mv.fullscreen.active : false;
    },
    get pip() {
      return mv.pip ? mv.pip.active : false;
    },
    get tracks() {
      if (mv.info && mv.info.meta && mv.info.meta.tracks) {
        return MistUtil.tracks.parse(mv.info.meta.tracks);
      }
      return null;
    },
    get size() {
      return mv.size || null;
    },
    get capabilities() {
      return mv.getCapabilities ? mv.getCapabilities() : {};
    },
    get quality() {
      return mv.monitor && mv.monitor.vars ? mv.monitor.vars.score : null;
    },
    get streamState() {
      return mv.state || null;
    },
    // ── Mutations ──
    play: function() {
      return mv.api ? mv.api.play() : Promise.resolve();
    },
    pause: function() {
      if (mv.api) mv.api.pause();
    },
    set currentTime(val) {
      if (mv.api) mv.api.currentTime = val;
    },
    set volume(val) {
      if (mv.api) mv.api.volume = val;
    },
    set muted(val) {
      if (mv.api) mv.api.muted = val;
    },
    set playbackRate(val) {
      if (mv.api) mv.api.playbackRate = val;
    },
    set loop(val) {
      if (mv.api) mv.api.loop = val;
    },
    set fullscreen(val) {
      if (!mv.fullscreen) return;
      if (val) mv.fullscreen.request();
      else mv.fullscreen.exit();
    },
    set pip(val) {
      if (!mv.pip) return;
      if (val) mv.pip.request();
      else mv.pip.exit();
    },
    setTrack: function(type, trackid) {
      return mv.api ? mv.api.setTrack(type, trackid) : false;
    },
    getStats: function() {
      return mv.api ? mv.api.getStats() : null;
    },
    destroy: function() {
      mv.unload("destroy() called");
    },
    reload: function() {
      mv.reload("reload() called");
    },
    nextCombo: function() {
      mv.nextCombo();
    },
    translate: function(key, fallback) {
      return mv.translate(key, fallback);
    },
    setTheme: function(themeOrTokens, mode) {
      var tokens = resolveTheme(themeOrTokens, mode);
      if (mv.container && tokens) {
        for (var name in tokens) {
          var prop = name.indexOf("--") === 0 ? name : "--mist-" + name;
          mv.container.style.setProperty(prop, tokens[name]);
        }
      }
    },
    // ── Subscriptions ──
    state: {
      on: function(prop, cb) {
        return mv.playerState.on(prop, cb);
      },
      off: function(prop, cb) {
        mv.playerState.off(prop, cb);
      },
      get: function(prop) {
        return mv.playerState.get(prop);
      }
    },
    on: function(event, callback) {
      MistUtil.event.addListener(options.target, event, callback);
    },
    off: function(event, callback) {
      options.target.removeEventListener(event, callback);
    },
    get raw() {
      return mv;
    }
  };
}
export {
  ControlChannel,
  ControlChannelAPI,
  DataChannel2WebSocket,
  MistPlayer,
  MistSkin,
  MistSkins,
  MistThemes,
  MistUI,
  MistUtil,
  MistVideo,
  PlayerState,
  createPlayer,
  locales,
  mistPlay2 as mistPlay,
  mistplayers,
  registerWrapper,
  resolveTheme
};
