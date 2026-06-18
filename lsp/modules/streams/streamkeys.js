import { uiHelpers } from '../core/ui_helpers.js';
import { editStream } from './streams.js';
import { apiClient } from '../core/api_client.js';
import { formEngine } from '../core/form_engine.js';
import { navto } from '../core/navigation.js';
import { tabView } from '../core/tab_view.js';
import { uiCore } from '../core/ui_core.js';
import { copy } from '../core/clipboard.js';
import { streamHints } from './stream_hints.js';
import { el, getval, setval } from '../core/dom_helpers.js';

export function streamkeys(streamname,onsave,opts){
  opts = opts || {};
  const pageMode = !!opts.pageMode;

  if (!onsave) {
    onsave = function(){
      tabView.showTab("Stream keys");
    }
  }

  let $c = el('section', {class: 'streamkeys-module'});
  $c.classList.toggle('streamkeys-page', pageMode);
  $c.textContent = 'Loading..';
  apiClient.send(function(d){ //request current stream keys + active streams
    const saveas = {};
    const streamkeysData = (d.streamkeys && (typeof d.streamkeys == "object")) ? d.streamkeys : {};
    let current_streams = Object.assign({},mist.data.streams);
    if (d.active_streams) {
      for (const streamname of d.active_streams) {
        const streambase = streamname.split("+")[0];

        if (streambase in mist.data.streams) {
          if ((streambase != streamname) && !(streamname in current_streams)) {
            //it's a new wildcard stream
            current_streams[streamname] = Object.assign({},mist.data.streams[streambase]);
            current_streams[streamname].name = streamname;
          }
          current_streams[streamname].online = 1;
        }
      }
    }

    function normalizeStreamForField(fullStream) {
      const full = String(fullStream || '');
      if (!streamname) { return full; }
      const parts = full.split('+');
      if (parts[0] !== streamname) { return null; }
      return parts.slice(1).join('+');
    }
    function listPushStreamSuggestions() {
      const out = [];
      const seen = {};
      const streams = Object.keys(current_streams).sort();
      for (let i = 0; i < streams.length; i++) {
        const full = streams[i];
        if (current_streams[full].source?.slice(0,7) != "push://") { continue; }
        const value = normalizeStreamForField(full);
        if (value === null || value === '') { continue; }
        if (seen[value]) { continue; }
        seen[value] = 1;
        out.push({
          value: value,
          label: value,
          search: full
        });
      }
      return out;
    }
    function listQuickPickSuggestions() {
      const scores = {};
      function addScore(fullStream, amount) {
        const value = normalizeStreamForField(fullStream);
        if (value === null || value === '') { return; }
        scores[value] = (scores[value] || 0) + amount;
      }
      for (const key in streamkeysData) {
        addScore(streamkeysData[key], 2);
      }
      for (let i = 0; i < (d.active_streams || []).length; i++) {
        addScore(d.active_streams[i], 3);
      }
      return Object.keys(scores).sort(function(a,b){
        return scores[b] - scores[a] || a.localeCompare(b);
      }).slice(0, 6).map(function(value){
        return {
          value: value,
          label: value
        };
      });
    }

    const streamFieldCfg = {
        label: "For stream name",
        type: "str",
        pointer: { main: saveas, index: "stream" },
        validate: streamname ? [] : ["required","streamname_with_wildcard",function(val){
          //notify if this stream does not exist
          if (!(val in current_streams)) {
            return {
              msg: "This stream does not exist (yet). You can add a key for it anyway.",
              "break": false
            };
          }
          //warn if this stream exists but does not have a push:// source
          if (current_streams[val].source?.slice(0,7) != "push://") {
            return {
              msg: "It is not possible to push into this stream. Pushing to this stream will only work if it is not already active.",
              classes: ["orange"],
              "break": false
            };
          }

        }],
        help: "Enter the stream for which this stream key will be valid. You can enter a stream with wildcard. You can add stream keys for streams that do not exist yet.",
        datalist: function(){
          let out = [];
          for (const stream in current_streams) {
            if (current_streams[stream].source?.slice(0,7) == "push://") {
              if (streamname) {
                if (stream.split("+")[0] == streamname) {
                  out.push(stream.split("+").slice(1).join("+"));
                }
              }
              else {
                out.push(stream);
              }
            }
          }
          return out;
        }()
      };
    if (streamname) {
      streamFieldCfg.prefix = streamname+"+";
    }

    const streamSuggestions = listPushStreamSuggestions();
    const quickSuggestions = listQuickPickSuggestions();
    const streamPickerCfg = (streamSuggestions.length || quickSuggestions.length) ? {
      type: "custom",
      label: streamname ? "Stream suffix hints" : "Stream hints",
      help: "Search configured push streams and click one to fill the stream name field.",
      build: function(){
        const streamInput = streamFieldCfg._field;
        if (!(streamInput instanceof HTMLElement)) {
          return el('div');
        }
        const picker = uiHelpers.createSmartSelect({
          label: streamname ? "Wildcard suffix" : "Stream",
          emptyLabel: streamname ? "Select a wildcard suffix..." : "Select a stream...",
          searchPlaceholder: streamname ? "Type to filter wildcard suffixes..." : "Type to filter streams...",
          quickLabel: streamname ? "Recent suffixes" : "Top streams by keys",
          maxQuickPicks: 6,
          onChange: function(value) {
            setval(streamInput, value, ['stream_hint']);
            streamInput.dispatchEvent(new Event('input', {bubbles: true}));
            streamInput.dispatchEvent(new Event('change', {bubbles: true}));
          }
        });
        picker.setOptions(streamSuggestions);
        picker.setQuickPicks(quickSuggestions);
        picker.setValue(getval(streamInput), {silent: true});

        function syncPickerValue() {
          picker.setValue(getval(streamInput), {silent: true});
        }
        streamInput.addEventListener('input', syncPickerValue);
        streamInput.addEventListener('change', syncPickerValue);

        return picker.element;
      }
    } : false;

    const $addSection = formEngine.buildUI([el('h3', null, 'Add stream key(s)'),streamFieldCfg,streamPickerCfg,{
        label: "Stream key(s)",
        type: "inputlist",
        classes: ["streamkeysinputlist"],
        pointer: { main: saveas, index: "keys" },
        validate: ["required"],
        help: "Enter one or more keys",
        input: {
          type: "str",
          clipboard: true,
          maxlength: 256,
          validate: [function(val,me){
            if (val in streamkeysData) {
              //duplicates in the current field do not need to be tested - they're all for the same stream so it won't be an issue
              return {
                msg: "The key '"+val+"' is already in use. Duplicates are not allowed.",
                classes: ["red"]
              };
            }
            if (val.length && !val.match(/^[0-9a-z]+$/i)) {
              return {
                msg: "The key '"+val+"' contains special characters. We recommend not using these as some video streaming protocols do not accept them.",
                classes: ["orange"],
                "break": false
              }
            }
          }],
          unit: el('button', {
            onclick: function(){
              let $field = this.closest(".field_container").querySelector(".field");

              function apply() {
                setval($field, uiHelpers.randomKey(32));
                if ($field._validate($field)) {
                  apply();
                }
              }
              apply();

              $field.dispatchEvent(new Event('keyup',{bubbles:true}));
            }
          }, "Generate")
        }
      },{
        type: "buttons",
        buttons: [{
          type: "save",
          icon: "plus",
          label: "Add",
          "function": function(){
            let send = {};
            for (const key of saveas.keys) {
              send[key] = streamname ? (saveas.stream ? streamname+"+"+saveas.stream : streamname) : saveas.stream;
            }
            apiClient.send(function(){
              onsave();
            },{streamkey_add:send});
          }
        }]
      }]);
    $addSection.classList.add('streamkeys-add');

    let $streamkeysRows = null;
    let $streamkeysSummary = el('span', {}, "0 keys across 0 streams.");
    let $streamkeysEmpty = el('div', {class: 'streamkeys-empty'}, "No stream keys found.");

    function listRows(){
      let rows = [];
      for (const key in streamkeysData) {
        const stream = streamkeysData[key];
        if (streamname && (stream.split("+")[0] != streamname)) {
          continue;
        }
        rows.push({
          key: key,
          stream: stream,
          base: stream.split("+")[0]
        });
      }
      rows.sort(function(a,b){
        return a.stream.localeCompare(b.stream) || a.key.localeCompare(b.key);
      });
      return rows;
    }
    function streamKind(stream){
      let kind = "Unconfigured";
      const base = stream.split("+")[0];
      if (base in current_streams) {
        const source = current_streams[base].source || "";
        if (source.slice(0,7) != "push://") {
          kind = "Overrides source";
        }
        else if (source.slice(0,16) == "push://invalid,host") {
          kind = "Key only";
        }
        else {
          kind = "Configured";
        }
      }
      return kind;
    }

    const $listSection = formEngine.buildUI([el('h3', null, 'Current stream keys'+(streamname ? " for '"+streamname+"'" : '')),{
        type: "help",
        help: $streamkeysSummary
      },{
        type: "help",
        help: "Note: the stream status on this page does not update automatically."
      },{
        label: "Search streams and keys",
        help: "Only rows that contain this text are shown. Pagination applies to rows.",
        "function": function(){
          $streamkeysRows?.filter?.(getval(this));
        },
        css: { marginBottom: "1em" }
      },function(){
        const $table = el('table', {class: 'streamkeys-table'});
        const $thead = el('thead');
        const headerRow = el('tr');
        headerRow.appendChild(el('th', {}, "Status"));
        headerRow.appendChild(el('th', {}, "Stream"));
        headerRow.appendChild(el('th', {}, "Key"));
        headerRow.appendChild(el('th', {}, "Type"));
        headerRow.appendChild(el('th', {}, "Actions"));
        $thead.appendChild(headerRow);

        const $tbody = el('tbody');
        $streamkeysRows = $tbody;

        function updateSummary(){
          const allRows = listRows();
          const streams = {};
          for (const row of allRows) {
            streams[row.stream] = 1;
          }
          const streamCount = Object.keys(streams).length;
          $streamkeysSummary.textContent = allRows.length+" key"+(allRows.length == 1 ? "" : "s")+" across "+streamCount+" stream"+(streamCount == 1 ? "" : "s")+".";
        }
        function updateEmptyState(){
          if (!$streamkeysRows) {
            $streamkeysEmpty.hidden = false;
            return;
          }
          const hasVisible = !!$streamkeysRows.querySelector("tr:not(.hidden)");
          $streamkeysEmpty.hidden = hasVisible;
        }
        function refreshPaging(resetToFirst){
          if ($tbody.show_page) {
            if (resetToFirst) {
              $tbody.show_page(1);
            }
            else {
              $tbody.show_page();
            }
          }
          updateEmptyState();
        }
        function streamKeys(stream){
          let keys = [];
          for (const key in streamkeysData) {
            if (streamkeysData[key] == stream) {
              keys.push(key);
            }
          }
          keys.sort();
          return keys;
        }
        function formatStream(stream){
          const $cont = el('span', {class: 'clickable stream-name'}, stream);
          if (stream.indexOf("+") >= 0) {
            const split = stream.split("+");
            $cont.innerHTML = '';
            $cont.appendChild(el('span', {class: 'wildparent'}, split[0]+"+"));
            $cont.appendChild(document.createTextNode(split.slice(1).join('')));
          }
          $cont.addEventListener("contextmenu",function(e){
            e.preventDefault();

            const headerDiv = el('div', {class: 'header'});
            headerDiv.innerHTML = $cont.innerHTML;
            const header = [headerDiv];

            const editSpan1 = el('span');
            editSpan1.innerHTML = "Edit "+(stream.indexOf("+") < 0 ? "stream" : "<b>"+stream.split("+")[0]+"</b>");
            const editSpan2 = el('span');
            editSpan2.innerHTML = "Create "+(stream.indexOf("+") < 0 ? "stream" : "<b>"+stream.split("+")[0]+"</b>");

            const gototabs = [
              [editSpan1,function(){ editStream(stream); },"Edit","Change the settings of this stream."],
              ["Stream status",function(){ navto("Status",stream); },"Status","See more details about the status of this stream."],
              ["Preview stream",function(){ navto("Preview",stream); },"Preview","Watch the stream."]
            ];
            if (!(stream.split("+")[0] in mist.data.streams)) {
              gototabs.shift();
              gototabs.unshift([editSpan2,function(){ editStream(stream); },"Edit","Create this stream."]);
            }

            const menu = [header];
            menu.push(gototabs);
            context_menu.show(menu,e);

          });
          $cont.addEventListener("click",function(e){
            this.dispatchEvent(new MouseEvent("contextmenu",e));
            e.stopPropagation();
          });
          return $cont;
        }
        function keyActions(stream,key,e){
          let raw = streamHints.updateLiveStreamHint(stream,"push://invalid,host","raw",false,[key]);
          function createCopyEntry(label,text) {
            return [
              "Copy "+label,function(){
                copy(text).then(()=>{
                  this._setText("Copied!");
                  setTimeout(function(){ context_menu.hide(); },300);
                }).catch((err)=>{
                  this._setText("Copy: "+err);
                  setTimeout(function(){ context_menu.hide(); },300);
                  uiHelpers.openCopyFallback({
                    title: "Copy to clipboard",
                    error: err,
                    label: "Text",
                    text: text
                  });
                });
              },"copy","Copy the "+label+" to the clipboard:\n"+text
            ];
          }

          let fields = [];
          fields.push(createCopyEntry("key",key));
          for (const label in raw) {
            if (Array.isArray(raw[label])) {
              fields.push(createCopyEntry(label+" url",raw[label][0]));
            }
            else if (label == "RTMP") {
              fields.push(createCopyEntry("full RTMP url",raw.RTMP.full_url[0]));
              fields.push(createCopyEntry("RTMP url (without key)",Object.keys(raw.RTMP.pairs)[0]));
            }
          }

          const headerDiv = el('div', {class: 'header'});
          const keyDiv = el('div', {style: 'font-family: monospace'}, key);
          const forDiv = el('div', {}, "for ");
          const streamElem = formatStream(stream);
          streamElem.classList.remove("clickable");
          forDiv.appendChild(streamElem);
          headerDiv.appendChild(keyDiv);
          headerDiv.appendChild(forDiv);

          context_menu.show([
            [headerDiv],
            fields
          ],e);
        }
        function deleteKey(stream,key){
          if (!confirm("Are you sure you want to remove the key '"+key+"'?")) {
            return;
          }
          apiClient.send(function(){
            delete streamkeysData[key];
            Array.from($tbody.children).filter(function(r){
              return r.getAttribute("data-key") == key;
            }).forEach(function(r){ r.remove(); });
            updateSummary();
            refreshPaging(false);
          },{
            streamkey_del: key
          });
        }
        function deleteStream(stream){
          const keys = streamKeys(stream);
          if (!keys.length) {
            return;
          }
          if (!confirm("Are you sure you want to remove all "+keys.length+" keys of the stream '"+stream+"'?")) {
            return;
          }
          apiClient.send(function(){
            for (const key of keys) {
              delete streamkeysData[key];
            }
            Array.from($tbody.children).filter(function(r){
              return r.getAttribute("data-stream") == stream;
            }).forEach(function(r){ r.remove(); });
            updateSummary();
            refreshPaging(false);
          },{
            streamkey_del: keys
          });
        }

        function renderRows(){
          $tbody.innerHTML = '';
          const rows = listRows();
          $streamkeysEmpty.hidden = !!rows.length;
          for (const row of rows) {
            const active = d.active_streams?.indexOf(row.stream) >= 0;

            const statusDiv = el('div', {
              'data-streamstatus': active ? 4 : 0,
              title: active ? "Active" : "Inactive"
            });

            const keySpan = el('span', {class: 'clickable key-value'}, row.key);
            keySpan.addEventListener("contextmenu",function(e){
              e.preventDefault();
              keyActions(row.stream,row.key,e);
            });
            keySpan.addEventListener("click",function(e){
              this.dispatchEvent(new MouseEvent("contextmenu",e));
              e.stopPropagation();
            });

            const copyBtn = el('button', {
              'data-icon': 'copy',
              title: 'Copy key',
              onclick: () => copy(row.key)
            }, "Copy");

            const deleteBtn = el('button', {
              'data-icon': 'trash',
              title: 'Delete key',
              onclick: () => deleteKey(row.stream,row.key)
            }, "Delete");

            const deleteStreamBtn = el('button', {
              'data-icon': 'trash',
              title: 'Delete all keys for this stream',
              onclick: () => deleteStream(row.stream)
            }, "Delete stream keys");

            const $tr = el('tr', {
              'data-stream': row.stream,
              'data-key': row.key
            });
            $tr.appendChild(el('td', {}, statusDiv));
            $tr.appendChild(el('td', {}, formatStream(row.stream)));
            $tr.appendChild(el('td', {}, keySpan));
            $tr.appendChild(el('td', {}, streamKind(row.stream)));
            const actionsTd = el('td');
            actionsTd.appendChild(copyBtn);
            actionsTd.appendChild(deleteBtn);
            actionsTd.appendChild(deleteStreamBtn);
            $tr.appendChild(actionsTd);

            $tbody.appendChild($tr);
          }
          updateSummary();
        }

        $tbody.filter = function(str){
          str = (str || "").toLowerCase();
          for (const row of this.children) {
            const matches = (row.getAttribute("data-stream").toLowerCase().indexOf(str) >= 0)
              || (row.getAttribute("data-key").toLowerCase().indexOf(str) >= 0);
            row.classList.toggle("hidden",!matches);
          }
          refreshPaging(true);
        };

        renderRows();

        $table.appendChild($thead);
        $table.appendChild($tbody);
        const scrollDiv = el('div', {class: 'table-scroll'});
        scrollDiv.appendChild($table);
        scrollDiv.appendChild($streamkeysEmpty);
        return scrollDiv;
      }()]);
    $listSection.classList.add('streamkeys-list');
    if ($streamkeysRows) {
      const $pageControl = uiCore.pagecontrol($streamkeysRows,25);
      const pageControlEl = $pageControl;
      pageControlEl.classList.add('streamkeys-page-control');
      const titleSpan = el('span', {class: 'streamkeys-page-control-title'}, "Pagination");
      pageControlEl.insertBefore(titleSpan, pageControlEl.firstChild);
      const jumpTo = pageControlEl.querySelector('.jump_to');
      if (jumpTo) {
        const jumpContainer = jumpTo.closest('.input_container');
        if (jumpContainer) jumpContainer.remove();
      }
      const selectEl = pageControlEl.querySelector('select');
      if (selectEl) {
        const selectContainer = selectEl.closest('.input_container');
        if (selectContainer) {
          const labelSpan = selectContainer.querySelector(':scope > span');
          if (labelSpan) labelSpan.textContent = 'Rows per page';
        }
      }
      $listSection.appendChild(pageControlEl);
    }

    $c.innerHTML = '';
    if (!pageMode) {
      const h1 = el('h1', null, 'Manage stream keys'+(streamname ? " for '"+streamname+"'" : ''));
      h1.style.marginTop = '0';
      $c.appendChild(h1);
    }
    $c.appendChild(formEngine.buildUI([{
      type: "help",
      help: "Stream keys are a method to bypass all security and allow an incoming push for the given stream. If a token that matches a stream is used it will be accepted. This will even apply to vod or unconfigured streams, making them active as a live stream with default settings. <br>Note: A stream with source 'push://' and a stream key would accept both its stream name and stream key for input. To avoid this enable the 'Require stream key' option."
    },$addSection,$listSection
    ]));

    const context_menu = new uiCore.context_menu();
    $c.appendChild(context_menu.ele);

    const firstField = $c.querySelector('.field');
    if (firstField) firstField.focus();
  },{ streamkeys: true, active_streams: true, capabilities: true });
  return $c;
};
