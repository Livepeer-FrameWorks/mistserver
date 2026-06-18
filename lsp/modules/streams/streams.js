import { registerTab } from '../core/tab_registry.js';
import { AppShell } from '../core/appshell.js';
import { register } from '../core/mode_dispatch.js';
import * as format from '../core/formatters.js';
import { uiHelpers } from '../core/ui_helpers.js';
import { stream } from './stream_utils.js';
import { streamScenarios } from './stream_scenarios.js';
import { apiClient } from '../core/api_client.js';
import { formEngine } from '../core/form_engine.js';
import { navto } from '../core/navigation.js';
import { dynamicUI } from '../core/dynamic.js';
import { uiCore } from '../core/ui_core.js';
import { findInput } from '../core/capabilities.js';
import { sockets } from '../core/sockets.js';
import { streamHints } from './stream_hints.js';
import { mistHelpers } from '../core/mist_helpers.js';
import { el, getval, setval } from '../core/dom_helpers.js';
import { defineSection } from '../core/section_registry.js';

defineSection({
  id: 'streams/list',
  page: 'Streams',
  title: 'Streams',
  subtitle: 'Manage and monitor all configured streams',
  keywords: ['stream list', 'configured streams', 'wildcard', 'folder stream', 'source', 'viewers', 'inputs', 'outputs', 'online', 'offline'],
  requires: ['streams'],
  navTo: { tab: 'Streams', other: '' }
});

defineSection({
  id: 'streams/edit',
  page: 'Streams',
  title: 'Stream Configuration',
  subtitle: 'Create or edit stream settings',
  keywords: ['create stream', 'edit stream', 'source URI', 'RTMP ingest', 'SRT ingest', 'WebRTC ingest', 'RTSP', 'file', 'folder', 'always on', 'processes', 'transcoding', 'recording', 'composer', 'stream keys', 'access'],
  requires: ['capabilities'],
  navTo: { tab: 'Streams', other: '' }
});

function streamsPage(tab, other, prev, $main, $pageHeader) {
  if (!('capabilities' in mist.data)) {
    $main.innerHTML = 'Loading..';
    apiClient.send(function(){
      navto(tab,other);
    },{capabilities: true, streamkeys: true});
    return;
  }


  const stored = mistHelpers.stored.get();
  let sortstreams = {
    by: "name",
    dir: 1
  };
  let pagesize;
  if (other == '') {
    if ('viewmode' in stored) {
      other = stored.viewmode;
    } else if (AppShell.getMode() === 'guided') {
      other = 'thumbnails';
    }
  }
  if ('sortstreams' in stored) {
    sortstreams = stored.sortstreams;
  }
  if ('streams_pagesize' in stored) {
     pagesize = stored.streams_pagesize;
  }



  let $streams;

  var WILD_STORAGE_KEY = 'wild_expanded';
  var wildExpanded = {};
  try {
    var raw = sessionStorage.getItem(WILD_STORAGE_KEY);
    if (raw) wildExpanded = JSON.parse(raw);
  } catch(e) {}
  function isWildExpanded(groupKey) { return !!wildExpanded[groupKey]; }
  function toggleWildGroup(groupKey) {
    var nowExpanded = !isWildExpanded(groupKey);
    if (nowExpanded) wildExpanded[groupKey] = true;
    else delete wildExpanded[groupKey];
    try { sessionStorage.setItem(WILD_STORAGE_KEY, JSON.stringify(wildExpanded)); } catch(e) {}
    var children = $streams.querySelectorAll('[data-wildgroup="' + groupKey + '"]');
    for (var i = 0; i < children.length; i++) {
      children[i].classList.toggle('wild-collapsed', !nowExpanded);
    }
    var parentEl = $streams.querySelector('[data-wildparent="' + groupKey + '"]');
    if (parentEl) parentEl.classList.toggle('wild-expanded', nowExpanded);
    if ($streams.show_page) $streams.show_page();
  }

  const isThumbnails = (other === 'thumbnails');

  const viewToggle = el('button', {class: 'header-toggle view-toggle', type: 'button', title: isThumbnails ? 'Thumbnail view' : 'List view', 'aria-label': isThumbnails ? 'Switch to list view' : 'Switch to thumbnail view'});
  if (!isThumbnails) viewToggle.classList.add('toggled');

  const iconLeftOutside = el('span', {class: 'toggle-icon toggle-icon-left toggle-icon-outside', 'data-icon': 'grid', 'aria-hidden': 'true'});
  const iconLeftIntrack = el('span', {class: 'toggle-icon toggle-icon-left toggle-icon-intrack', 'data-icon': 'grid', 'aria-hidden': 'true'});
  const toggleThumb = el('span', {class: 'toggle-thumb'});
  const iconRightIntrack = el('span', {class: 'toggle-icon toggle-icon-right toggle-icon-intrack', 'data-icon': 'list', 'aria-hidden': 'true'});
  const toggleTrack = el('span', {class: 'toggle-track', 'aria-hidden': 'true'});
  toggleTrack.appendChild(iconLeftIntrack);
  toggleTrack.appendChild(toggleThumb);
  toggleTrack.appendChild(iconRightIntrack);
  const iconRightOutside = el('span', {class: 'toggle-icon toggle-icon-right toggle-icon-outside', 'data-icon': 'list', 'aria-hidden': 'true'});

  viewToggle.appendChild(iconLeftOutside);
  viewToggle.appendChild(toggleTrack);
  viewToggle.appendChild(iconRightOutside);

  viewToggle.addEventListener('click', function() {
    const newMode = isThumbnails ? 'list' : 'thumbnails';
    mistHelpers.stored.set('viewmode', newMode);
    navto('Streams', newMode);
  });

  const manageKeysBtn = el('button', {'data-icon': 'key'}, 'Stream keys');
  manageKeysBtn.addEventListener('click', function(){ navto("Stream keys"); });

  const createStreamBtn = el('button', {'data-icon': 'plus', class: 'stream-primary-action'}, 'New stream');
  createStreamBtn.addEventListener('click', function(){ showEditStreamPopup(''); });

  uiHelpers.appendPageActions($pageHeader, [viewToggle, manageKeysBtn, createStreamBtn]);

  const $form = formEngine.buildUI([
    {
      type: 'help',
      classes: ['page-intro'],
      help: "Streams are your media sources - live ingest via RTMP/SRT/WebRTC, IP cameras via RTSP, local files for VoD, or folders for wildcard delivery. Each stream can have processes attached for transcoding, AI analytics, or recording."
    },
    {
      label: "Filter streams",
      classes: ["filter"],
      help: "Stream names that do not contain the text you enter here will be hidden.",
      "function": function(e){
        const val = getval(this);
        if ($streams) $streams.filter(val);
      },
      css: {"margin-top":"3em"}
    }
  ]);

  let statusFilter = 'all';
  const statusFilterEl = el('span', {class: 'streams-status-filter'});
  ['All', 'Online', 'Offline'].forEach(function(label) {
    const val = label.toLowerCase();
    const btn = el('button', null, label);
    btn.addEventListener('click', function() {
      statusFilter = val;
      const btns = statusFilterEl.querySelectorAll('button');
      for (let b = 0; b < btns.length; b++) { btns[b].classList.remove('active'); }
      this.classList.add('active');
      applyStatusFilter();
    });
    if (val === 'all') btn.classList.add('active');
    statusFilterEl.appendChild(btn);
  });
  const filterField = $form.querySelector('.field.filter');
  if (filterField && filterField.parentNode) {
    filterField.parentNode.insertBefore(statusFilterEl, filterField.nextSibling);
  }

  function applyStatusFilter() {
    if (!$streams) return;
    for (let i = 0; i < $streams.children.length; i++) {
      const item = $streams.children[i];
      if (statusFilter === 'all') {
        item.classList.remove('status-hidden');
        continue;
      }
      const id = item.getAttribute('data-id');
      const d = current_streams[id];
      const isOnline = d && d.stats && d.stats[1] >= 1 && d.stats[1] <= 4;
      if ((statusFilter === 'online' && isOnline) || (statusFilter === 'offline' && !isOnline)) {
        item.classList.remove('status-hidden');
      } else {
        item.classList.add('status-hidden');
      }
    }
    if ($streams.show_page) $streams.show_page();
  }

  const emptyState = el('div', {class: 'streams-empty-state'});
  emptyState.appendChild(el('div', {'data-icon': 'upload'}));
  emptyState.appendChild(el('h3', null, 'No streams configured'));
  emptyState.appendChild(el('p', null, 'Create your first stream to start delivering media.'));
  const emptyCreateBtn = el('button', {'data-icon': 'plus'}, 'Create a stream');
  emptyCreateBtn.addEventListener('click', function(){ showEditStreamPopup(''); });
  emptyState.appendChild(emptyCreateBtn);
  const emptyHint = el('p', {class: 'empty-header-hint'});
  emptyHint.appendChild(el('span', {'data-icon': 'arrow-up-right'}));
  emptyHint.appendChild(el('span', null, 'You can also use New stream or Stream keys at the top.'));
  emptyState.appendChild(emptyHint);

  function checkEmpty() {
    const hasStreams = !!(current_streams && Object.keys(current_streams).length);
    if (hasStreams) {
      emptyState.hidden = true;
      emptyState.style.display = 'none';
      $form.hidden = false;
    } else {
      emptyState.hidden = false;
      emptyState.style.display = 'flex';
      $form.hidden = true;
    }
  }

  $main.classList.add('page-body--flex-col', 'streams-page-body', 'streams-page-shell', 'slab-shell', 'slab-shell--seamed');
  const streamsShell = $main;
  streamsShell.appendChild($form);
  streamsShell.appendChild(emptyState);


  const current_streams = Object.assign({},mist.data.streams);
  const context_menu = new uiCore.context_menu();

  if (other == "thumbnails") {
    $streams = dynamicUI.dynamic({
      create: function(){
        const cont = document.createElement("div");
        cont.className = "streams thumbnails";

        uiCore.sortableItems(cont,function(sortby){
          return this.sortValues[sortby];
        },{}); //add a sorting function to the container, but do not apply sort attributes

        return cont;
      },
      values: current_streams,
      add: {
        create: function(id){
          const streamEl = document.createElement("div");
          streamEl.className = "stream";
          streamEl.setAttribute("data-id",id);

          const elements = ["thumbnail","actions"];
          streamEl.elements = {};
          streamEl.elements.header = document.createElement("a");
          streamEl.elements.header.className = "header";
          streamEl.appendChild(streamEl.elements.header);
          for (const i in elements) {
            const e = elements[i];
            streamEl.elements[e] = document.createElement("div");
            streamEl.elements[e].className = e;
            streamEl.elements[e].raw = false;
            streamEl.appendChild(streamEl.elements[e]);
          }
          var wildGroupKey;
          if (id.indexOf("+") >= 0) {
            streamEl.setAttribute("data-iswildcardstream","yes");
            var wildBase = id.split("+")[0];
            wildGroupKey = wildBase + "+";
            streamEl.setAttribute("data-wildgroup", wildGroupKey);
            if (!isWildExpanded(wildGroupKey)) streamEl.classList.add("wild-collapsed");
            const wildparent = document.createElement("span");
            wildparent.className = "wildparent";
            const parts = id.split("+");
            wildparent.appendChild(document.createTextNode(parts.shift()+"+"));
            streamEl.elements.header.appendChild(wildparent);
            streamEl.elements.header.appendChild(document.createTextNode(parts.join("+")));
          }
          else {
            streamEl.setAttribute("data-iswildcardstream","no");
            wildGroupKey = id + "+";
            streamEl.setAttribute("data-wildparent", wildGroupKey);
            if (isWildExpanded(wildGroupKey)) streamEl.classList.add("wild-expanded");
            streamEl.elements.header.innerText = id;
            streamEl.elements._wcBadge = el('span', {class: 'wildcard-count-badge'});
            streamEl.elements._wcBadge.hidden = true;
            streamEl.elements._wcBadge.addEventListener('click', function(e) {
              e.stopPropagation();
              e.preventDefault();
              toggleWildGroup(wildGroupKey);
            });
            streamEl.elements.header.appendChild(streamEl.elements._wcBadge);
          }
          streamEl.elements.header.addEventListener("click",function(){
            showEditStreamPopup(id);
          });
          streamEl.elements.thumbnail.addEventListener("click",function(){
            if (current_streams[id].isfolderstream) {
              if (!current_streams[id].filesfound) {
                streamEl.setAttribute("data-scanning", "");
                var skeletons = [];
                for (var si = 0; si < 3; si++) {
                  var skel = el('div', {class: 'stream folder-skeleton'});
                  skel.appendChild(el('div', {class: 'header folder-skeleton-bar'}));
                  skel.appendChild(el('div', {class: 'thumbnail folder-skeleton-block'}));
                  skeletons.push(skel);
                  streamEl.parentNode.insertBefore(skel, streamEl.nextSibling);
                }
                streamHints.findFolderSubstreams(current_streams[id],function(result){
                  for (var ri = 0; ri < skeletons.length; ri++) skeletons[ri].remove();
                  streamEl.removeAttribute("data-scanning");
                  Object.assign(current_streams,result);
                  $streams.update(current_streams);
                  current_streams[id].filesfound = true;
                  streamEl.setAttribute("data-showingsubstreams","yes");
                  streamEl.setAttribute("title","This is a folder stream: it points to a folder with media files inside.");
                  var count = Object.keys(result).length;
                  if (count >= 50) {
                    streamEl.setAttribute("title","Found " + count + " files in this folder.");
                  }
                });
              }
              else {
                showEditStreamPopup(id);
              }
            }
            else {
              navto("Preview",id);
            }
          });

          const actionsBtn = el('button', null, 'Actions');
          actionsBtn.addEventListener('click', function(e){
            const rect = this.getBoundingClientRect();
            context_menu.fill(id,{pageX:rect.left + window.scrollX, pageY:rect.top + window.scrollY});
            e.stopPropagation();
          });
          streamEl.elements.actions.appendChild(actionsBtn);

          streamEl.elements.activestream = stream.status(id,{
            thumbnail:false,
            tags: {
              readonly: true,
              onclick: function(e,id){
                if (this.getAttribute("data-type") > 0) {
                  const filterEl = $form.querySelector(".field.filter");
                  if (getval(filterEl) == "#"+id) {
                    const last = filterEl._lastval;
                    setval(filterEl, last ? last : "");
                  }
                  else {
                    filterEl._lastval = getval(filterEl);
                    setval(filterEl, "#"+id);
                  }
                }
                return;
              }
            }
          });
          streamEl.insertBefore(
            streamEl.elements.activestream,
            streamEl.children[1]
          );

          streamEl.remove = function(){
            if (this.parentNode) {
              this.parentNode.removeChild(this);
            }
          };

          streamEl.addEventListener("contextmenu",function(e){
            e.preventDefault();
            context_menu.fill(id,e);
          });

          return streamEl;
        },
        update: function(data){
          if ((data.online == 1) && (data.online != this.elements.thumbnail.raw)) {
            const $thumb = stream.thumbnail(data.name,{clone:true});
            this.elements.thumbnail.appendChild($thumb);
            this.elements.thumbnail.raw = data.online;
          }

          if (data.source !== this.raw_source) {
            if (data.source === null) {
              this.elements.header.classList.add("wildparent");
              this.setAttribute("title","This stream has no configuration and will disappear once it goes offline.");
            }
            else {
              if (this.raw_src === null) {
                this.classList.remove("wildparent");
              }

              this.raw_source = data.source;

              //is it a folder stream?
              const inputs_f = findInput("Folder");
              this.setAttribute("title",data.source);
              if (inputs_f) {
                if (mistHelpers.inputMatch(inputs_f.source_match,data.source)) {
                  this.setAttribute("data-isfolderstream","yes");
                  data.isfolderstream = true;
                  this.setAttribute("title","This is a folder stream: it points to a folder with media files inside. Click to request its sub streams.");
                }
                else {
                  this.setAttribute("data-isfolderstream","no");
                  this.setAttribute("title",data.source);
                  data.isfolderstream = false;
                }
              }
            }

            const scenarioId = streamScenarios.detectScenario(data.source);
            const existing = this.elements.header.querySelector('.source-type-icon');
            if (existing) existing.remove();
            if (scenarioId && streamScenarios.SCENARIOS[scenarioId]) {
              const iconSpan = document.createElement('span');
              iconSpan.className = 'source-type-icon';
              iconSpan.setAttribute('data-icon', streamScenarios.SCENARIOS[scenarioId].icon);
              iconSpan.title = streamScenarios.SCENARIOS[scenarioId].title;
              this.elements.header.insertBefore(iconSpan, this.elements.header.firstChild);
            }
          }

          const streambase = data.name.split('+')[0];
          const streamConf = mist.data.streams[streambase];
          this.setAttribute('data-alwayson', (streamConf && streamConf.always_on) ? 'yes' : 'no');

          //translate state integer to something to sort to
          //we want to see "Available" streams first, then by order of how far along it is in its boot sequence
          const state_map = [
            0, //Offline
            1, //Initializing
            2, //Booting
            3, //Waiting for data
            5, //Available
            4, //Shutting down
            0 //Invalid state: treat as offline
          ];

          this.sortValues = {
            name: data.name,
            state: data.stats && (data.stats.length >= 2) ? (state_map.length > data.stats[1] ? state_map[data.stats[1]] : 0) : 0,
            viewers: data.stats && (data.stats.length >= 3) ? data.stats[2] : 0,
            inputs: data.stats && (data.stats.length >= 4) ? data.stats[3] : 0,
            outputs: data.stats && (data.stats.length >= 5) ? data.stats[4] : 0
          };

          if (this.elements._wcBadge && data.name.indexOf('+') < 0) {
            let wcCount = 0;
            const prefix = data.name + '+';
            for (const sid in current_streams) {
              if (sid.indexOf(prefix) === 0) wcCount++;
            }
            if (wcCount > 0) {
              this.elements._wcBadge.textContent = '+' + wcCount;
              this.elements._wcBadge.title = wcCount + ' active wildcard instance' + (wcCount !== 1 ? 's' : '') + ' inheriting this configuration';
              this.elements._wcBadge.hidden = false;
            } else {
              this.elements._wcBadge.hidden = true;
            }
          }

        }
      },
      update: function(){
        this.sort();
        if (this.show_page) this.show_page();
        checkEmpty();
      }
    });
    streamsShell.appendChild($streams);

    let sort_index = sortstreams.by;
    const sort_dir = {name: 1,viewers:-1,state:-1,inputs:-1,outputs:-1}; //for name, ascending is intuitive, but for viewers we prolably want to sort descending
    let sort_reverse = sort_dir[sort_index]*sortstreams.dir;

    const reverseCheckbox = el('input', {type: 'checkbox'});
    reverseCheckbox.checked = (sort_reverse == -1);
    reverseCheckbox.addEventListener('change', function(){
      sort_reverse = this.checked ? -1 : 1;
      sortstreams.dir = sort_reverse*sort_dir[sort_index];
      mistHelpers.stored.set('sortstreams',sortstreams);

      if ($streams) $streams.sort(sort_index,sort_dir[sort_index]*sort_reverse);
    });
    const reverseLabel = el('label');
    reverseLabel.appendChild(reverseCheckbox);
    reverseLabel.appendChild(el('span', null, 'Reverse'));

    const sortUI = formEngine.buildUI([{
      label: "Sort streams by",
      help: "Choose by which attribute the streams listed below should be sorted",
      type: "select",
      select: [
        ["name","Stream name"],
        ["state","State (Online, offline, waiting etc.)"],
        ["viewers","Viewers"],
        ["inputs","Inputs"],
        ["outputs","Outputs"]
      ],
      value: sortstreams.by,
      "function": function(e){
        sort_index = getval(this);
        sortstreams.by = sort_index;
        mistHelpers.stored.set('sortstreams',sortstreams);

        if ($streams) $streams.sort(sort_index,sort_dir[sort_index]*sort_reverse);
      },
      "unit": reverseLabel
    }]);
    const sortChildren = sortUI.children;
    while (sortChildren.length) {
      $form.appendChild(sortChildren[0]);
    }

  }
  else {
    const table = el("table", {class: "streams table-wide"});
    const tableScroll = el('div', {class: 'table-scroll'});
    tableScroll.appendChild(table);
    streamsShell.appendChild(tableScroll);
    table.layout = {
      name: function(d,id){
        if (id != this.raw) {
          this.raw = id;
          const td = this;
          const rowTitle = el('div', {class: 'stream-row-title'});
          const a = el("a", {class: "clickable"}, d.name);
          a.addEventListener("click", function(){
            if ((td.getAttribute("data-iswildcard") == "no") && (td.getAttribute("data-isfolderstream") == "yes")) {
              showEditStreamPopup(id);
            }
            else {
              navto("Preview",id);
            }
          });
          const actionsBtn = el("button", {class: "stream-row-actions", type: "button", title: "Actions"}, "Actions");
          actionsBtn.addEventListener("click", function(e){
            const rect = this.getBoundingClientRect();
            context_menu.fill(id,{pageX:rect.left + window.scrollX, pageY:rect.top + window.scrollY});
            e.stopPropagation();
          });
          let parentstream = "";
          let row = this.closest('tr');
          if (id.indexOf("+") >= 0) {
            const split = id.split("+");
            parentstream = split.shift();
            const substream = split.join("+");
            this.setAttribute("data-iswildcard",parentstream+"+");
            if (row) {
              row.setAttribute("data-wildgroup", parentstream + "+");
              if (!isWildExpanded(parentstream + "+")) row.classList.add("wild-collapsed");
            }
            a.title = "This is a wildcard stream: its config is inherited from its parent: '"+parentstream+"'.";
            a.innerHTML = '';
            const wp = el("span", {class: "wildparent"}, parentstream+"+");
            a.appendChild(wp);
            a.appendChild(document.createTextNode(substream));
          }
          else {
            this.setAttribute("data-iswildcard","no");
            var wildGroupKey = id + "+";
            if (row) {
              row.setAttribute("data-wildparent", wildGroupKey);
              if (isWildExpanded(wildGroupKey)) row.classList.add("wild-expanded");
            }
            this._wcBadge = el('span', {class: 'wildcard-count-badge'});
            this._wcBadge.hidden = true;
            this._wcBadge.addEventListener('click', function(e) {
              e.stopPropagation();
              e.preventDefault();
              toggleWildGroup(wildGroupKey);
            });
            a.appendChild(this._wcBadge);
          }
          rowTitle.appendChild(a);
          rowTitle.appendChild(actionsBtn);
          this.innerHTML = '';
          this.appendChild(rowTitle);
          this._nameLink = a;
        }
        if (d.source !== this.raw_src) {
          if (d.source === null) {
            this.classList.add("wildparent");
            const linkEl = this.querySelector("a");
            if (linkEl) linkEl.title = "This stream has no configuration and will disappear once it goes offline.";
          }
          else if (this.raw_src === null) {
            this.classList.remove("wildparent");
            const linkEl2 = this.querySelector("a");
            if (linkEl2) {
              const parentName = id.indexOf("+") >= 0 ? id.split("+")[0] : "";
              linkEl2.title = parentName ? "This is a wildcard stream: its config is inherited from its parent: '"+parentName+"'." : "";
            }
          }

          this.raw_src = d.source;
          //check if this is a folder stream
          if (mist.data.capabilities) {
            const inputs_f = findInput("Folder");
            if (inputs_f) {
              if (mistHelpers.inputMatch(inputs_f.source_match,d.source)) {
                this.setAttribute("data-isfolderstream","yes");
                this.title = "This is a folder stream: it points to a folder with media files inside. Click the '\u2795' to request its sub streams.";
                d.isfolderstream = true;
              }
              else {
                this.setAttribute("data-isfolderstream","no");
                this.title = d.source;
                d.isfolderstream = false;
              }
            }
          }
        }
        const scenarioId = streamScenarios.detectScenario(d.source);
        if (scenarioId !== this.raw_scenario) {
          this.raw_scenario = scenarioId;
          const nameLink = this._nameLink || this.querySelector('a.clickable');
          if (nameLink) {
            const existing = nameLink.querySelector('.source-type-icon');
            if (existing) { existing.remove(); }
            if (scenarioId && streamScenarios.SCENARIOS[scenarioId]) {
              const s = streamScenarios.SCENARIOS[scenarioId];
              const icon = el('span', {class: 'source-type-icon', 'data-icon': s.icon, title: s.title});
              nameLink.insertBefore(icon, nameLink.firstChild);
            }
          }
        }
        const streambase = d.name.split('+')[0];
        const conf = mist.data.streams[streambase];
        const row = this.closest('tr');
        if (row) {
          row.setAttribute('data-alwayson', (conf && conf.always_on) ? 'yes' : 'no');
        }

        if (this._wcBadge && d.name.indexOf('+') < 0) {
          let wcCount = 0;
          const prefix = d.name + '+';
          for (const sid in current_streams) {
            if (sid.indexOf(prefix) === 0) wcCount++;
          }
          if (wcCount > 0) {
            this._wcBadge.textContent = '+' + wcCount;
            this._wcBadge.title = wcCount + ' active wildcard instance' + (wcCount !== 1 ? 's' : '') + ' inheriting this configuration';
            this._wcBadge.hidden = false;
          } else {
            this._wcBadge.hidden = true;
          }
        }
      },
      state: function(d){
        let state = el("div", {'data-streamstatus': 0}, "Inactive");
        if ("stats" in d) {
          if (this.raw == d.stats[1]) { return; }
          const s = ["Inactive","Initializing","Booting","Waiting for data","Available","Shutting down","Invalid state"];
          state = el("div", {'data-streamstatus': d.stats[1]}, s[d.stats[1]]);
          this.raw = d.stats[1];
        }
        this.innerHTML = '';
        this.appendChild(state);
        this.classList.add("activestream");
      },
      tags: function(d){
        //this is a dynamic element
        this.update(d);
      },
      source: function(d){
        if (d.source === this.raw_src) return;
        this.raw_src = d.source;
        this.textContent = d.source || '';
        this.title = d.source || '';
      },
      viewers: function(d){
        let out = "";
        if ("stats" in d) {
          if (this.raw == d.stats[2]) { return; }
          out = d.stats[2];
          this.raw = d.stats[2];
          if (out == 0) out = "";
        }
        this.innerHTML = out;
      },
      inputs: function(d){
        let out = "";
        if ("stats" in d) {
          if (this.raw == d.stats[3]) { return; }
          out = d.stats[3];
          this.raw = d.stats[3];
          if (out == 0) out = "";
        }
        this.innerHTML = out;
      },
      outputs: function(d){
        let out = "";
        if ("stats" in d) {
          if (this.raw == d.stats[4]) { return; }
          out = d.stats[4];
          this.raw = d.stats[4];
          if (out == 0) out = "";
        }
        this.innerHTML = out;
      }
    };

    const tr = el("tr", {'data-sortby': 'name'});
    const thead = el("thead", {class: "sticky"});
    thead.appendChild(tr);
    table.appendChild(thead);
    const headers = {
      name: "Stream name",
      source: "Source"
    };
    for (const i in table.layout) {
      const label = i in headers ? headers[i] : format.capital(i);
      const th = el("th", null, label);
      if (label != "") {
        th.setAttribute("data-index",i);
      }
      tr.appendChild(th);
    }
    $streams = dynamicUI.dynamic({
      create: function(){
        const tbody = document.createElement("tbody");

        uiCore.sortableItems(tbody,function(sortby){
          return this._cells[sortby].raw;
        },{controls:tr,sortsave:"sortstreams"});

        tbody.remove = function(){
          if (this.parentNode) {
            this.parentNode.removeChild(this);
          }
        };
        return tbody;
      },
      values: current_streams,
      add: {
        create: function(streamname){
          const row = document.createElement("tr");
          row.setAttribute("data-id",streamname);

          row._cells = {};
          for (const i in table.layout) {
            const td = document.createElement("td");
            td.setAttribute("data-index",i);
            row._cells[i] = td;
            row.append(td);
          }
          const tags_td = row._cells.tags;
          row._cells.tags = stream.tags({
            streamname: streamname,
            context_menu: context_menu,
            onclick: function(e,id){
              if (this.getAttribute("data-type") > 0) {
                const filterEl = $form.querySelector(".field.filter");
                if (getval(filterEl) == "#"+id) {
                  const last = filterEl._lastval;
                  setval(filterEl, last ? last : "");
                }
                else {
                  filterEl._lastval = getval(filterEl);
                  setval(filterEl, "#"+id);
                }
              }
            },
            getStreamstatus: function(){
              return this.closest("tr").querySelector("[data-streamstatus]").getAttribute("data-streamstatus");
            }
          });
          tags_td.appendChild(row._cells.tags);

          row.addEventListener("contextmenu",function(e){
            e.preventDefault();
            context_menu.fill(streamname,e);
          });
          row.addEventListener("click",function(e){
            if (current_streams[streamname].isfolderstream) {
              if ("filesfound" in current_streams[streamname]) {
                return;
              }
              row.setAttribute("data-scanning", "");
              var skeletonRows = [];
              var colCount = row.cells ? row.cells.length : 5;
              for (var si = 0; si < 3; si++) {
                var skelRow = document.createElement("tr");
                skelRow.className = "folder-skeleton-row";
                for (var ci = 0; ci < colCount; ci++) {
                  var skelTd = document.createElement("td");
                  skelTd.appendChild(el('div', {class: 'folder-skeleton-bar'}));
                  skelRow.appendChild(skelTd);
                }
                skeletonRows.push(skelRow);
                if (row.nextSibling) {
                  row.parentNode.insertBefore(skelRow, row.nextSibling);
                } else {
                  row.parentNode.appendChild(skelRow);
                }
              }
              streamHints.findFolderSubstreams(current_streams[streamname],function(result){
                for (var ri = 0; ri < skeletonRows.length; ri++) skeletonRows[ri].remove();
                row.removeAttribute("data-scanning");
                Object.assign(current_streams,result);
                $streams.update(current_streams);
                row.setAttribute("data-showingsubstreams","");
              });

              $streams.show_page(this);
            }
          });

          row.remove = function(){
            if (this.parentNode) {
              this.parentNode.removeChild(this);
            }
          };

          return row;
        },
        update: function(data,allValues){
          for (const i in table.layout) {
            table.layout[i].call(this._cells[i],data,this._id);
          }
        }
      },
      update: function(){
        this.sort();
        if (this.show_page) this.show_page();
        checkEmpty();
      }
    });
    table.appendChild($streams);
  }


  $streams.filter = function(str){
    if (str[0] == "#") {
      //filter tags
      str = str.slice(1);
      for (let i = 0; i < this.children.length; i++) {
        const item = this.children[i];
        const thetags = other == "thumbnails" ? item.elements.activestream.querySelector(".tags") : item._cells.tags;
        if (thetags && (str in thetags.values) && (thetags.values[str] > 0)) {
          item.classList.remove("hidden");
        }
        else {
          item.classList.add("hidden");
        }
      }
    }
    else {
      //filter stream name
      str = str.toLowerCase();
      for (let i = 0; i < this.children.length; i++) {
        const item = this.children[i];
        if (item.getAttribute("data-id").toLowerCase().indexOf(str) >= 0) {
          item.classList.remove("hidden");
        }
        else {
          item.classList.add("hidden");
        }
      }
    }
    $streams.show_page();
  };

  streamsShell.appendChild(context_menu.ele);
  context_menu.fill = function(streamname,e){
    let settings = current_streams[streamname];
    function formatStreamname(streamname) {
      if (settings.source === null) {
        //this stream does not exist in config
        return "<span class=\"wildparent\">"+streamname+"</span><div class=\"description\">Unconfigured</div>";
      }
      if (streamname.indexOf("+") >= 0) {
        //it's a wildcard stream: highlight the base
        let split = streamname.split("+");
        return "<span class=\"wildparent\">"+split[0]+"+</span>"+split.slice(1).join("+");
      }
      return streamname;
    }
    const headerEl = el("div", {class: "header"});
    headerEl.innerHTML = formatStreamname(streamname);
    const scenarioId = streamScenarios.detectScenario(settings.source);
    if (scenarioId && streamScenarios.SCENARIOS[scenarioId]) {
      const sc = streamScenarios.SCENARIOS[scenarioId];
      const scIcon = el('span', {class: 'source-type-icon', 'data-icon': sc.icon});
      scIcon.style.marginRight = '0.4em';
      headerEl.insertBefore(scIcon, headerEl.firstChild);
    }
    if (settings.stats && settings.stats.length >= 2) {
      const states = ["Inactive","Initializing","Booting","Waiting","Online","Shutting down"];
      const stateIdx = settings.stats[1];
      const badge = el('span', {class: 'context-menu-badge', 'data-streamstatus': stateIdx}, states[stateIdx] || '');
      headerEl.appendChild(badge);
      if (settings.stats.length >= 3 && settings.stats[2] > 0) {
        headerEl.appendChild(el('span', {class: 'context-menu-viewers'}, settings.stats[2] + ' viewer' + (settings.stats[2] !== 1 ? 's' : '')));
      }
    }
    const header = [headerEl];

    const editLabel = el("span");
    editLabel.innerHTML = "Edit "+(streamname.indexOf("+") < 0 ? "stream" : "<b>"+streamname.split("+")[0]+"</b>");
    const gototabs = [
      [editLabel,function(){ showEditStreamPopup(streamname); },"Edit","Change the settings of this stream."],
      ["Stream status",function(){ navto("Status",streamname); },"Status","See more details about the status of this stream."],
      ["Preview stream",function(){ navto("Preview",streamname); },"Preview","Watch the stream."],
      ["Embed stream",function(){ navto("Embed",streamname); },"Embed","Get urls to this stream or get code to embed it on your website."]
    ];
    let actions = [
      ["Delete stream",function(){
        if (confirm('Are you sure you want to delete the stream "'+streamname+'"?')) {
          delete mist.data.streams[streamname];
          apiClient.send(function(d){
            delete current_streams[streamname];
            $streams.update(current_streams);
          },{deletestream: [streamname]});
        }
      },"trash","Remove this stream's settings."],
      ["Stop sessions",function(){
        if (confirm("Are you sure you want to disconnect all sessions (viewers, pushes and possibly the input) for the stream '"+streamname+"'?")) {
          apiClient.send(function(){
            //done
          },{stop_sessions:streamname});
        }
      },"stop","Disconnect sessions for this stream. Disconnecting a session will kill any currently open connections (viewers, pushes and possibly the input). If the USER_NEW trigger is in use, it will be triggered again by any reconnecting connections."],
      ["Invalidate sessions",function(){
        if (confirm("Are you sure you want to invalidate all sessions for the stream '"+streamname+"'?\nThis will re-trigger the USER_NEW trigger.")) {
          apiClient.send(function(){
            //done
          },{invalidate_sessions:streamname});
        }
      },"invalidate","Invalidate all the currently active sessions for this stream. This has the effect of re-triggering the USER_NEW trigger, allowing you to selectively close some of the existing connections after they have been previously allowed. If you don't have a USER_NEW trigger configured, this will not have any effect."],
      ["Nuke stream",function(){
        if (confirm("Are you sure you want to completely shut down the stream '"+streamname+"'?\nAll viewers will be disconnected.")) {
          apiClient.send(function(){
            //done
          },{nuke_stream:streamname});
        }
      },"nuke","Shut down a running stream completely and/or clean up any potentially left over stream data in memory. It attempts a clean shutdown of the running stream first, followed by a forced shut down, and then follows up by checking for left over data in memory and cleaning that up if any is found."]
    ];
    if (settings.source === null) {
      gototabs.shift();
      const createLabel = el("span");
      createLabel.innerHTML = "Create <b>"+streamname.split("+")[0]+"</b>";
      gototabs.unshift([createLabel,function(){ showEditStreamPopup(streamname); },"Edit","Create this stream."]);
      actions.shift();
    }
    if (settings.isfolderstream) {
      gototabs.pop();
      gototabs.pop();
      actions = [actions[0]];
    }



    const menu = [header];
    if (current_streams[streamname].isfolderstream && !current_streams[streamname].filesfound) {
      menu.push([
        ["Scan folder for sub streams",function(){
          streamHints.findFolderSubstreams(current_streams[streamname],function(result){
            Object.assign(current_streams,result);
            $streams.update(current_streams);
          });
        },"folder"]
      ]);
    }
    menu.push(gototabs);
    menu.push(actions);

    //let's not wake it up for this alone
    if (current_streams[streamname].online == 1) {
      const aside = el("aside");
      aside.appendChild(stream.thumbnail(streamname));
      menu.push(aside);
    }

    context_menu.show(menu,e);
  };




  sockets.ws.active_streams.subscribe(function(type,data){
    if (type != "stream") return;
    const streamname = data[0];
    const streambase = streamname.split("+")[0];

    if (streambase in mist.data.streams) {
      if (streambase != streamname) {
        if  (!(streamname in current_streams)) {
          //it's a new wildcard stream
          current_streams[streamname] = Object.assign({},mist.data.streams[streambase]);
          current_streams[streamname].name = streamname;
          current_streams[streamname].online = data[1] > 0 ? 1 : 0;
          if (current_streams[streambase].isfolderstream) {
            current_streams[streamname].source += streamname.replace(streambase+"+");
          }
        }
        else if (data.slice(1).every((v)=>!v)) {
          //this wildcard stream no longer has any stats or tags
          //remove it from the table
          delete current_streams[streamname];
          $streams.update(current_streams);
          return;
        }
      }
      current_streams[streamname].stats = data;

      $streams.update(current_streams);
    }
    else if (streamname in current_streams) {
      current_streams[streamname].stats = data;
      $streams.update(current_streams);
    }
    else {
      //received information about unknown stream
      if (streamname) {
        //insert with default settings, uneditable
        current_streams[streamname] = {
          name: streamname,
          source: null,
          stats: data
        };
        $streams.update(current_streams);
      }
      else {
        console.log("Received information about unknown stream",streamname,data);
      }
    }

  });

  const pagecontrol = uiCore.pagecontrol($streams,pagesize);
  streamsShell.appendChild(pagecontrol);

  //save selected page size
  const pageLengthEl = pagecontrol.querySelector('[data-role="pagelength"]') || (pagecontrol.elements && pagecontrol.elements.pagelength);
  if (pageLengthEl) {
    pageLengthEl.addEventListener('change', function(){
      mistHelpers.stored.set("streams_pagesize", this.value);
    });
  }

  checkEmpty();

};

register('Streams', {
  guided: streamsPage,
  advanced: streamsPage
});

function streamHasConfig(streamname) {
  if (!streamname) { return false; }
  const streambase = streamname.split("+")[0];
  return !!(mist.data.streams && (streambase in mist.data.streams));
}

function showEditStreamPopup(other) {
  if (!other) { other = ''; }
  navto('Edit', other);
}

export { showEditStreamPopup as editStream };

registerTab('Status', function(tab, other, prev, $main, $pageHeader) {
  if (other == '') { navto('Streams'); return; }
  if (!streamHasConfig(other)) { navto('Edit', other); return; }
  $pageHeader.classList.add('page-header--stream-detail');
  $main.classList.add('page-body--stream-detail');

  const dashboard = el("div", {class: "dashboard stream-status-dashboard slab-shell slab-shell--seamed"});

  $pageHeader.innerHTML = '';
  $pageHeader.appendChild(stream.header(other,tab));
  $main.appendChild(dashboard);

  const actions = stream.actions("Status",other);
  actions.classList.add("stream-slab", "stream-slab--actions");
  dashboard.appendChild(actions);
  let statusViewBuilt = false;

  const findMist = stream.findMist(function(url,data){
    if (statusViewBuilt) { return; }
    statusViewBuilt = true;

    dashboard.appendChild(stream.status(other,{
      status: false,
      stats: false,
      slab: true,
      title: "Live source",
      titleIcon: "radio"
    }));
    dashboard.appendChild(stream.metadata(other));
    dashboard.appendChild(stream.clients(other));
    dashboard.appendChild(stream.pushes(other));
    dashboard.appendChild(stream.processes(other));
    dashboard.appendChild(stream.triggers(other,"Status"));
    dashboard.appendChild(stream.logs(other));
    dashboard.appendChild(stream.accesslogs(other));
  });
  dashboard.appendChild(findMist);

});

registerTab('Preview', function(tab, other, prev, $main, $pageHeader) {

  if (other == '') { navto('Streams'); return; }
  if (!streamHasConfig(other)) { navto('Edit', other); return; }
  $pageHeader.classList.add('page-header--stream-detail');
  $main.classList.add('page-body--stream-detail');

  const dashboard = el('div', {class: "dashboard stream-preview-dashboard slab-shell slab-shell--seamed"});

  function buildSlab(title, icon, classes) {
    let className = "stream-slab";
    if (classes) {
      className += " " + classes;
    }
    const slab = el("section", {class: className});
    const head = el("div", {class: "stream-slab-head"});
    head.appendChild(el("h3", {class: "stream-slab-title", "data-icon": icon}, title));
    const body = el("div", {class: "stream-slab-body"});
    slab.appendChild(head);
    slab.appendChild(body);
    slab.slabBody = body;
    return slab;
  }

  let status = el("section", {class: "activestream stream-slab stream-slab--status preview-status-shell"}, "Loading..");
  const mediaStack = el("div", {class: "preview-media-stack"});
  const diagnosticsStack = el("div", {class: "preview-diagnostics-stack"});
  $pageHeader.innerHTML = '';
  $pageHeader.appendChild(stream.header(other,tab));
  $main.appendChild(dashboard);
  dashboard.appendChild(status);
  dashboard.appendChild(mediaStack);
  dashboard.appendChild(diagnosticsStack);

  const findMist = stream.findMist(function(url){
    window.mv = {};
    mediaStack.innerHTML = '';

    const previewContent = stream.preview(other,window.mv);
    const previewSlab = buildSlab("Live player", "play", "stream-slab--preview-player");
    previewSlab.slabBody.appendChild(previewContent);
    mediaStack.appendChild(previewSlab);
    mediaStack.appendChild(stream.playercontrols(window.mv,previewContent));
    const statusContent = stream.status(other,{
      tags:false,
      thumbnail:false,
      status:false,
      stats:false,
      slab:true,
      title:"Live source",
      titleIcon:"radio"
    });
    statusContent.classList.add("preview-status-shell");
    status.parentNode.replaceChild(statusContent, status);
    status = statusContent;
    diagnosticsStack.innerHTML = '';
    diagnosticsStack.appendChild(stream.metadata(other));

  });

  dashboard.appendChild(findMist);

});

registerTab('Embed', function(tab, other, prev, $main, $pageHeader) {
  if (other == '') { navto('Streams'); return; }
  if (!streamHasConfig(other)) { navto('Edit', other); return; }
  $pageHeader.classList.add('page-header--stream-detail');
  $main.classList.add('page-body--stream-detail', 'embed-page-body');

  $pageHeader.innerHTML = '';
  $pageHeader.appendChild(stream.header(other,tab));

  $main.appendChild(
    stream.findMist(function(url){
      $main.appendChild(stream.embedurls(other,url,this.getUrls()));
    },false,true) //force full url search
  );

});
