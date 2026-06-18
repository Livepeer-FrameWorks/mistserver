import { APP_NAME } from '@brand';
import { pushRenderers } from './push_renderers.js';
import { uiHelpers } from '../core/ui_helpers.js';
import { editStream } from '../streams/streams.js';
import * as format from '../core/formatters.js';
import { apiClient } from '../core/api_client.js';
import { formEngine } from '../core/form_engine.js';
import { navto } from '../core/navigation.js';
import { dynamicUI } from '../core/dynamic.js';
import { uiCore } from '../core/ui_core.js';
import { copy } from '../core/clipboard.js';
import { mistHelpers } from '../core/mist_helpers.js';
import { sockets } from '../core/sockets.js';
import { el, getval } from '../core/dom_helpers.js';

const pr = pushRenderers;

export function pushes(options){
  if (!options) {
    options = {};
  }
  options = Object.assign({
    stream: false, //if stream is passed; filter the results to be only for that stream
    logs: true,
    stop_pushes: true,
    form: false,
    collapsible: false,
    show_auto: true
  },options);

  let $pushes = el("div",{class:"pushes"});
  $pushes.innerHTML = "Loading..";

  apiClient.send(function(d){
    $pushes.innerHTML = "";
    mist.data.push_list = d.push_list;

    const context_menu = new uiCore.context_menu();
    $pushes.appendChild(context_menu.ele);

    if (options.push_settings) {
      const settingsSection = el("section");
      settingsSection.appendChild(el("h3",null,"Automatic push settings"));
      settingsSection.appendChild(
        formEngine.buildUI([
          {
            type: "help",
            help: "These settings only apply to automatic pushes."
          },{
            label: 'Delay before retry',
            unit: 's',
            type: 'int',
            min: 0,
            help: 'How long the delay should be before '+APP_NAME+' retries an automatic push.<br>If set to 0, it does not retry.',
            'default': 3,
            pointer: {
              main: d.push_settings,
              index: 'wait'
            }
          },{
            label: 'Maximum retries',
            unit: '/s',
            type: 'int',
            min: 0,
            help: 'The maximum amount of retries per second (for all automatic pushes).<br>If set to 0, there is no limit.',
            'default': 0,
            pointer: {
              main: d.push_settings,
              index: 'maxspeed'
            }
          },{
            type: 'buttons',
            buttons: [{
              type: 'save',
              label: 'Save',
              'function': function(){
                apiClient.send(function(d){
                  navto('Push');
                },{
                  push_settings: d.push_settings
                })
              }
            }]
          }
        ])
      );
      $pushes.appendChild(settingsSection);
    }

    if (options.filter) {
      const filterUI = formEngine.buildUI([{
        label: "Filter the pushes below",
        classes: ["filter"],
        help: "Pushes that do not contain this text in their stream, target or notes will be hidden.",
        "function": function(e){
          const val = getval(this);
          const tables = $pushes.querySelectorAll("table[data-pushtype]");
          Array.from(tables).forEach(function(table){
            table.filter(val);
          });
        },
        css: {"margin-top":"3em"}
      }]);
      Object.assign(filterUI.style, {"margin-bottom":"0"});
      $pushes.appendChild(filterUI);
    }

    function buildPushCont(type,values) {
      let layout = {
        "": function(push){
          const cont = el("label",{class:"toggle-switch"});
          let checkbox = el("input",{type:"checkbox"});
          checkbox.checked = !push.deactivated;
          checkbox.addEventListener("change",function(){
            if (push.deactivated) {
              let o = {};
              o[push.id] = push;
              apiClient.send(function(d){
                $table.update(d.auto_push);
              },{
                push_auto_add: o
              });
            }
            else {
              push.stream = pr.DEACTIVATION_MARKER+push.stream;
              let o = {};
              o[push.id] = push;
              apiClient.send(function(d){
                $table.update(d.auto_push);
              },{
                push_auto_add: o
              });
            }
          });
          cont.appendChild(checkbox);
          cont.appendChild(el("div",{class:"slider"}));
          let sortSpan = el("span",null,push.deactivated ? "Disabled" : "Enabled");
          sortSpan.hidden = true;
          cont.appendChild(sortSpan);
          cont.addEventListener("click",function(e){
            e.stopPropagation();
          });
          return cont;
        },
        Stream: function(push){
          if (push.stream[0] == "#") return push.stream;
          const a = el("a",{class:"clickable"},push.stream);
          a.addEventListener("click",function(e){
            navto("Preview",push.stream);
            e.stopPropagation();
          });
          a.addEventListener("contextmenu",function(e){
            e.preventDefault();
            e.stopPropagation();
            const streamname = push.stream;

            const headerDiv = el("div",{class:"header"},streamname);
            const header = [headerDiv];
            let editLabel = el("span");
            if (streamname.indexOf("+") < 0) {
              editLabel.textContent = "Edit stream";
            } else {
              editLabel.innerHTML = "Edit <b>"+streamname.split("+")[0]+"</b>";
            }
            const gototabs = [
              [editLabel,function(){ editStream(streamname); },"Edit","Change the settings of this stream."],
              ["Stream status",function(){ navto("Status",streamname); },"Status","See more details about the status of this stream."],
              ["Preview stream",function(){ navto("Preview",streamname); },"Preview","Watch the stream."]
            ];

            const menu = [header];
            menu.push(gototabs);

            context_menu.show(menu,e);

          });
          return a;
        },
        Target: function(push){
          const cont = el("div");
          let t = push.target;
          if (type != "Automatic") {
            if (push.stats && push.stats.current_target) {
              t = push.stats.current_target;
            }
            else {
              t = push.resolved_target;
            }
          }
          t = t.split("?");
          let params = [];
          if (t.length > 1) params = t.pop().split("&");
          const main = t.join("?");
          const mainSpan = el("span",{title:push.target},main);
          cont.appendChild(mainSpan);
          if (params.length) {
            cont.appendChild(el("span",{class:"param"},"?"+params[0]));
            for (let i = 1; i < params.length; i++) {
              cont.appendChild(el("span",{class:"param"},"&"+params[i]));
            }
          }
          return Array.from(cont.children);
        },
        Conditions: false,
        Notes: false,
        Statistics: false,
        Actions: function(push,type) {
          const cont = el("div",{class:"buttons"});
          if (type == 'Automatic') {
            const editBtn = el("button",null,"Edit");
            editBtn.addEventListener("click",function(e){
              e.stopPropagation();
              navto('Push Config', 'auto_' + push.id);
            });
            cont.appendChild(editBtn);
          }
          const stopBtn = el("button",null,(type == 'Automatic' ? 'Remove' : 'Stop'));
          stopBtn.addEventListener("click",function(e){
            e.stopPropagation();

            if (confirm("Are you sure you want to "+this.textContent.toLowerCase()+" this push?\n"+push.stream+' to '+push.target)) {
              const stopSpan = el("span",{class:"red"},(type == 'Automatic' ? 'Removing..' : 'Stopping..'));
              this.innerHTML = "";
              this.appendChild(stopSpan);
              if (type == 'Automatic') {
                let me = this;
                apiClient.send(function(d){
                  me.textContent = "Done.";
                  $table.update(d.auto_push);
                },{push_auto_remove:push.id});
              }
              else {
                apiClient.send(function(d){
                  //done
                },{'push_stop':[push.id]});
              }
            };
          });
          cont.appendChild(stopBtn);
          if (type == "Automatic") {
            if (options.stop_pushes) {
              const spBtn = el("button",null,"Stop pushes");
              spBtn.addEventListener("click",function(e){
                e.stopPropagation();

                let msg = "Are you sure you want to stop all pushes matching\n\""+push.stream+' to '+push.target+"\"?";
                if (push.stream[0] == "#") {
                  msg = "Are you sure you want to stop all pushes to "+push.target+"\"?";
                }
                if (!push.deactivated && (d.push_settings.wait != 0)) {
                  msg += "\n\nRetrying is enabled. That means the push will probably just restart. You'll probably want to set 'Delay before retry' to 0, or disable the autopush first.";
                }

                if (confirm(msg)) {
                  let button = this;
                  button.textContent = 'Stopping pushes..';
                  const pushIds = [];
                  const push_list = mist.data.push_list;
                  for (const i in push_list) {
                    if ((push.target == push_list[i][2]) && ((push.stream[0] == "#")  || (push.stream == push_list[i][1]))) {
                      pushIds.push(push_list[i][0]);
                      let row = $pushes.querySelector('table[data-pushtype="active"] tr[data-pushid="'+push_list[i][0]+'"]');
                      if (row) {
                        const td = el("td");
                        td.setAttribute("colspan","99");
                        td.appendChild(el("span",{class:"red"},"Stopping.."));
                        row.innerHTML = "";
                        row.appendChild(td);
                      }
                    }
                  }

                  apiClient.send(function(){
                    button.textContent = 'Stop pushes';
                  },{
                    push_stop: pushIds
                  });
                }
              });
              cont.appendChild(spBtn);
            }
          }

          return cont;
        }
      };
      if (options.stream) {
        delete layout.Stream;
      }
      if (type == "Automatic") {
        layout.Conditions = function(push){
          const conditions = el("div");
          if ("scheduletime" in push) {
            conditions.appendChild(el("span",null,'schedule on '+(new Date(push.scheduletime*1e3)).toLocaleString()));
          }
          if ("completetime" in push) {
            conditions.appendChild(el("span",null,"complete on "+(new Date(push.completetime*1e3)).toLocaleString()));
          }
          if ("start_rule" in push) {
            conditions.appendChild(el("span",null,"starts if "+pr.formatCondition.apply(null,push.start_rule)));
          }
          if ("end_rule" in push) {
            conditions.appendChild(el("span",null,"stops if "+pr.formatCondition.apply(null,push.end_rule)));
          }
          return conditions.children.length ? conditions : "";
        }
        layout.Notes = function(push){
          if (("x-LSP-notes" in push) && (push["x-LSP-notes"])) {
            return push["x-LSP-notes"];
          }
          return "";
        };
      }
      else {
        delete layout[""];
        layout.Statistics = {
          create: function(){
            return el("div",{class:"statistics"});
          },
          add: {
            create: function(id){
              const labels = pr.STAT_LABELS;
              if ((options.logs) && (id == "logs")) {
                return dynamicUI.dynamic({
                  create: function(){
                    return el("div",{class:"logs"});
                  },
                  add: {
                    create: function(){
                      return el("div");
                    },
                    update: function(item){
                      this.innerHTML = format.time(item[0])+' ['+item[1]+'] '+item[2];
                    }
                  }
                });
              }
              if (id in labels) {
                const eDiv = el("div");
                eDiv.setAttribute("beforeifnotempty",labels[id]);
                return eDiv;
              }
            },
            update: function(val,allValues){
              const me = this;
              const formatting = {
                pid: function(v){ return v;},
                latency: function(v){
                  let out = el("span");
                  out.innerHTML = format.addUnit(format.number(v),"ms");
                  const unitEl = out.querySelector(".unit");
                  if (unitEl) {
                    const infoSpan = el("span",{class:"info","data-icon":"info"});
                    infoSpan.addEventListener("mouseenter",function(e){
                      const tip = el("div");
                      let h = el("h3");
                      h.innerHTML = "Latency: "+out.innerHTML;
                      tip.appendChild(h);
                      tip.appendChild(el("p",null,"This the difference between the last sent timestamp and the theoretically highest possible playback position. This is usually mostly jitter buffers."));
                      UI.tooltip.show(e,tip);
                    });
                    infoSpan.addEventListener("mouseleave",function(e){
                      UI.tooltip.hide();
                    });
                    unitEl.appendChild(infoSpan);
                  }
                  return out;
                },
                active_ms: function(v) { return format.duration(v*1e-3); },
                bytes: format.bytes,
                mediatime: function(v){ return format.duration(v*1e-3); },
                media_tx: function(v){ return format.duration(v*1e-3); },
                mediatimestamp: function(v){ return format.duration(v*1e-3); },
                tracks: function(v){ return v.join(", "); },
                pkt_retrans_count: function(v){
                  return format.number(v || 0);
                },
                pkt_loss_count: function(v){
                  return format.number(v || 0)+" ("+format.addUnit(format.number(allValues.pkt_loss_perc || 0),"%")+" over the last "+format.addUnit(5,"s")+")";
                }
              };

              if (this._id in formatting) {
                const result = formatting[this._id](val);
                if (result instanceof HTMLElement) {
                  this.innerHTML = "";
                  this.appendChild(result);
                } else {
                  this.innerHTML = result;
                }
              }

            }
          }
        }
      }

      let $cont = el("div",{onempty:"None."});
      let $table;
      if (type == "Automatic"){
        $table = dynamicUI.dynamic({
          create: function(){
            let $table = el("table",{"data-pushtype":"auto"});
            const $header = el("tr");
            const thead = el("thead");
            thead.appendChild($header);
            $table.appendChild(thead);
            $table.appendChild(el("tbody"));
            for (const i in layout) {
              if (!layout[i]) continue;
              const cell = el("th",{class:"header","data-index":i},i);
              $header.appendChild(cell);
            }
            let actionsCell = $header.querySelector('[data-index="Actions"]');
            if (actionsCell) {
              actionsCell.removeAttribute("data-index");
              actionsCell.textContent = "";
            }

            uiCore.sortableItems($table,function(sortby){
              return $table._children[this.getAttribute("data-pushid")]._children[sortby].raw;
            },{
              controls: $header,
              sortby: "Stream" in layout ? "Stream" : "Target",
              sortsave: "sort_autopushes",
              container: $table.children[1] //tbody
            });

            $table.filter = function(str){
              if (typeof str == "undefined") {
                const filterInput = $pushes.querySelector("input.filter");
                str = filterInput ? getval(filterInput) : "";
                if (!str) str = "";
              }
              str = str.toLowerCase();
              function match(value) {
                if ((typeof value == "undefined") || (value === null)) return false;
                return value.toLowerCase().indexOf(str) >= 0;
              }

              for (const i in $table._children) {
                const item = $table._children[i];
                const push = item.values;
                if (match(push.stream) || match(push.target) || match(push["x-LSP-notes"])) {
                  item.classList.remove("hidden");
                }
                else {
                  item.classList.add("hidden");
                }
              }
            };

            return $table;
          },
          add: {
            create: function(id){
              let $tr = el("tr",{"data-pushid":id});
              $tr._children = {};
              for (const i in layout) {
                if (!layout[i]) continue;
                let $td = el("td",{"data-index":i});
                $tr._children[i] = $td;
                $tr.appendChild($td);
                if (typeof layout[i] == "object" && layout[i] instanceof HTMLElement) {
                  $td.innerHTML = "";
                  $td.appendChild(layout[i].cloneNode(true));
                }
                else if (typeof layout[i] == "object") {
                  $td.dynamic = dynamicUI.dynamic(layout[i]);
                  if ($td.dynamic instanceof HTMLElement) {
                    $td.innerHTML = "";
                    $td.appendChild($td.dynamic);
                  } else {
                    $td.innerHTML = $td.dynamic;
                  }
                }
              }
              $tr.addEventListener("click",function(e){
                if (window.getSelection().toString().length) { return; }
                const push = $table._children[id].values;
                navto('Push Config', 'auto_' + push.id);
              });
              $tr.addEventListener("contextmenu",function(e){
                e.preventDefault();
                let push = $table._children[id].values;

                const $header = el("div",{class:"header"});
                if (!options.stream) {
                  $header.appendChild(el("span",null,push.stream));
                  $header.appendChild(el("span",{class:"unit"}," \u00BB "));
                }
                const targetSpan = el("span",{class:"target"});
                const targetChildren = Array.from($table._children[id]._children["Target"].children);
                targetChildren.forEach(function(child){
                  targetSpan.appendChild(child.cloneNode(true));
                });
                $header.appendChild(targetSpan);
                if (push["x-LSP-notes"]) {
                  $header.appendChild(el("div",{class:"description"},push["x-LSP-notes"]));
                }
                const actions = [];
                actions.push(["Edit",function(){
                  navto('Push Config', 'auto_' + push.id);
                },"Edit","Edit this automatic push"]);
                actions.push(["Copy target",function(){
                  const me = this;
                  const text = push.target;
                  copy(text).then(function(){
                    me._setText("Copied!")
                    setTimeout(function(){ context_menu.hide(); },300);
                  }).catch(function(e){
                    me._setText("Copy: "+e);
                    setTimeout(function(){ context_menu.hide(); },300);
                    uiHelpers.openCopyFallback({
                      title: "Copy push target",
                      error: e,
                      label: "Push target",
                      text: text
                    });
                  });
                  return false;
                },"copy","Copy the full target url to the clipboard."]);
                if (push.deactivated) {
                  actions.push(["Enable",function(){
                    let o = {};
                    o[push.id] = push;
                    apiClient.send(function(d){
                      $table.update(d.auto_push);
                      context_menu.hide();
                    },{
                      push_auto_add: o
                    });
                    return false;
                  },"wake","Enable this automatic push"]);
                }
                else {
                  actions.push(["Disable",function(){
                    push.stream = pr.DEACTIVATION_MARKER+push.stream;
                    let o = {};
                    o[push.id] = push;
                    apiClient.send(function(d){
                      $table.update(d.auto_push);
                      context_menu.hide();
                    },{
                      push_auto_add: o
                    });
			return false;
                  },"sleep","Disable this automatic push: it will not start new pushes but will remain listed"]);
                }
                actions.push(["Remove",function(){
                  if (confirm("Are you sure you want to "+this.textContent.toLowerCase()+" this push?\n"+push.stream+" to "+push.target)) {
                    this.innerHTML = "";
                    this.appendChild(el("span",{class:"red"},"Removing.."));
                    let me = this;
                    apiClient.send(function(d){
                      me.textContent = "Done.";
                      $table.update(d.auto_push);
                      context_menu.hide();
                    },{push_auto_remove:push.id});
                  }
                  return false;
                },"trash","Remove this automatic push."]);
                if (options.stop_pushes) {
                  actions.push(["Stop pushes",function(e){
                    e.stopPropagation();

                    let msg = "Are you sure you want to stop all pushes matching\n\""+push.stream+' to '+push.target+"\"?";
                    if (push.stream[0] == "#") {
                      msg = "Are you sure you want to stop all pushes to "+push.target+"\"?";
                    }
                    if (!push.deactivated && (d.push_settings.wait != 0)) {
                      msg += "\n\nRetrying is enabled. That means the push will probably just restart. You'll probably want to set 'Delay before retry' to 0, or disable the autopush first.";
                    }

                    if (confirm(msg)) {
                      let button = this;
                      const icon = button.children[0];
                      button.textContent = 'Stopping pushes..';
                      if (icon) button.insertBefore(icon, button.firstChild);
                      const pushIds = [];
                      const push_list = mist.data.push_list;
                      for (const i in push_list) {
                        if ((push.target == push_list[i][2]) && ((push.stream[0] == "#")  || (push.stream == push_list[i][1]))) {
                          pushIds.push(push_list[i][0]);
                          let row = $pushes.querySelector('table[data-pushtype="active"] tr[data-pushid="'+push_list[i][0]+'"]');
                          if (row) {
                            const td = el("td");
                            td.setAttribute("colspan","99");
                            td.appendChild(el("span",{class:"red"},"Stopping.."));
                            row.innerHTML = "";
                            row.appendChild(td);
                          }
                        }
                      }

                      apiClient.send(function(){
                        button.textContent = 'Stop pushes';
                        if (icon) button.insertBefore(icon, button.firstChild);
                        context_menu.hide();
                      },{
                        push_stop: pushIds
                      });
                    }
                    return false; //keep menu open
                  },"stop","Stop all active pushes "+(push.stream[0] == "#" ? "" : "matching '"+push.stream+"'")+" to '"+push.target+"'."]);
                  actions.push(["Stop & remove",function(e){
                    e.stopPropagation();

                    let msg = "Are you sure you want to stop all pushes matching\n\""+push.stream+' to '+push.target+"\",\n and then remove this automatic push?";
                    if (push.stream[0] == "#") {
                      msg = "Are you sure you want to stop all pushes to "+push.target+"\",\n and then remove this automatic push?";
                    }

                    if (confirm(msg)) {
                      let button = this;
                      const icon = button.children[0];
                      button.textContent = 'Stopping pushes..';
                      if (icon) button.insertBefore(icon, button.firstChild);
                      const pushIds = [];
                      const push_list = mist.data.push_list;
                      for (const i in push_list) {
                        if ((push.target == push_list[i][2]) && ((push.stream[0] == "#")  || (push.stream == push_list[i][1]))) {
                          pushIds.push(push_list[i][0]);
                          let row = $pushes.querySelector('table[data-pushtype="active"] tr[data-pushid="'+push_list[i][0]+'"]');
                          if (row) {
                            const td = el("td");
                            td.setAttribute("colspan","99");
                            td.appendChild(el("span",{class:"red"},"Stopping.."));
                            row.innerHTML = "";
                            row.appendChild(td);
                          }
                        }
                      }

                      apiClient.send(function(d){
                        button.textContent = 'Stop pushes';
                        if (icon) button.insertBefore(icon, button.firstChild);
                        $table.update(d.auto_push);
                        context_menu.hide();
                      },{
                        push_stop: pushIds,
                        push_auto_remove: push.id
                      });
                    }
                    return false;
                  },"stop_remove","Stop all active pushes "+(push.stream[0] == "#" ? "" : "matching '"+push.stream+"'")+" to '"+push.target+"' and then remove this automatic push."]);
                }

                const menu = [[$header],actions];

                context_menu.show(menu,e);
              });

              return $tr;
            },
            update: function(push){
              for (const i in layout) {
                if (typeof layout[i] == "function") {
                  const newvalue = layout[i](push,type);
                  if (newvalue != this._children[i].raw) {
                    if (newvalue instanceof HTMLElement) {
                      this._children[i].raw = newvalue.outerHTML;
                    }
                    else if (Array.isArray(newvalue)) {
                      this._children[i].raw = newvalue.map(function(n){ return n.outerHTML || n; }).join("");
                    }
                    else {
                      this._children[i].raw = newvalue;
                    }
                    if (newvalue instanceof HTMLElement) {
                      this._children[i].innerHTML = "";
                      this._children[i].appendChild(newvalue);
                    }
                    else if (Array.isArray(newvalue)) {
                      this._children[i].innerHTML = "";
                      newvalue.forEach(function(child){
                        if (child instanceof HTMLElement) {
                          this._children[i].appendChild(child);
                        } else {
                          this._children[i].innerHTML += child;
                        }
                      }.bind(this));
                    }
                    else {
                      this._children[i].innerHTML = "";
                      if (typeof newvalue === "string" || typeof newvalue === "number") {
                        this._children[i].textContent = newvalue;
                      }
                    }
                    if (i == "Notes") { this._children[i].setAttribute("title",newvalue); }
                  }
                }
              }
              const t = this.closest('table'); if (t && t.sort) t.sort();
            },
            getEntries: function(values){
              return pr.parseDeactivation(values);
            }
          },
          update: function(values){
            let $table = this;
            $table._data_values = values;

            function isColumnUsed(column) {
              for (const i in $table._children) {
                const $row = $table._children[i];
                if ($row._children[column].raw != "") {
                  return true;
                }
              }
              return false;
            }
            if (Object.keys(values).length) {
              if (!$table.parentNode) {
                $cont.appendChild($table);
              }
            }
            else if ($table.parentNode) {
              $table.parentNode.removeChild($table);
            }
            $table.sort();
            if (options.filter) $table.filter();
          },
          values: values,
          getEntries: function(d){
            let out = {};
            let streamnameisbase = false;
            if (options.stream && (options.stream.split("+").length == 1)) {
              streamnameisbase = true;
            }
            for (const i in d) {
              let values = d[i];
              values.id = i;
              const sn = values.stream.replace(pr.DEACTIVATION_MARKER,"");
              if (!options.stream || (sn == options.stream) || (!streamnameisbase && (sn.split("+")[0] == options.stream))) {
                out[values.id] = values;
              }
            }
            return out;
          }

        });
      }
      else {
        $table = dynamicUI.dynamic({
          create: function(){
            let $table = el("table",{"data-pushtype":"active"});
            const $header = el("tr");
            const thead = el("thead");
            thead.appendChild($header);
            $table.appendChild(thead);
            $table.appendChild(el("tbody"));
            for (const i in layout) {
              if (!layout[i]) continue;
              const cell = el("th",{class:"header","data-index":i},i);
              $header.appendChild(cell);
            }
            let actionsCell = $header.querySelector('[data-index="Actions"]');
            if (actionsCell) {
              actionsCell.removeAttribute("data-index");
              actionsCell.textContent = "";
            }

            let stored = mistHelpers.stored.get()
            if (!("sort_pushes" in stored)) { stored.sort_pushes = {}; }
            let $whichStat = el("select");
            const opts = [
              ["pid","Pid"],
              ["active_seconds","Time active"],
              ["bytes","Transferred data"],
              ["mediatime","Transferred media time"]
            ];
            opts.forEach(function(o){
              let opt = el("option",null,o[1]);
              opt.value = o[0];
              $whichStat.appendChild(opt);
            });
            $whichStat.value = stored.sort_pushes_statistics_type ? stored.sort_pushes_statistics_type : "pid";
            $whichStat.addEventListener("change",function(){
              mistHelpers.stored.set("sort_pushes_statistics_type",this.value);
              $table.sort("Statistics");
            });

            const statsCell = $header.querySelector('[data-index="Statistics"]');
            if (statsCell) statsCell.appendChild($whichStat);

            uiCore.sortableItems($table,function(sortby){
              if (sortby == "Statistics") {
                let stats = $table._children[this.getAttribute("data-pushid")].values;
                if (stats) stats = stats.stats;
                const which = $whichStat.value;
                if (stats && (which in stats)) { return stats[which]; }
                return null;
              }
              return $table._children[this.getAttribute("data-pushid")]._children[sortby].raw;
            },{
              controls: $header,
              sortby: "Statistics",
              sortsave: "sort_pushes",
              container: $table.children[1] //tbody
            });

            $table.filter = function(str){
              if (typeof str == "undefined") {
                const filterInput = $pushes.querySelector("input.filter");
                str = filterInput ? getval(filterInput) : "";
                if (!str) str = "";
              }
              str = str.toLowerCase();
              function match(value) {
                if ((typeof value == "undefined") || (value === null)) return false;
                return value.toLowerCase().indexOf(str) >= 0;
              }

              for (const i in $table._children) {
                const item = $table._children[i];
                const push = item.values;
                if (match(push.stream) || match(push.target) || match(push.resolved_target)) {
                  item.classList.remove("hidden");
                }
                else {
                  item.classList.add("hidden");
                }
              }
            }


            return $table;
          },
          add: {
            create: function(id){
              let $tr = el("tr",{"data-pushid":id});
              $tr._children = {};
              for (const i in layout) {
                if (!layout[i]) continue;
                let $td = el("td",{"data-index":i});
                $tr._children[i] = $td;
                $tr.appendChild($td);
                if (typeof layout[i] == "object" && layout[i] instanceof HTMLElement) {
                  $td.innerHTML = "";
                  $td.appendChild(layout[i].cloneNode(true));
                }
                else if (typeof layout[i] == "object") {
                  $td.dynamic = dynamicUI.dynamic(layout[i]);
                  if ($td.dynamic instanceof HTMLElement) {
                    $td.innerHTML = "";
                    $td.appendChild($td.dynamic);
                  } else {
                    $td.innerHTML = $td.dynamic;
                  }
                }
              }

              $tr.addEventListener("contextmenu",function(e){
                e.preventDefault();
                const push = $table._children[id].values;

                const $header = el("div",{class:"header"});
                if (!options.stream) {
                  $header.appendChild(el("span",null,push.stream));
                  $header.appendChild(el("span",{class:"unit"}," \u00BB "));
                }
                const targetSpan = el("span",{class:"target"});
                const targetChildren = Array.from($table._children[id]._children["Target"].children);
                targetChildren.forEach(function(child){
                  targetSpan.appendChild(child.cloneNode(true));
                });
                $header.appendChild(targetSpan);
                const actions = [];
                actions.push(["Stop",function(){
                  if (confirm("Are you sure you want to "+this.textContent.toLowerCase()+" this push?\n"+push.stream+" to "+push.target)) {
                    this.innerHTML = "";
                    this.appendChild(el("span",{class:"red"},"Stopping.."));
                    apiClient.send(function(d){
                      //done
                    },{'push_stop':[push.id]});
                  }
                },"trash","Stop this push."]);
                const menu = [[$header],actions];

                context_menu.show(menu,e);
              });
              return $tr;
            },
            update: function(push){
              for (const i in layout) {
                if (typeof layout[i] == "function") {
                  const newvalue = layout[i](push,type);
                  if (newvalue != this._children[i].raw) {
                    if (newvalue instanceof HTMLElement) {
                      this._children[i].innerHTML = "";
                      this._children[i].appendChild(newvalue);
                    } else if (Array.isArray(newvalue)) {
                      this._children[i].innerHTML = "";
                      newvalue.forEach(function(child){
                        if (child instanceof HTMLElement) {
                          this._children[i].appendChild(child);
                        }
                      }.bind(this));
                    } else {
                      this._children[i].innerHTML = newvalue;
                    }
                    this._children[i].raw = newvalue;
                  }
                }
                else if ((i == "Statistics") && (layout[i])) {
                  let v = {
                    pid: push.id
                  };
                  if (push.stats) { v = Object.assign(v,push.stats); }
                  v.logs = push.logs;
                  this._children[i].dynamic.update(v);
                }
              }
            }
          },
          update: function(values){
            let $table = this;
            $table._data_values = values;
            if (Object.keys(values).length) {
              if (!$table.parentNode) {
                $cont.appendChild($table);
              }
            }
            else if ($table.parentNode) {
              $table.parentNode.removeChild($table);
            }
            $table.sort();
            if (options.filter) $table.filter();
          },
          values: values,
          getEntries: function(d){
            let out = {};
            let streamnameisbase = false;
            if (options.stream && (options.stream.split("+").length == 1)) {
              streamnameisbase = true;
            }
            for (const i in d) {
              let values = mistHelpers.convertPushArr2Obj(d[i]);
              if (!options.stream || (values.stream == options.stream) || (!streamnameisbase && (values.stream.split("+")[0] == options.stream))) {
                out[values.id] = values;
              }
            }
            return out;
          }
        });
      }
      $cont.update = function(){ return $table.update.apply($table,arguments); }
      return $cont;
    }


    if (options.show_auto) {
      const autoSection = el("section");
      autoSection.appendChild(el("h3",null,"Automatic pushes"));
      const autoBtns = el("div",{class:"buttons"});
      const addAutoBtn = el("button",{"data-icon":"plus"},"Add an automatic push");
      addAutoBtn.addEventListener("click",function(){
        navto('Push Config', 'auto');
      });
      autoBtns.appendChild(addAutoBtn);
      autoSection.appendChild(autoBtns);
      autoSection.appendChild(buildPushCont("Automatic",d.auto_push));
      $pushes.appendChild(autoSection);
    }

    const pushes_container = buildPushCont("Manual",d.push_list);
    const activeSection = el("section");
    activeSection.appendChild(el("h3",null,"Active pushes"));
    const activeBtns = el("div",{class:"buttons"});
    const startBtn = el("button",{"data-icon":"plus"},"Start a push");
    startBtn.addEventListener("click",function(){
      navto('Push Config', '');
    });
    activeBtns.appendChild(startBtn);
    if (options.stop_pushes) {
      const stopAllBtn = el("button",{"data-icon":"stop"},"Stop all pushes listed below");
      stopAllBtn.addEventListener("click",function(){
        const pushIds = [];
        const table = $pushes.querySelector('table[data-pushtype="active"]');
        if (!table) return;
        const rows = Array.from(table.querySelectorAll("tr[data-pushid]")).filter(function(r){ return r.offsetParent !== null; });
        rows.forEach(function(row){
          pushIds.push(row.getAttribute("data-pushid"));
        });
        if (pushIds.length) {
          if (confirm("Are you sure you want to stop "+pushIds.length+" push"+(pushIds.length > 1 ? "es?" : "?"))) {
            rows.forEach(function(row){
              const td = el("td");
              td.setAttribute("colspan","99");
              td.appendChild(el("span",{class:"red"},"Stopping.."));
              row.innerHTML = "";
              row.appendChild(td);
            });
            apiClient.send(function(){
              //done
            },{
              push_stop: pushIds
            });
          }
        }
      });
      activeBtns.appendChild(stopAllBtn);
    }
    activeSection.appendChild(activeBtns);
    activeSection.appendChild(pushes_container);
    $pushes.appendChild(activeSection);

    sockets.http.api.subscribe(function(d){
      pushes_container.update(d.push_list);
      mist.data.push_list = d.push_list;
    },{push_list:1});

    if (options.collapsible) {
      Array.from($pushes.children).forEach(function(section){
        if (section.tagName === "SECTION" && !section.classList.contains("context_menu")) {
          section.classList.add("collapsible");
          section.classList.add("expanded");
          const firstChild = section.children[0];
          if (firstChild) {
            firstChild.addEventListener("click",function(){
              section.classList.toggle("expanded");
            });
          }
        }
      });
    }


  },{push_auto_list:1,push_list:1,push_settings:1});

  const outerSection = el("section",{class:"pushes"});
  outerSection.appendChild(el("h3",null,"Pushes and recordings"));
  outerSection.appendChild($pushes);
  return outerSection;
}
