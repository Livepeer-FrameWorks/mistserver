import { send, state } from './api.js';
import { preloadImg, formatBitrate, Tooltip } from './media.js';

export function addStream(strm) {
  let sElem = null;
  if (!document.getElementById("strm_" + strm.name)) {

    sElem = document.createElement('div');
    sElem.id = "strm_" + strm.name;
    sElem.className = "media_elem";

    sElem.setAttribute("data-tags", strm.tags ? strm.tags.join(" ") : "");

    const title = document.createElement('p');
    title.innerText = strm.name;
    title.setAttribute("title", strm.name);
    sElem.appendChild(title);

    sElem.appendChild(preloadImg());

    const hs = document.createElement("h5");
    hs.innerText = "Track stats:";
    sElem.appendChild(hs);
    const tracks = document.createElement("div");
    tracks.classList.add("tracks");
    sElem.appendChild(tracks);

    const packetloss = document.createElement("div");
    packetloss.classList.add("packetloss");
    sElem.appendChild(packetloss);

    const buttons = document.createElement("div");
    buttons.className = "button_container titlefont";
    const reset = document.createElement("p");
    reset.className = "button";
    reset.setAttribute("data-icon", "\u21A9\uFE0F");
    reset.appendChild(document.createTextNode("Reset"));
    reset.title = "Disconnect sessions for this stream. This will kill any currently open connections (viewers, pushes and the input).";
    reset.onclick = function(e) {
      e.stopPropagation();
      if (confirm("Are you sure you want to reset '" + strm.name + "'?\nThis will disconnect any currently open connections (viewers, pushes and the input).")) {
        send({stop_sessions: strm.name}).then(() => {
          reset.innerText = "Done.";
          setTimeout(function() { reset.innerText = "Reset"; }, 3e3);
        }).catch(() => {
          reset.innerText = "Failed";
        });
      }
    };
    buttons.appendChild(reset);
    const nuke = document.createElement("p");
    nuke.className = "button";
    nuke.setAttribute("data-icon", "\u2622\uFE0F");
    nuke.appendChild(document.createTextNode("Nuke"));
    nuke.title = "Shut down a running stream completely and/or clean up any potentially left over stream data in memory. It attempts a clean shutdown of the running stream first, followed by a forced shut down, and then follows up by checking for left over data in memory and cleaning that up if any is found.";
    nuke.onclick = function(e) {
      e.stopPropagation();
      if (confirm("Are you sure you want to NUKE '" + strm.name + "'?\nThis will shut down a running stream completely, forcefully disconnecting any viewers, pushes and the input.")) {
        send({nuke_stream: strm.name}).then(() => {
          nuke.innerText = "Nuked.";
          setTimeout(function() { nuke.innerText = "Nuke"; }, 3e3);
        }).catch(() => {
          nuke.innerText = "Failed";
        });
      }
    };
    buttons.appendChild(nuke);
    sElem.appendChild(buttons);

    function clickStream() {
      if (this._pushlist) {
        this._pushlist._remove();
        return false;
      }

      const createPushButton = (autopush) => {
        const stream = strm;

        const button = document.createElement("button");
        button.classList.add("push");
        button.classList.add("titlefont");

        const label = autopush.stream.slice(4).replaceAll("-", " ");
        button.appendChild(document.createTextNode(label));

        const tags = sElem.getAttribute("data-tags").split(" ");
        button.setAttribute("data-enabled", tags.includes(autopush.stream.slice(1)));

        button.setAttribute("title", autopush["x-LSP-notes"] ? autopush["x-LSP-notes"] : (button.getAttribute("data-enabled") == "true" ? "Enabled, click to disable" : "Disabled, click to enable"));

        button.addEventListener("click", (e) => {
          const tag = autopush.stream.slice(1);

          if (button.getAttribute("data-enabled") == "true") {
            console.log("Disabling (auto)push of", stream.name, "to", autopush.target, "by removing", autopush.stream, "tag");

            const c = {
              untag_stream: {
                [stream.name]: autopush.stream.slice(1)
              },
              stream_tags: stream.name,
              push_list: true
            };
            if (stream.name.includes("+")) {
              c.push_auto_list = true;
            } else {
              c.streams = true;
            }
            send(c).then((r1) => {
              const command = {};

              if (stream.name.includes("+")) {
                const remove = [];
                for (const id in r1.auto_push) {
                  const ap = r1.auto_push[id];
                  if ((ap["x-LSP-notes"] == autopush.stream) && (ap.stream == stream.name)) {
                    remove.push(id);
                  }
                }
                if (remove.length) {
                  command.push_auto_remove = remove;
                }
              } else {
                let tagIndex = r1.streams[stream.name].tags.indexOf(tag);
                while (tagIndex >= 0) {
                  r1.streams[stream.name].tags.splice(tagIndex, 1);
                  command.addstream = {[stream.name]: r1.streams[stream.name]};
                  tagIndex = r1.streams[stream.name].tags.indexOf(tag);
                }
              }

              if ("push_list" in r1) {
                const stop = [];
                for (const push of r1.push_list) {
                  if ((push[1] == stream.name) && (push[2] == autopush.target)) {
                    stop.push(push[0]);
                  }
                }
                if (stop.length) {
                  console.log("Stopping pushes:", stop);
                  command.push_stop = stop;
                  for (const push_id of stop) {
                    const cont = document.getElementById("push_" + push_id);
                    if (cont) {
                      const btn = cont.querySelector(".trash");
                      if (btn) {
                        btn.innerText = "Stopping..";
                      }
                    }
                  }
                }
              }

              if (Object.keys(command).length) {
                send(command).then(() => {
                  sElem.setAttribute("data-tags", r1.stream_tags[stream.name]?.join(" "));
                  clickStream.call(sElem);
                });
              } else {
                sElem.setAttribute("data-tags", r1.stream_tags[stream.name]?.join(" "));
                clickStream.call(sElem);
              }
            });

          } else {
            const alreadyActive = state.tBar.querySelector(".media_elem[data-target=\"" + autopush.target + "\"]");
            if (alreadyActive) {
              if (!confirm("A push to '" + label + "' is already running from channel '" + alreadyActive.children[0].children[0].innerText + "'!\n\nAre you sure you want to push '" + stream.name + "' there, too?")) { return; }
            }

            console.log("Enabling (auto)push of", stream.name, "to", autopush.target, "by adding", autopush.stream, "tag");

            send({
              tag_stream: {
                [stream.name]: tag
              },
              stream_tags: stream.name,
              streams: true
            }).then((r1) => {
              const finish = () => {
                sElem.setAttribute("data-tags", r1.stream_tags[stream.name]?.join(" "));
                clickStream.call(sElem);
              };

              if (stream.name.includes("+")) {
                send({
                  push_auto_add: {
                    stream: stream.name,
                    target: autopush.target,
                    "x-LSP-notes": autopush.stream
                  }
                }).then(() => {
                  finish();
                });
              } else {
                if (!r1.streams[stream.name].tags.includes(tag)) {
                  r1.streams[stream.name].tags.push(tag);
                  send({
                    addstream: {[stream.name]: r1.streams[stream.name]}
                  }).then(() => {
                    finish();
                  });
                } else {
                  finish();
                }
              }
            });
          }

          e.stopPropagation();
        });

        return button;
      };

      send({push_auto_list: true}).then(j => {
        const newList = document.createElement('div');
        newList.className = "autopush_list";
        newList._remove = function() {
          delete this.parentNode._pushlist;
          this.parentNode.removeChild(this);
        };
        if ("auto_push" in j) {
          for (const s in j["auto_push"]) {
            const ap = j["auto_push"][s];
            if (ap.stream.slice(0, 4) != "#T1-") continue;
            newList.appendChild(createPushButton(ap));
          }
        }

        if (this._pushlist) { this._pushlist._remove(); }
        this.appendChild(newList);
        this._pushlist = newList;
      });
    }
    sElem.onclick = clickStream;

    state.sBar.appendChild(sElem);
  }
  if (!sElem) sElem = document.getElementById("strm_" + strm.name);
  if (sElem) {
    sElem.querySelector(".thumbnail").setSrc(state.thumbURL + strm.name + ".jpg");

    if (strm.health) {
      const tracks = sElem.querySelector(".tracks");
      const seen = {};
      let iterator;
      if ("tracks" in strm.health) {
        iterator = strm.health.tracks;
      } else {
        iterator = Object.keys(strm.health);
      }
      for (const i of iterator) {
        if (typeof strm.health[i] == "object") {
          seen[i] = 1;

          const trackhealth = strm.health[i];
          let t = tracks.querySelector("[data-track=\"" + i + "\"]");
          const fields = {
            encoding: false
          };
          fields.jitter = function(d) {
            this.setAttribute("data-warn", d?.jitter < 300 ? "no" : "yes");
            return d?.jitter;
          };
          if (i.slice(0, 5) == "video") {
            fields.resolution = (d) => d ? [d?.width, d?.height].join("\u00D7") : undefined;
            fields.fps = function(d) {
              if (d) {
                if (d.efpks) {
                  this.setAttribute("data-warn", d.efpks == d.fpks ? "no" : (d.fpks == 0 ? "variable" : "yes"));
                  return Math.round(d.efpks / 10) / 100;
                } else {
                  this.removeAttribute("data-warn");
                  return Math.round(d.fpks / 10) / 100;
                }
              }
            };
          } else if (i.slice(0, 5) == "audio") {
            fields.channels = (d) => d?.channels;
            fields.bitrate = function(d) {
              if (!d || !d.kbits) return "";
              const [val, unit] = formatBitrate(d?.kbits * 1024);
              this.style.setProperty("--unit", "'" + unit + "'");
              return val;
            };
          }
          if (!t) {
            t = document.createElement("span");
            t.setAttribute("data-track", i);
            t.setAttribute("data-track-idx", trackhealth.idx);
            tracks.appendChild(t);
            for (const name in fields) {
              const e = document.createElement("span");
              e.classList.add(name);
              e.setAttribute("title", name);
              if (fields[name]) {
                e._set = function(d) {
                  const v = fields[name].call(e, d);
                  if (v != this.innerText) {
                    this.innerText = v;
                  }
                };
              }
              t.appendChild(e);
            }
            t._tooltip = new Tooltip(t, function() {
              const health = t._health;
              const fragment = new DocumentFragment();

              const titleEl = document.createElement("h2");
              titleEl.appendChild(document.createTextNode("Track " + health.idx + " - " + health.codec));
              fragment.appendChild(titleEl);

              const table = document.createElement("table");
              fragment.appendChild(table);
              const layout = {
                Encoder: () => [t.querySelector(".encoding")?.getAttribute("data-hwencode"), undefined, (ele) => {
                  ele.className = "encoding";
                  ele.setAttribute("data-hwencode", ele.innerHTML);
                }],
                Jitter: () => [health?.jitter, "ms", (ele) => {
                  if (health?.jitter >= 300) ele.setAttribute("data-warn", "yes");
                }],
                Channels: () => health?.channels,
                Samplerate: () => [health?.rate, "Hz"],
                Resolution: () => health?.width ? [[health.width, health.height].join("\u00D7"), "px"] : undefined,
                "Declared framerate": () => [health?.fpks / 1000, "fps", (ele) => {
                  if (("fpks" in health) && ("efpks" in health)) {
                    ele.setAttribute("data-warn", health.efpks == health.fpks ? "no" : "yes");
                  }
                }],
                "Effective framerate": () => [health?.efpks / 1000, "fps"],
                Bitrate: () => {
                  if (!health || !health.kbits) return;
                  return formatBitrate(health?.kbits * 1024);
                },
                "Keyframe interval": () => {
                  if (health?.keys?.ms_min && (i.slice(0, 5) != "audio")) {
                    if (health.keys.ms_min == health.keys.ms_max) {
                      return [health.keys.ms_min, "ms"];
                    } else {
                      return [health.keys.ms_min + "\u2013" + health.keys.ms_max, "ms"];
                    }
                  }
                }
              };
              for (const key in layout) {
                let content = layout[key]();
                let unit, apply_after;
                if (Array.isArray(content)) {
                  unit = content?.[1];
                  apply_after = content?.[2];
                  content = content[0];
                }
                if (content) {
                  const row = document.createElement("tr");
                  table.appendChild(row);
                  const l = document.createElement("td");
                  l.appendChild(document.createTextNode(key + ":"));
                  row.appendChild(l);
                  const v = document.createElement("td");
                  v.innerHTML = content;
                  if (unit) {
                    v.style.setProperty("--unit", "'" + unit + "'");
                  }
                  if (apply_after) {
                    apply_after(v);
                  }
                  row.appendChild(v);
                }
              }

              return fragment;
            }, document.body);
          }
          t._health = trackhealth;
          if (t.getAttribute("data-codec") != trackhealth.codec) {
            t.setAttribute("data-codec", trackhealth.codec);
          }
          for (const child of t.children) {
            if ("_set" in child) {
              child._set(trackhealth);
            }
          }
          if (t._tooltip.element.checkVisibility()) {
            t._tooltip.show();
          }
        }
      }
      for (let i = tracks.children.length - 1; i >= 0; i--) {
        const child = tracks.children[i];
        if (!(child.getAttribute("data-track") in seen)) {
          tracks.removeChild(child);
          child._tooltip.remove();
        }
      }
    }
    sElem.setAttribute("data-tags", strm.tags ? strm.tags.join(" ") : "");

    const packetlossVal = function(current, previous) {
      if (typeof previous == "undefined") return "";
      const packloss = current.packloss - previous.packloss;
      const packsent = current.packsent - previous.packsent;
      const total = packloss + packsent;
      if (total == 0) return 0;
      return Math.round(100 * packloss / total);
    }(strm, state.laststats[strm.name]);
    sElem.setAttribute("data-packetloss", packetlossVal);
    sElem.style.setProperty("--packetloss", packetlossVal !== undefined ? packetlossVal : 0);
    sElem.style.setProperty("--packetperc", packetlossVal !== undefined ? packetlossVal + "%" : 0);
  }
}
