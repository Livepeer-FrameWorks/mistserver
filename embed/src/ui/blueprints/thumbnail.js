export var ThumbnailPreview = {
  parseVTT: function(text) {
    var cues = [];
    var blocks = text.replace(/\r\n/g, "\n").split("\n\n");
    for (var i = 0; i < blocks.length; i++) {
      var lines = blocks[i].trim().split("\n");
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
            cues.push({ start: start, end: end, url: url });
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
    for (var i = 0; i < cues.length; i++) {
      if (time >= cues[i].start && time < cues[i].end) {
        return cues[i];
      }
    }
    return null;
  },

  parseFragment: function(url) {
    // Parse url#xywh=x,y,w,h or just plain url
    var hash = url.indexOf("#");
    if (hash === -1) return { src: url, x: 0, y: 0, w: 0, h: 0, isSprite: false };
    var src = url.substring(0, hash);
    var frag = url.substring(hash + 1);
    var match = frag.match(/xywh=(\d+),(\d+),(\d+),(\d+)/);
    if (!match) return { src: src, x: 0, y: 0, w: 0, h: 0, isSprite: false };
    return {
      src: src,
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
