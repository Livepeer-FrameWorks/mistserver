import { send, state, versions } from './api.js';
import { preloadImg } from './media.js';
import { addStream } from './streams.js';

export function refresh() {
  const command = {
    active_streams: {longform: true},
    push_list: true,
    proc_list: true,
    push_auto_list: true,
    ui_settings: true,
    enumerate_sources: "sdi:",
    capabilities: state.capabilitiesToggle ? true : "sdi:"
  };
  send(command).then(j => {

    if (j.ui_settings && j.ui_settings.thumbURL) {
      if (JSON.stringify(state.EFGconfig) != JSON.stringify(j.ui_settings)) {
        state.EFGconfig = j.ui_settings;
        state.thumbURL = state.EFGconfig.thumbURL;
        console.log("New config applied", state.EFGconfig);
      }
    }

    if ("active_streams" in j) {
      const good = [];

      for (const s in j["active_streams"]) {
        if (!j["active_streams"].hasOwnProperty(s)) { continue; }
        const strm = j["active_streams"][s];
        strm.name = s;
        if (strm.status == "Online") {
          addStream(strm);
          good.push(s);
        }
      }

      for (const c of state.sBar.children) {
        if (!good.includes(c.children[0].innerText)) {
          state.sBar.removeChild(c);
          c.querySelectorAll(".tracks > *").forEach((t) => {
            t._tooltip?.remove();
          });
        }
      }
    }

    if ("push_list" in j) {
      const reversePushes = {};
      if ("auto_push" in j) {
        for (const i in j.auto_push) {
          const ap = j.auto_push[i];
          if (ap.stream.slice(0, 4) != "#T1-") {
            if ((ap["x-LSP-notes"]?.slice(0, 4) == "#T1-") && ap.stream.includes("+")) {
              const streamData = j.active_streams?.[ap.stream];
              if (streamData) {
                const tag = ap["x-LSP-notes"].slice(1);
                if (!streamData.tags?.includes(tag)) {
                  console.log("Adding missing #" + tag + " tag for '" + ap.stream + "'");
                  send({
                    tag_stream: {
                      [ap.stream]: tag
                    }
                  }).then(() => {});
                }
                if (streamData.tags === null) { streamData.tags = []; }
                streamData.tags.push(tag);
                document.getElementById("strm_" + ap.stream).setAttribute("data-tags", streamData.tags.join(" "));
              }
            }
            continue;
          }
          reversePushes[ap.target] = ap.stream.slice(4).replaceAll("-", " ");
        }
      }

      const good = [];
      for (const s in j["push_list"]) {
        const push = j["push_list"][s];
        good.push("push_" + push[0]);

        if (!document.getElementById("push_" + push[0])) {
          const sElem = document.createElement('div');
          sElem.id = "push_" + push[0];
          sElem.className = "media_elem";
          sElem.setAttribute("data-target", push[2]);

          const p = document.createElement('p');
          sElem.appendChild(p);

          const title = document.createElement('span');
          title.innerText = push[1];
          title.onclick = () => {
            document.getElementById("strm_" + push[1]).click();
          };
          p.appendChild(title);
          title.style.cursor = "pointer";

          p.appendChild(document.createTextNode(" \u00BB " + (push[2] in reversePushes ? reversePushes[push[2]] : push[2])));
          p.title = p.innerText;

          sElem.appendChild(preloadImg());

          const trash = document.createElement('p');
          trash.innerText = "Stop output";
          trash.className = "trash titlefont button";
          trash.title = "Stop push (will auto-restart if still enabled!)";
          trash.setAttribute("data-icon", "\u267B");
          trash.onclick = () => {
            if (confirm("Are you sure you want to stop the push of " + push[1] + " to " + push[2] + "?\nIf the push is still activated, it will auto-restart shortly after.")) {
              console.log("Stopping push:", push[0]);
              send({push_stop: [push[0]]}).then(() => {
                trash.innerText = "Done.";
                setTimeout(function() { trash.innerText = "Reset"; }, 3e3);
              }).catch(() => {
                trash.innerText = "Failed";
              });
            }
          };
          sElem.appendChild(trash);

          const buttons = document.createElement("div");
          buttons.className = "button_container titlefont";
          const reset = document.createElement("p");
          reset.className = "button";
          reset.setAttribute("data-icon", "\u21A9\uFE0F");
          reset.appendChild(document.createTextNode("Reset"));
          reset.title = "Reset the decklink card for this output.";
          reset.onclick = function(e) {
            e.stopPropagation();
            if (confirm("Are you sure you want to reset the decklink card for this output?\n" + push[1] + " \u00BB " + (push[2] in reversePushes ? reversePushes[push[2]] : push[2]))) {
              send({push_reinit: [push[0]]}).then(() => {
                reset.innerText = "Done.";
                setTimeout(function() { reset.innerText = "Reset"; }, 3e3);
              }).catch(() => {
                reset.innerText = "Failed";
              });
            }
          };
          buttons.appendChild(reset);
          const nukeBtn = document.createElement("p");
          nukeBtn.className = "button";
          nukeBtn.setAttribute("data-icon", "\u2622\uFE0F");
          nukeBtn.appendChild(document.createTextNode("Nuke"));
          nukeBtn.title = "Attempt to hard-kill the output.";
          nukeBtn.onclick = function(e) {
            e.stopPropagation();
            if (confirm("Are you sure you want to NUKE this output?\n" + push[1] + " \u00BB " + (push[2] in reversePushes ? reversePushes[push[2]] : push[2]))) {
              send({push_kill: [push[0]]}).then(() => {
                nukeBtn.innerText = "Nuked.";
                setTimeout(function() { nukeBtn.innerText = "Nuke"; }, 3e3);
              }).catch(() => {
                nukeBtn.innerText = "Failed";
              });
            }
          };
          buttons.appendChild(nukeBtn);
          sElem.appendChild(buttons);

          state.tBar.appendChild(sElem);
        }
        try {
          document.getElementById("push_" + push[0]).querySelector(".thumbnail").setSrc(state.thumbURL + push[1] + ".jpg");
        } catch (e) {
          // thumbnail loading can fail silently
        }
      }
      for (const c of state.tBar.children) {
        if (!good.includes(c.id)) { state.tBar.removeChild(c); }
      }
    }

    if ("enumerate_sources" in j && Array.isArray(j["enumerate_sources"]) && j["enumerate_sources"].length > 0) {
      const deckLinkCont = document.getElementById("decklink");
      const good = [];

      function formatStatus(str) {
        if (str == "DISABLED") { return "<span class=\"disabled\">DISABLED</span>"; }
        str = str.replace(/inactive/ig, "<span class=\"inactive\">inactive</span>");
        str = str.replace(/^ingesting/i, "input ingesting");
        str = str.replace(/^playing/i, "output playing");

        const rows = str.split(", ");
        const trs = [];
        for (const row of rows) {
          const tds = [];
          const words = row.split(" ");
          const firstword = words.shift();
          switch (firstword) {
            case "input":
            case "output": {
              tds.push("<td>" + firstword + ":</td>");
              tds.push("<td>" + words.join(" ") + "</td>");
              break;
            }
            case "detected": {
              tds.push("<td></td>");
              tds.push("<td>detected " + words.slice(1) + "</td>");
              break;
            }
            default: {
              tds.push("<td colspan='2'>" + row + "</td>");
            }
          }
          trs.push("<tr>" + tds.join("") + "</tr>");
        }

        if (trs.length) return "<table><tbody>" + trs.join("") + "</tbody></table>";
        return "";
      }

      for (const s of j["enumerate_sources"]) {
        const str = s.split(" ");
        const source = str.shift();
        const parts = str.join(" ").split("(");
        const status = parts.pop().trim().slice(0, -1);
        const name = parts.join("(").trim();

        good.push("sdi_" + source);

        let sdiElem = document.getElementById("sdi_" + source);
        if (!sdiElem) {
          sdiElem = document.createElement("div");
          sdiElem.id = "sdi_" + source;
          sdiElem.className = "media_elem sdi_source";
          deckLinkCont.appendChild(sdiElem);

          const p = document.createElement("p");
          p.innerText = source;
          p.title = source;
          sdiElem.appendChild(p);

          const n = document.createElement("p");
          n.innerText = name;
          sdiElem.appendChild(n);

          const st = document.createElement("p");
          st.innerHTML = formatStatus(status);
          sdiElem.appendChild(st);
        } else {
          sdiElem.children[1].innerText = name;
          sdiElem.children[2].innerHTML = formatStatus(status);
        }
      }
      for (const c of deckLinkCont.children) {
        if (!good.includes(c.id)) { deckLinkCont.removeChild(c); }
      }
    } else {
      document.getElementById("decklink").innerText = "";
    }

    if ("proc_list" in j) {
      const streams_seen = {};
      for (const i in j.proc_list) {
        const proc = j.proc_list[i];

        if ("sink_tracks" in proc) {
          function getEncoder(proc_encoder) {
            if (!proc_encoder) return "software";
            const encoders = {
              Vaapi: ["vaapi"],
              Nvidia: ["nvidia", "nvenc", "cuda", "cuvid", "nvdec"],
              "Intel QuickSync": ["intel", "quicksync", "qsv"],
              VideoToolbox: ["videotoolbox"],
              Vulkan: ["vulkan", "vk"]
            };
            for (const e in encoders) {
              for (const str of encoders[e]) {
                if (proc_encoder.indexOf(str) >= 0) {
                  return e;
                }
              }
            }
            return "software";
          }
          const encoder = getEncoder(proc.ainfo?.encoder?.toLowerCase());

          const stream_elem = document.getElementById("strm_" + proc.sink);
          if (stream_elem) {
            for (const idx of proc.sink_tracks) {
              const track_elem = stream_elem.querySelector(".tracks [data-track-idx=\"" + idx + "\"] .encoding");
              if (track_elem) {
                track_elem.setAttribute("data-hwencode", encoder);
                track_elem.setAttribute("title", "This track was encoded with " + encoder);
              }
            }
            if (proc.sink in streams_seen) {
              streams_seen[proc.sink] = streams_seen[proc.sink].concat(proc.sink_tracks);
            } else {
              streams_seen[proc.sink] = proc.sink_tracks;
            }
          }
        }
      }

      for (const s in j["active_streams"]) {
        const stream_elem = document.getElementById("strm_" + s);
        if (stream_elem) {
          if (!(s in streams_seen)) {
            stream_elem.querySelectorAll(".tracks .encoding").forEach((e) => {
              e.removeAttribute("data-hwencode");
              e.setAttribute("title", "This is a source track");
            });
          } else {
            Array.from(stream_elem.querySelector(".tracks")?.children).forEach(function(e) {
              if (!e) return;
              if (streams_seen[s].indexOf(Number(e.getAttribute("data-track-idx"))) == -1) {
                const enc = e.querySelector(".encoding");
                if (enc) {
                  enc.removeAttribute("data-hwencode");
                  enc.setAttribute("title", "This is a source track");
                }
              }
            });
          }
        }
      }
    }

    if (("capabilities" in j) && (j.capabilities)) {
      if (j.capabilities.connectors && (j.capabilities.connectors.TSSRT || j.capabilities.connectors["TSSRT.exe"])) {
        const SRT = j.capabilities.connectors.TSSRT || j.capabilities.connectors["TSSRT.exe"];
        versions._set(0, SRT.desc.split(" ").slice(-2).join(" "));
      }
      if (j.capabilities.name == "DeckLink") {
        try {
          versions._set(1, "DeckLink " + j.capabilities.desc.split("(").pop().slice(0, -1));
        } catch (e) {}
      }
      state.capabilitiesToggle = !state.capabilitiesToggle;
    }
    state.laststats = j.active_streams;
  }).catch(() => {
    // connection error handling is already done in send()
  });
}
