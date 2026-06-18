import { APP_NAME } from '@brand';
import * as format from '../core/formatters.js';
import { editAutomation } from '../pages/triggers.js';
import { editStream } from './streams.js';
import { pushes as pushesTab } from '../pushes/pushes.js';
import { apiClient } from '../core/api_client.js';
import { formEngine } from '../core/form_engine.js';
import { navto } from '../core/navigation.js';
import { tabView } from '../core/tab_view.js';
import { dynamicUI } from '../core/dynamic.js';
import { uiCore } from '../core/ui_core.js';
import { uiHelpers } from '../core/ui_helpers.js';
import { findOutput } from '../core/capabilities.js';
import { sockets } from '../core/sockets.js';
import { streamHints } from './stream_hints.js';
import { mistHelpers } from '../core/mist_helpers.js';
import { el, getval } from '../core/dom_helpers.js';
import { parseURL, otherhost } from '../core/app_state.js';
import { mistPlay, mistplayers, MistUtil as PlayerUtil } from '@player';
import { getActiveThemeId } from '../core/themes.js';

function appendStreamNode(parent, value) {
  if (!value && value !== 0) { return; }
  if (Array.isArray(value)) {
    for (let i = 0; i < value.length; i++) {
      appendStreamNode(parent, value[i]);
    }
    return;
  }
  if (value instanceof Node) {
    parent.appendChild(value);
    return;
  }
  if ((typeof value === "string") || (typeof value === "number")) {
    parent.appendChild(document.createTextNode(String(value)));
  }
}

function streamCellRenderKey(value) {
  if ((value === null) || (typeof value == "undefined")) { return ""; }
  if (Array.isArray(value)) {
    return "array:" + value.map(streamCellRenderKey).join("|");
  }
  if (value instanceof Node) {
    return "node:" + value.textContent;
  }
  return String(value);
}

function setStreamCellValue(cell, value) {
  if ((value === null) || (typeof value == "undefined")) {
    cell.innerHTML = "";
    return;
  }
  if (Array.isArray(value)) {
    cell.innerHTML = "";
    for (let i = 0; i < value.length; i++) {
      appendStreamNode(cell, value[i]);
    }
    return;
  }
  if (value instanceof Node) {
    cell.innerHTML = "";
    cell.appendChild(value);
    return;
  }
  cell.innerHTML = value;
}

function createStreamSlab(options) {
  options = Object.assign({
    className: "",
    title: "",
    titleTag: "h3",
    icon: "",
    actions: null
  }, options || {});

  const classNames = ["stream-slab"];
  if (options.className) {
    classNames.push(options.className);
  }
  const slab = el("section", {class: classNames.join(" ")});
  const head = el("div", {class: "stream-slab-head"});
  const title = el(options.titleTag, {class: "stream-slab-title"}, options.title);
  if (options.icon) {
    title.setAttribute("data-icon", options.icon);
  }
  head.appendChild(title);

  let actions = null;
  if (options.actions) {
    actions = el("div", {class: "stream-slab-actions"});
    appendStreamNode(actions, options.actions);
    if (actions.childNodes.length) {
      head.appendChild(actions);
    }
  }

  const body = el("div", {class: "stream-slab-body"});
  slab.appendChild(head);
  slab.appendChild(body);
  slab.slabHead = head;
  slab.slabTitle = title;
  slab.slabActions = actions;
  slab.slabBody = body;
  return slab;
}

export const stream = {
  header: function(streamname,currenttab,parentstream){
    const streambase = parentstream || (streamname || "").split("+")[0];
    const streamExists = !!(streambase && mist.data.streams && (streambase in mist.data.streams));
    const subtitle = currenttab == "Edit"
      ? (streamExists ? 'Edit "'+streambase+'"' : 'Create "'+streambase+'"')
      : currenttab;
    const statusNode = stream.status(streamname,{tags:false,thumbnail:false,livestreamhint:false});
    statusNode.classList.add('subtab-header-status-indicator');

    let tabDefs = [
      ["Edit","Edit"],
      { label: "Status", tab: "Status", disabled: !streamExists },
      { label: "Preview", tab: "Preview", disabled: !streamExists },
      { label: "Embed", tab: "Embed", disabled: !streamExists }
    ];

    let isFolderStream = false;
    const config = mist.data.streams ? mist.data.streams[streambase] : null;
    if (config?.source && (config.source.match(/^\/.+\/$/) || config.source.match(/^folder:.+$/))) {
      isFolderStream = true;
    }
    if (isFolderStream) {
      tabDefs = tabDefs.filter(function(tabInfo){
        if (!tabInfo || typeof tabInfo == "string") { return true; }
        if (Array.isArray(tabInfo)) { return tabInfo[1] != "Preview" && tabInfo[1] != "Embed"; }
        return tabInfo.tab != "Preview" && tabInfo.tab != "Embed";
      });
    }

    tabDefs.push(false);
    tabDefs.push(["Overview","Streams"]);

    const tabs = [];
    let cont = null;
    for (let i = 0; i < tabDefs.length; i++) {
      const tabInfo = tabDefs[i];
      if (!tabInfo) {
        tabs.push(false);
        continue;
      }
      let label = tabInfo;
      let tabName = tabInfo;
      let disabled = false;
      let disabledTitle = "";
      if (typeof tabInfo == "object" && !Array.isArray(tabInfo)) {
        label = tabInfo.label || tabInfo.tab;
        tabName = tabInfo.tab;
        disabled = !!tabInfo.disabled;
        disabledTitle = tabInfo.disabledTitle || "Create this stream first.";
      } else if (typeof tabInfo != "string") {
        label = tabInfo[0];
        tabName = tabInfo[1];
      }
      tabs.push({
        label: label,
        tab: tabName,
        icon: tabName,
        active: (tabName == currenttab),
        disabled: disabled,
        disabledTitle: disabledTitle,
        onClick: function(tabNameCopy) {
          return function() {
            if (tabNameCopy == currenttab) { return; }
            if (cont && (currenttab == "Edit") && (cont.getAttribute("data-changed") == "yes")) {
              if (!confirm("Your settings have not been saved. Are you sure you want to navigate away?")) {
                return;
              }
            }
            navto(tabNameCopy,tabNameCopy == "Streams" ? "" : streamname);
          };
        }(tabName)
      });
    }

    cont = uiHelpers.createSubtabHeader({
      classes: ['subtab-header--stream'],
      title: 'Stream "'+streamname+'"',
      subtitle: subtitle,
      status: statusNode,
      currentIcon: currenttab,
      tabs: tabs
    });
    cont.setAttribute("data-changed","no");
    return cont;
  },
  bigbuttons: function(streamname,currenttab){
    const cont = el('div', {class: 'bigbuttons'});

    const settingsBtn = el('button', {class: 'settings'}, 'Settings');
    settingsBtn.addEventListener('click', function(){
      editStream(streamname);
    });
    cont.appendChild(settingsBtn);

    let isFolderStream = false;
    if (streamname.indexOf("+") < 0) {
      const config = mist.data.streams[streamname];
      if (config.source && (config.source.match(/^\/.+\/$/) || config.source.match(/^folder:.+$/))) {
        isFolderStream = true;
      }
    }



    if (currenttab != "Status") {
      const statusBtn = el('button', {class: 'status'}, 'Status');
      statusBtn.addEventListener('click', function(){
        navto('Status',streamname);
      });
      cont.appendChild(statusBtn);
    }
    if ((currenttab != "Preview") && !isFolderStream) {
      const previewBtn = el('button', {class: 'preview'}, 'Preview');
      previewBtn.addEventListener('click', function(){
        navto('Preview',streamname);
      });
      cont.appendChild(previewBtn);
    }
    if ((currenttab != "Embed") && !isFolderStream) {
      const embedBtn = el('button', {class: 'embed'}, 'Embed');
      embedBtn.addEventListener('click', function(){
        navto('Embed',streamname);
      });
      cont.appendChild(embedBtn);
    }

    const returnBtn = el('button', {class: 'cancel return'}, 'Return');
    returnBtn.addEventListener('click', function(){
      navto('Streams');
    });
    cont.appendChild(returnBtn);

    return cont;
  },
  findMist: function(on_success_callback,url_rewriter,fullsearch){
    const cont = el("div", {class: "findMist"});
    cont.hidden = true;

    const debug = false;

    function makeUnique(ret) {
      const unique = [];
      for (let i = 0; i < ret.length; i++) {
        if (ret.indexOf(ret[i]) == i) { unique.push(ret[i]); }
      }
      return unique;
    }

    function findHTTP(callback){
      const result = { HTTP: [], HTTPS: [] };
      let http = result.HTTP;
      let https = result.HTTPS;

      function normalizeConnectorName(name) {
        return ("" + (name || "")).replace(".exe","").toUpperCase();
      }
      function normalizeBaseURL(url, protocolHint) {
        if (!url) { return ""; }
        let base = ("" + url).trim();
        if (!base.length) { return ""; }
        if (base.slice(0,2) == "//") {
          base = protocolHint.toLowerCase()+":"+base;
        }
        else if (base.indexOf("://") < 0) {
          base = protocolHint.toLowerCase()+"://"+base.replace(/^\/+/,"");
        }
        base = parseURL(base).full;
        if (base.slice(-1) != "/") { base += "/"; }
        return base;
      }
      function getPort(connector, protocolName) {
        if (connector.port) { return connector.port; }
        if (!mist.data.capabilities || !mist.data.capabilities.connectors) { return false; }
        const keys = [
          connector.connector,
          protocolName,
          protocolName+".exe"
        ];
        for (const i in keys) {
          const key = keys[i];
          if (!(key in mist.data.capabilities.connectors)) { continue; }
          const optional = mist.data.capabilities.connectors[key].optional;
          if (optional && optional.port && ('default' in optional.port)) {
            return optional.port['default'];
          }
        }
        return false;
      }

      if (debug) console.log("Attempting to find urls to reach "+APP_NAME+"'s HTTP output..");

      if (sockets.http_host) {
        let cachedURL = normalizeBaseURL(sockets.http_host,location.protocol == "https:" ? "HTTPS" : "HTTP");
        if (!cachedURL) { cachedURL = sockets.http_host; }
        if (fullsearch) {
          http.push(cachedURL);
          https.push(cachedURL);
          if (debug) console.log("Found previously used urls to "+APP_NAME+"'s HTTP output, but performing a full search; adding it to both the result lists",cachedURL);
        }
        else {
          if (debug) console.log("Found a previously used url to "+APP_NAME+"'s HTTP output, using that one",cachedURL);
          return callback([cachedURL]);
        }
      }

      if (!mist.data.capabilities) {
        if (debug) console.log("I don't know "+APP_NAME+"'s capabilities yet, retrieving those first.. brb")
        apiClient.send(function(){
          findHTTP(callback);
        },{capabilities:true});
        return;
      }



      const stored = mistHelpers.stored.get();
      if ("HTTPUrl" in stored) {
        const storedHTTP = normalizeBaseURL(stored.HTTPUrl,"HTTP");
        if (storedHTTP) {
          if (debug) console.log("Found a previously valid HTTP url stored in "+APP_NAME+"'s config, adding it to the HTTP list",storedHTTP);
          http.push(storedHTTP);
        }
      }
      if ("HTTPSUrl" in stored) {
        const storedHTTPS = normalizeBaseURL(stored.HTTPSUrl,"HTTPS");
        if (storedHTTPS) {
          if (debug) console.log("Found a previously valid HTTPS url stored in "+APP_NAME+"'s config, adding it to the HTTPS list",storedHTTPS);
          https.push(storedHTTPS);
        }
      }
      const protocols = (mist.data.config && mist.data.config.protocols) ? mist.data.config.protocols : [];
      for (const i in protocols) {
        const connector = protocols[i];
        const protocolName = normalizeConnectorName(connector.connector);
        switch (protocolName) {
          case "HTTP":
          case "HTTPS": {
            if (connector.pubaddr) {
              let pubaddr = connector.pubaddr;
              if (!Array.isArray(pubaddr)) { pubaddr = [pubaddr]; }
              for (const j in pubaddr) {
                const candidate = normalizeBaseURL(pubaddr[j],protocolName);
                if (!candidate) { continue; }
                const protocol = parseURL(candidate).protocol.replace("://","").toUpperCase();
                if (protocol in result) {
                  result[protocol].push(candidate);
                  if (debug) console.log("Found a public address in the protocol config, adding it to the "+protocol+" list",candidate);
                }
                else {
                  result[protocolName].push(candidate);
                  console.warn("Unknown protocol in public address configuration for "+connector.connector+": ",candidate);
                }
              }
            }
            const port = getPort(connector,protocolName);
            if (port) {
              let u = parseURL(mist.user.host,{port:port,pathname:'',protocol:protocolName.toLowerCase()+":"}).full;
              u = normalizeBaseURL(u,protocolName);
              result[protocolName].push(u);
              if (debug) console.log("Found a port in the protocol config, adding it to the "+protocolName+" list",u);
            }

            break;
          }
        }
      }


      if (!http.length) {
        let u = parseURL(mist.user.host,{port:8080,pathname:''}).full;
        u = normalizeBaseURL(u,"HTTP");
        if (debug) console.log("Haven't found any HTTP urls yet, adding the default to the HTTP list",u);
        http.push(u);
      }
      if (!https.length) {
        if (debug) console.log("Haven't found any HTTPS urls yet, copying the HTTP list to HTTPS: it's worth a try",http);
        result.HTTPS = http;
        https = result.HTTPS;
      }

      http = makeUnique(http);
      https = makeUnique(https);

      cont.urls = result;
      if (debug) console.log("Full scan result:",result);
      const r = location.protocol == "https:" ? https : http;
      if (debug) console.log("Returning:",r);
      return callback(r);
    }

    function on_fail(urls) {
      if ((urls.length == 1) && (urls[0] == sockets.http_host)) {
        sockets.http_host = null;
        return findHTTP(function(result){
          if ((!result) || (!result.length)) {
            return on_fail([]);
          }
          cont.hidden = true;
          on_success(result[0]);
        });
      }

      let attempts;
      let morehelp;
      if (urls.length) {
        attempts = el("div");
        const ulList = el("ul");
        for (const i in urls) {
          const li = el("li");
          const a = el("a", {href: urls[i], target: "MistURL"});
          a.textContent = urls[i];
          li.appendChild(a);
          ulList.appendChild(li);
        }
        attempts.appendChild(ulList);
        morehelp = el("div");
        const infoIcon = el("span");
        infoIcon.textContent = "\u2139\uFE0F";
        Object.assign(infoIcon.style, {position: "absolute", margin: "1em 0 0 2em"});
        morehelp.appendChild(infoIcon);
        const helpBox = el("div", {style: {border: "1px solid #bbb", padding: "0.5em 1em 0 3em", margin: "0 1em"}});
        helpBox.appendChild(el("h2", null, "Why am I seeing this?"));
        const p1 = el("p");
        p1.innerHTML = "If you think one of the urls above should have worked, try clicking the link. <br>If something went wrong, your browser may give you more information about what it is. <br>If you see something like this:";
        helpBox.appendChild(p1);
        const exampleBox = el("div", {style: {marginLeft: "2em"}});
        exampleBox.innerHTML = "<h1>Unsupported Media Type</h1>The server isn't quite sure what you wanted to receive from it.";
        helpBox.appendChild(exampleBox);
        const p2 = el("p");
        p2.innerHTML = "then "+APP_NAME+" <i>can</i> be reached there but it couldn't be called from this page, possibly because of mixed content (if you're on an https website, your browser will refuse to load http content: create an https output) or CORS issues (if the HTTP output is accessible on a domain other than '"+location.origin+"' and that is not configured to allow cross-domain requests: check your proxy configuration).";
        helpBox.appendChild(p2);
        morehelp.appendChild(helpBox);

      }
      else {
        attempts = el("span");
        attempts.textContent = "I've not tried anything yet. I'm cluessless. Please help.";
      }

      const save= {};
      const S = location.protocol == "https:" ? "S" : "";
      cont.hidden = false;
      cont.innerHTML = '';
      const p = el("p");
      p.textContent = "Something went wrong: I could not locate "+APP_NAME+"'s HTTP"+S+" output url. Where can it be reached?";
      cont.appendChild(p);
      const descDiv = el("div", {class: "description"});
      descDiv.appendChild(el("p", null, "I've attempted:"));
      descDiv.appendChild(attempts);
      cont.appendChild(descDiv);
      const builtUI = formEngine.buildUI([{
        label: APP_NAME+"'s HTTP"+S+" endpoint",
        type: "datalist",
        datalist: urls,
        help: "Please specify the url at which "+APP_NAME+"'s HTTP"+S+" endpoint can be reached.",
        validate: ["required",function(val,me){
          if (val.length < 4) { return; }
          if (val.slice(0,4) != "http") {
            return {
              msg: "The url to "+APP_NAME+" should probably start with "+location.protocol+"//, for example: <br>"+parseURL(location.origin,{port:location.protocol == "https:" ? 4433 : 8080,pathname:""}).full,
              "break": false,
              classes: ['orange']
            }
          }
          if (val.slice(0,5) != location.href.slice(0,5)) {
            return {
              msg: "It looks like you're attempting to connect to "+APP_NAME+" using "+parseURL(val).protocol+", while this page has been loaded over "+parseURL(location.href).protocol+". Your browser may refuse this because it is insecure.",
              "break": false,
              classes: ['orange']
            }
          }
        }],
        pointer: { main: save, index: "url" }
      },{
        type: "buttons",
        buttons: [{
          type: "save",
          label: "Connect",
          "function": function(){
            let url = save.url;
            if (url[url.length-1] != "/") { url += "/"; }
            cont.hidden = true;
            on_success(url);
          }
        }]
      }]);
      if (builtUI instanceof HTMLElement) cont.appendChild(builtUI);
      if (morehelp) cont.appendChild(morehelp);
    }


    function on_success(url) {
      if (url != sockets.http_host) {
        sockets.http_host = url;
        mistHelpers.stored.set("HTTP"+(url.slice(0,5) == "https" ? "S" : "")+"Url",url);
      }

      on_success_callback.call(cont,url);

    }

    cont.getUrls = function(kind){
      return cont.urls;
    }

    findHTTP(function(result){
      if (result && result.length) {
        cont.hidden = true;
        on_success(result[0]);
      } else {
        on_fail(result || []);
      }
    });

    return cont;
  },
  livestreamhint: function(streamname){
    const cont = el("div", {class: "livestreamhint"});

    const settings = mist.data.streams[streamname.split("+")[0]];
    if (settings?.source && (settings.source.slice(0,1) != "/") && (!settings.source.match(/-exec:/))) {
      if ("streamkeys" in mist.data) {
        streamHints.updateLiveStreamHint(streamname,settings.source,cont);
      }
      else {
        apiClient.send(function(){
          streamHints.updateLiveStreamHint(streamname,settings.source,cont);
        },{streamkeys: true});
      }
    }
    else {
      return false;
    }

    return cont;
  },
  status: function(streamname,options){
    const defaultoptions = {
      livestreamhint: true,
      status: true,
      stats: true,
      slab: false,
      title: "Stream status",
      titleIcon: "activity",
      tags: {
        readonly: false,
        onclick: false
      },
      thumbnail: true
    };
    if (!options) {
      options = {};
    }
    if (options.tags == "readonly") { options.tags = { readonly: true }; }
    if (typeof options.tags == "object") {
      options.tags = Object.assign({},defaultoptions.tags,options.tags);
    }
    options = Object.assign(defaultoptions,options);

    const statusDiv = options.status ? el("div", {"data-streamstatus": "0"}) : false;
    if (statusDiv) { statusDiv.textContent = "Inactive"; statusDiv.hidden = true; }

    let addtagDiv = false;
    if (options.tags && (!options.tags.readonly)) {
      addtagDiv = el("div", {class: "input_container", title: "Add a tag to this stream's current tags.\nNote that tags added here will not be applied when the stream restarts. To do that, add the tag through the stream settings.", style: {display: "block"}});
      const showIfInactive = el("span", {showifstate: "0"});
      showIfInactive.textContent = "Transient tags can be added here once the stream is active.";
      addtagDiv.appendChild(showIfInactive);
      const tagInput = el("input", {showifstate: "1", placeholder: "Tag name"});
      tagInput.addEventListener("keydown", function(e){
        switch (e.key) {
          case " ": {
            e.preventDefault();
            break;
          }
          case "Enter": {
            this.parentNode.querySelector("button").click();
            break;
          }
        }
      });
      addtagDiv.appendChild(tagInput);
      const tagBtn = el("button", {showifstate: "1"});
      tagBtn.textContent = "Add transient tag";
      tagBtn.addEventListener('click', function(){
        const addtag = {};
        const me = this;
        const inp = me.parentNode.querySelector("input");
        const save = {tag_stream:{}};
        save.tag_stream[streamname] = inp.value;
        me.textContent = "Adding..";
        apiClient.send(function(d){
          me.textContent = "Add tag";
          inp.value = "";
        },save);
      });
      addtagDiv.appendChild(tagBtn);
    }

    const activestream = {
      mode: 0,
      cont: el("div", {class: "activestream", "data-state": "0"}),
      status: statusDiv,
      viewers: options.stats ? el("span", {beforeifnotempty: "Viewers: "}) : false,
      inputs: options.stats ? el("span", {beforeifnotempty: "Inputs: "}) : false,
      outputs: options.stats? el("span", {beforeifnotempty: "Outputs: "}) : false,
      context_menu: false,
      tags: false,
      addtag: addtagDiv,
      livestreamhint: options.livestreamhint ? stream.livestreamhint(streamname) : false
    };

    activestream.tags = false;
    if (options.tags) {
      if (!options.tags.readonly) {
        activestream.context_menu = new uiCore.context_menu();
      }
      activestream.tags = stream.tags({
        streamname: streamname,
        readonly: options.tags.readonly,
        onclick: options.tags.onclick,
        context_menu: activestream.context_menu,
        getStreamstatus: function(){
          return this.closest("[data-state]").getAttribute("data-state");
        }
      });
      activestream.tags.update();
    }


    if (activestream.status) activestream.cont.appendChild(activestream.status);
    if (activestream.viewers) activestream.cont.appendChild(activestream.viewers);
    if (activestream.inputs) activestream.cont.appendChild(activestream.inputs);
    if (activestream.outputs) activestream.cont.appendChild(activestream.outputs);
    if (activestream.tags) activestream.cont.appendChild(activestream.tags);
    if (activestream.addtag) activestream.cont.appendChild(activestream.addtag);
    if (options.thumbnail) {
      const thumb = stream.thumbnail(streamname);
      if (thumb) activestream.cont.appendChild(thumb);
    }
    if (activestream.livestreamhint) activestream.cont.appendChild(activestream.livestreamhint);

    if (activestream.context_menu) {
      activestream.cont.style.position = "relative";
      activestream.cont.appendChild(activestream.context_menu.ele);
    }

    sockets.ws.active_streams.subscribe(function(type,data){
      if (options.status) activestream.status.style.display = "";
      if (type == "stream") {
        if (data[0] == streamname) {

          activestream.cont.setAttribute("data-state",data[1]);

          const s = ["Inactive","Initializing","Booting","Waiting for data","Available","Shutting down","Invalid state"];
          if (options.status) { activestream.status.setAttribute("data-streamstatus",data[1]); activestream.status.textContent = s[data[1]]; }
          if (options.stats) {
            activestream.viewers.textContent = data[2];
            activestream.inputs.textContent = data[3] > 0 ? data[3] : "";
            activestream.outputs.textContent = data[4] > 0 ? data[4] : "";
          }
          if (options.tags) {
            activestream.tags.update({stats:data});
          }

          if (activestream.livestreamhint) {
            activestream.livestreamhint.hidden = false;
          }

        }
      }
      else if (type == "error") {
        if (options.status) {
          activestream.status.setAttribute("data-streamstatus",6);
          activestream.status.textContent = data;
        }
      }
    });

    if (options.slab) {
      const slabSection = createStreamSlab({
        className: "activestream stream-slab--status",
        title: options.title,
        icon: options.titleIcon
      });
      slabSection.slabBody.appendChild(activestream.cont);
      return slabSection;
    }

    const section = el("section", {class: "activestream"});
    section.appendChild(activestream.cont);
    return section;
  },
  actions: function(currenttab,streamname){
    const section = el("section", {class: "actions bigbuttons"});
    const stopBtn = el("button", {"data-icon": "stop", title: "Disconnect sessions for this stream. Disconnecting a session will kill any currently open connections (viewers, pushes and possibly the input). If the USER_NEW trigger is in use, it will be triggered again by any reconnecting connections."});
    stopBtn.textContent = "Stop all sessions";
    stopBtn.addEventListener('click', function(){
      if (confirm("Are you sure you want to disconnect all sessions (viewers, pushes and possibly the input) from this stream?")) {
        apiClient.send(function(){},{stop_sessions:streamname});
      }
    });
    section.appendChild(stopBtn);
    const invalBtn = el("button", {"data-icon": "invalidate", title: "Invalidate all the currently active sessions for this stream. This has the effect of re-triggering the USER_NEW trigger, allowing you to selectively close some of the existing connections after they have been previously allowed. If you don't have a USER_NEW trigger configured, this will not have any effect."});
    invalBtn.textContent = "Invalidate sessions";
    invalBtn.addEventListener('click', function(){
      if (confirm("Are you sure you want to invalidate all sessions for the stream '"+streamname+"'?\nThis will re-trigger the USER_NEW trigger.")) {
        apiClient.send(function(){},{ invalidate_sessions:streamname});
      }
    });
    section.appendChild(invalBtn);
    const nukeBtn = el("button", {"data-icon": "nuke", title: "Shut down a running stream completely and/or clean up any potentially left over stream data in memory. It attempts a clean shutdown of the running stream first, followed by a forced shut down, and then follows up by checking for left over data in memory and cleaning that up if any is found."});
    nukeBtn.textContent = "Nuke stream";
    nukeBtn.addEventListener('click', function(){
      if (confirm("Are you sure you want to completely shut down the stream '"+streamname+"'?\nAll viewers will be disconnected.")) {
        apiClient.send(function(){
          tabView.showTab(currenttab,streamname);
        },{nuke_stream:streamname});
      }
    });
    section.appendChild(nukeBtn);
    return section;
  },
  thumbnail: function(streamname,options){
    let image, stream;

    const jpg = el("img");
    const thumbSection = el("section", {class: "thumbnail"});
    let clone;
    if (options && options.clone) clone = el("img", {class: "clone"});


    if (streamname[0] == "/") {
      return;
    }
    else {

      if (!findOutput("JPG")) { return; }

      jpg.addEventListener('mouseenter', function(){
        if (image && stream) {
          this.setAttribute("src",stream);
        }
      });
      jpg.addEventListener('mouseleave', function(){
        if (image && stream) {
          this.setAttribute("src",image);
        }
      });

      sockets.ws.info_json.subscribe(function(data){
        if (!data.source) return;
        for (const i in data.source) {
          if (data.source[i].type == "html5/image/jpeg") {
            const url = data.source[i].url;
            if (url.indexOf(".mjpg") > -1) { stream = url; }
            else { image = url; }
            if (image && stream) { break; }
          }
        }
        if (stream || image) {
          if (!stream) stream = image;
          else if (!image) image = stream;

          jpg.setAttribute("src",image);
          thumbSection.style.setProperty("--src","url('"+image+"')");
          if (clone) clone.setAttribute("src",image);
        }
      },streamname);
    }

    thumbSection.appendChild(jpg);
    if (clone) thumbSection.appendChild(clone);
    return thumbSection;
  },
  metadata: function(streamname,options){
    const defaultoptions = {
      tracktable: true,
      tracktiming: true
    };
    if (!options) {
      options = {};
    }
    options = Object.assign(defaultoptions,options);

    const meta = el("div", {class: "meta"});
    meta.textContent = "Loading..";

    const mainDyn = dynamicUI.dynamic({
      create: function(){
        const cont = el("span");

        cont.main = dynamicUI.dynamic({
          create: function(){
            const main = el("div", {class: "main input_container"});
            main.type = el("span");
            const typeLabel = el("label");
            typeLabel.appendChild(el("span", null, "Type:"));
            typeLabel.appendChild(main.type);
            main.appendChild(typeLabel);
            main.buffer = el("span");
            const bufLabel = el("label", {"data-liveonly": ""});
            bufLabel.appendChild(el("span", null, "Buffer window:"));
            bufLabel.appendChild(main.buffer);
            main.appendChild(bufLabel);
            main.jitter = el("span");
            const jitLabel = el("label", {"data-liveonly": ""});
            jitLabel.appendChild(el("span", null, "Jitter:"));
            jitLabel.appendChild(main.jitter);
            main.appendChild(jitLabel);
            if (options.tracktiming) {
              main.timing = dynamicUI.dynamic({
                create: function(){
                  const graph = el("div", {class: "tracktiming"});

                  graph.box = el("div", {class: "boxcont"});
                  graph.box.label = el("span", {class: "center"});
                  graph.box.left = el("span", {class: "left"});
                  graph.box.right = el("span", {class: "right"});
                  const boxInner = el("div", {class: "box"});
                  graph.box.appendChild(boxInner);
                  graph.box.appendChild(graph.box.label);
                  graph.box.appendChild(graph.box.left);
                  graph.box.appendChild(graph.box.right);

                  graph.appendChild(graph.box);

                  graph.bounds = null;
                  graph.mstopos = function(value){
                    if (!this.bounds) { return 25; }
                    const pos = (value - this.bounds.firstms.min) / (this.bounds.lastms.max - this.bounds.firstms.min) * 100;
                    return pos;
                  };
                  graph.mstosize = function(value){
                    if (!this.bounds) { return 10; }
                    return value / (this.bounds.lastms.max - this.bounds.firstms.min) * 100
                  };

                  return graph;
                },
                add: {
                  create: function(id){
                    const row = el("div", {class: "track", "data-track": id});
                    row.label = el("label");
                    row.box = el("div", {class: "box"});
                    row.jitter = el("div", {class: "jitter"});
                    row.box.appendChild(row.jitter);
                    row.left = el("span", {class: "left", beforeifnotempty: "-"});
                    row.right = el("span", {class: "right", beforeifnotempty: "+"});
                    row.box.appendChild(row.left);
                    row.box.appendChild(row.right);
                    row.appendChild(row.box);
                    row.appendChild(row.label);
                    return row;
                  },
                  update: function(values){
                    if (values.type != this.raw_type) {
                      this.setAttribute("data-type",values.type);
                    }
                    if (values.type != this.raw_tracktype) {
                      this.setAttribute("title",format.capital(values.type));
                      this.raw_tracktype = values.type;
                    }
                    if (this.label.raw != values.idx+values.type+values.codec) {
                      this.label.textContent = "Track "+values.idx+" ("+values.codec+")";
                      this.label.raw = values.idx+values.type+values.codec;
                      this.style.order = values.idx;
                    }
                    if (this.jitter.raw != values.jitter) {
                      this.jitter.setAttribute("title","Jitter: "+values.jitter+"ms");
                      this.jitter.raw = values.jitter;
                    }

                    const lastmsindex = "nowms" in values ? "nowms" : "lastms";

                    const from = main.timing.bounds.firstms.max;
                    const to = main.timing.bounds.lastms.min;

                    this.box.style.left = main.timing.mstopos(values.firstms)+"%";
                    this.box.style.right = (100-main.timing.mstopos(values[lastmsindex]))+"%";
                    if (from >= to) {
                      if (values.firstms > to) {
                        this.left.innerHTML = format.duration((from - values.firstms)*1e-3,true);
                      }
                      else {
                        this.left.innerHTML = format.duration((to - values.firstms)*1e-3,true);
                      }
                      if (values.lastms > from) {
                        this.right.innerHTML = format.duration((values[lastmsindex] - from)*1e-3,true);
                      }
                      else {
                        this.right.innerHTML = format.duration((values[lastmsindex] - to)*1e-3,true);
                      }
                    }
                    else {
                      this.left.innerHTML = format.duration((from - values.firstms)*1e-3,true);
                      this.right.innerHTML = format.duration((values[lastmsindex] - to)*1e-3,true);

                    }

                    this.jitter.style.width = (values.jitter / (values[lastmsindex] - values.firstms) * 100)+"%";
                  }
                },
                getEntries: function(values){
                  const out = {};

                  const skipmeta = !main.includemeta.checked;
                  for (const i in values) {
                    if (skipmeta && (values[i].type == "meta")) { continue; }
                    out[i] = values[i];
                  }
                  const bounds = {
                    firstms: {
                      min: 1e24,
                      max: -1e24
                    },
                    lastms: {
                      min: 1e24,
                      max: -1e24
                    }
                  };
                  for (const i in out) {
                    const lastmsindex = "nowms" in out[i] ? "nowms" : "lastms";
                    bounds.firstms.min = Math.min(bounds.firstms.min,out[i].firstms);
                    bounds.firstms.max = Math.max(bounds.firstms.max,out[i].firstms);
                    bounds.lastms.min = Math.min(bounds.lastms.min,out[i][lastmsindex]);
                    bounds.lastms.max = Math.max(bounds.lastms.max,out[i][lastmsindex]);
                  }
                  main.timing.bounds = bounds;
                  for (const i in out) {
                    out[i].bounds = bounds;
                  }

                  return out;
                },
                update: function(values){
                  const from = main.timing.bounds.firstms.max;
                  const to = main.timing.bounds.lastms.min;

                  this.box.label.innerHTML = format.duration(Math.abs(from - to)*1e-3,true);
                  this.box.querySelector(".box").style.setProperty("--ntracks",Object.keys(values).length);

                  if (from < to) {
                    this.box.style.marginLeft = main.timing.mstopos(from)+"%";
                    this.box.style.marginRight = (100-main.timing.mstopos(to))+"%";
                    this.box.left.innerHTML = format.duration(from*1e-3);
                    this.box.right.innerHTML = format.duration(to*1e-3);
                    this.box.classList.remove("gap");
                  }
                  else {
                    this.box.style.marginLeft = main.timing.mstopos(to)+"%";
                    this.box.style.marginRight = (100-main.timing.mstopos(from))+"%";
                    this.box.left.innerHTML = format.duration(to*1e-3);
                    this.box.right.innerHTML = format.duration(from*1e-3);
                    this.box.classList.add("gap");
                  }

                  if (this.box.offsetWidth < 40) {
                    this.box.classList.add("toosmall");
                  }
                  else {
                    this.box.classList.remove("toosmall");
                  }
                }
              });
              main.includemeta = el("input", {type: "checkbox"});
              main.includemeta.addEventListener('click', function(){
                cont.main.timing.update(cont.main.timing.values_orig);
              });
              const inclLabel = el("label");
              inclLabel.appendChild(el("span", null, "Include metadata in graph:"));
              inclLabel.appendChild(main.includemeta);
              main.appendChild(inclLabel);
              const timingLabel = el("label");
              timingLabel.appendChild(el("span", null, "Track timing:"));
              main.appendChild(timingLabel);
              main.appendChild(main.timing);
            }
            if (options.tracktable) {
              main.tracks = dynamicUI.dynamic({
                create: function(){
                  return el("div", {class: "tracks"});
                },
                getEntries: function(tracks){
                  const out = {
                    audio: {},
                    video: {},
                    subtitle: {},
                    meta: {}
                  };
                  if (tracks) {
                    const sorted = Object.keys(tracks).sort(function(a,b){
                      return tracks[a].idx - tracks[b].idx;
                    });

                    for (const i in sorted) {
                      const track = tracks[sorted[i]];
                      const type = track.codec == "subtitle" ? "subtitle" : track.type;
                      out[type][sorted[i]] = track;
                      track.nth = Object.values(out[type]).length;
                    }
                  }
                  return out;
                },
                add: function(id){
                  const tt = dynamicUI.dynamic({
                    create: function(id){
                      const table = el("table", {style: {width: "auto"}});
                      table._rows = [];
                      const headers = {
                        audio: {
                          vheader: 'Audio',
                          labels: ['Codec','Duration','Jitter','Avg bitrate','Peak bitrate','Channels','Samplerate','Language','Player track index','Track id']
                        },
                        video: {
                          vheader: 'Video',
                          labels: ['Codec','Duration','Jitter','Avg bitrate','Peak bitrate','Size','Framerate','Language','Player track index','Track id','Has B-Frames']
                        },
                        subtitle: {
                          vheader: 'Subtitles',
                          labels: ['Codec','Duration','Jitter','Avg bitrate','Peak bitrate','Language','Player track index','Track id']
                        },
                        meta: {
                          vheader: 'Metadata',
                          labels: ['Codec','Duration','Jitter','Avg bitrate','Peak bitrate','Track id']
                        }
                      };
                      table.headers = headers[id].labels;
                      table.header = el("tr", {class: "header"});
                      const vheaderTd = el("td", {class: "vheader", rowspan: headers[id].labels.length+1});
                      vheaderTd.appendChild(el("span", null, headers[id].vheader));
                      table.header.appendChild(vheaderTd);
                      table.header.appendChild(el("td"));
                      const tbody = el("tbody");
                      tbody.appendChild(table.header);
                      for (const i in headers[id].labels) {
                        const tr = el("tr", {"data-label": headers[id].labels[i]});
                        tr.appendChild(el("td", null, headers[id].labels[i]+":"));
                        table._rows.push(tr);
                        tbody.appendChild(tr);
                      }

                      table.appendChild(tbody);
                      return table;
                    },
                    add: {
                      create: function(id,parent){
                        const header = el("td");
                        const tds = [];
                        for (const i in tt.headers) {
                          tds.push(el("td"));
                        }

                        return {
                          header: header,
                          cells: tds,
                          customAdd: function(table){
                            table.header.appendChild(this.header);
                            for (const i in this.cells) {
                              table._rows[i].appendChild(this.cells[i]);
                            }
                          },
                          remove: function(){
                            this.header.remove();
                            for (const i in this.cells) {
                              this.cells[i].remove();
                            }
                          }
                        };
                      },
                      update: function(track){
                        function getValues(track) {
                          function peakoravg (track,key) {
                            if ("maxbps" in track) {
                              return format.bits(track[key]*8,1);
                            }
                            else {
                              if (key == "maxbps") {
                                return format.bits(track.bps*8,1);
                              }
                              return "unknown";
                            }
                          }
                          function displayDuration(track){
                            if ((track.firstms == 0) && (track.lastms == 0)) {
                              return "\u00AB No data \u00BB";
                            }
                            let lastmsindex = "lastms";
                            if ("nowms" in track) {
                              lastmsindex = "nowms";
                            }
                            let out;
                            out = format.duration((track.lastms-track.firstms)/1000);
                            out += '<br><span class=description>'+format.duration(track.firstms/1000)+' to '+format.duration(track[lastmsindex]/1000)+'</span>';
                            return out;
                          }
                          const type = track.codec == "subtitle" ? "subtitle" : track.type;
                          switch (type) {
                            case 'audio':
                              return {
                                header: 'Track '+track.idx,
                                body: [
                                  track.codec,
                                  displayDuration(track),
                                  format.addUnit(format.number(track.jitter),"ms"),
                                  peakoravg(track,"bps"),
                                  peakoravg(track,"maxbps"),
                                  track.channels,
                                  format.addUnit(format.number(track.rate),'Hz'),
                                  ('language' in track ? track.language : 'unknown'),
                                  track.nth,
                                  track.trackid
                                ]
                              };
                              break;
                            case 'video':
                              return {
                                header: 'Track '+track.idx,
                                body: [
                                  track.codec,
                                  displayDuration(track),
                                  format.addUnit(format.number(track.jitter),"ms"),
                                  peakoravg(track,"bps"),
                                  peakoravg(track,"maxbps"),
                                  format.addUnit(track.width,'x ')+format.addUnit(track.height,'px'),
                                  (track.fpks == 0 ? "variable" : format.addUnit(format.number(track.fpks/1000),'fps')),
                                  ('language' in track ? track.language : 'unknown'),
                                  track.nth,
                                  track.trackid,
                                  ("bframes" in track ? "yes" : "no")
                                ]
                              }
                              break;
                            case 'subtitle':
                              return {
                                header: 'Track '+track.idx,
                                body: [
                                  track.codec,
                                  displayDuration(track),
                                  format.addUnit(format.number(track.jitter),"ms"),
                                  peakoravg(track,"bps"),
                                  peakoravg(track,"maxbps"),
                                  ('language' in track ? track.language : 'unknown'),
                                  track.nth,
                                  track.trackid
                                ]
                              }
                              break;
                            case "meta":
                              return {
                                header: 'Track '+track.idx,
                                body: [
                                  track.codec,
                                  displayDuration(track),
                                  format.addUnit(format.number(track.jitter),"ms"),
                                  peakoravg(track,"bps"),
                                  peakoravg(track,"maxbps"),
                                  track.trackid
                                ]
                              }
                              break;
                          }
                        }
                        const values = getValues(track);

                        if (this.header.raw != values.header) {
                          this.header.textContent = values.header;
                          this.header.raw = values.header;
                        }
                        for (const i in this.cells) {
                          const bodyVal = values.body[i];
                          const renderKey = streamCellRenderKey(bodyVal);
                          if (this.cells[i].raw != renderKey) {
                            setStreamCellValue(this.cells[i], bodyVal);
                            this.cells[i].raw = renderKey;
                          }
                        }

                      }
                    },
                    update: function(){
                      if (Object.values(this._children).length) {
                        this.hidden = false;
                      }
                      else {
                        this.hidden = true;
                      }
                    },
                    getEntries: function(original){
                      let out = {};
                      for (const i in original) {
                        out[original[i].idx] = original[i]
                      }
                      return out;
                    }
                  },id);
                  return tt;
                },
                update: function(){}
              });
              const tracksLabel = el("label");
              tracksLabel.appendChild(el("span", null, "Track metadata:"));
              main.appendChild(tracksLabel);
              main.appendChild(main.tracks);
            }

            return main;

          },
          update: function(d){
            const main = this;
            if (main.type.raw != d.type) {
              main.type.textContent = d.type == "live" ? "Live" : "Video on demand";
              main.type.raw = d.type;
            }
            if (d.meta) {
              if (main.buffer.raw != d.meta.buffer_window) {
                main.buffer.innerHTML = d.meta.buffer_window ? format.addUnit(format.number(d.meta.buffer_window*1e-3),"s") : "N/A";
                main.buffer.raw = d.meta.buffer_window;
              }
              if (main.jitter.raw != d.meta.jitter) {
                main.jitter.innerHTML = d.meta.jitter ? format.addUnit(format.number(d.meta.jitter),"ms") : "N/A";
                main.jitter.raw = d.meta.jitter;
              }
              main.tracks.update(d.meta.tracks);
              main.timing.update(d.meta.tracks);
            }
          }
        });
        cont.audio = el("table"); cont.audio.hidden = true;
        cont.video = el("table"); cont.video.hidden = true;
        cont.subtitle = el("table"); cont.subtitle.hidden = true;
        cont.metadata = el("table"); cont.metadata.hidden = true;


        cont.appendChild(cont.main);
        cont.appendChild(cont.audio);
        cont.appendChild(cont.video);
        cont.appendChild(cont.subtitle);
        cont.appendChild(cont.metadata);
        return cont;
      },
      update: function(info){
        this.main.update(info);
        if (this.raw_type != info.type) {
          this.setAttribute("data-type",info.type);
          this.raw_type = info.type;
        }
      }
    });

    function ondata(data) {
      if (!data || mainDyn.freeze) { return; }

      if (data.error && (!meta.rawdata || meta.rawdata.type == "live")) {
        meta.innerHTML = "";
        meta.setAttribute("onempty",data.error+".");
      }
      else {
        if ((meta.rawdata && (meta.rawdata.type == "live")) || !data.error) {
          meta.rawdata = data;
        }
        if (!mainDyn.parentNode) {
          meta.innerHTML = '';
          const btnDiv = el("div");
          const rawBtn = el("button", null, "Open raw json");
          rawBtn.addEventListener('click', function(){
            const tab = window.open(null, "Stream info json for "+streamname);
            tab.document.write(
              "<html><head><title>Raw stream metadata for '"+streamname+"'</title><meta http-equiv=\"content-type\" content=\"application/json; charset=utf-8\"></head><body><code><pre>"+JSON.stringify(meta.rawdata,null,2)+"</pre></code></body></html>"
            );
            tab.document.close();
          });
          btnDiv.appendChild(rawBtn);
          const freezeBtn = el("button", {title: "Pauze updating the stream metadata information below"}, "Freeze");
          freezeBtn.addEventListener('click', function(){
            if (this.textContent == "Freeze") {
              this.textContent = "Frozen";
              this.style.background = "#bbb";
              mainDyn.freeze = true;
            }
            else {
              this.textContent = "Freeze";
              this.style.background = "";
              mainDyn.freeze = false;
            }
          });
          btnDiv.appendChild(freezeBtn);
          meta.appendChild(btnDiv);
          meta.appendChild(mainDyn);
        }

        function buildTrackinfo(info) {
          if (mainDyn.freeze) { return; }

          const metaD = info.meta;
          mainDyn.update(info);
        }
        buildTrackinfo(data);
      }

    }

    let firsttime = true;
    sockets.ws.info_json.subscribe(function(d){
      ondata(d);

      if (firsttime && !d.error) {
        firsttime = false;
        if (d.type == "live") {
          UI.interval.set(function(){
            fetch(sockets.http_host + "json_"+streamname+".js").then(function(r){
              if (!r.ok) throw new Error(r.statusText);
              return r.json();
            }).then(ondata).catch(function(){});
          },5e3);
        }

      }
    },streamname);


    const metaSection = createStreamSlab({
      className: "meta stream-slab--metadata",
      title: "Stream metadata",
      icon: "scan-search"
    });
    metaSection.slabBody.appendChild(meta);
    return metaSection;
  },
  processes: function(streamname){
    const procCont = el("div", {class: "processes", onempty: "None."});

    const layout = {
      "Process type:": function(d){ const b = el("b"); b.textContent = d.process; return b; },
      "Source:": function(d){
        const s = el("span");
        s.textContent = d.source;
        if (d.source_tracks && d.source_tracks.length) {
          s.appendChild(el("span", {class: "description"}, " track "+(d.source_tracks.slice(0,-2).concat(d.source_tracks.slice(-2).join(" and "))).join(", ")));
        }
        return s;
      },
      "Sink:": function(d){
        const s = el("span");
        s.textContent = d.sink;
        if (d.sink_tracks && d.sink_tracks.length) {
          s.appendChild(el("span", {class: "description"}, " track "+(d.sink_tracks.slice(0,-2).concat(d.sink_tracks.slice(-2).join(" and "))).join(", ")));
        }
        return s;
      },
      "Active for:": function(d){
        const since = new Date().setSeconds(new Date().getSeconds() - d.active_seconds);
        const s = el("span");
        const durSpan = el("span");
        durSpan.innerHTML = format.duration(d.active_seconds);
        s.appendChild(durSpan);
        s.appendChild(el("span", {class: "description"}, " since "+format.time(since/1e3)));
        return s;
      },
      "Pid:": function(d,i){ return i; },
      "Logs:": function(d){
        const logs = el("div");
        logs.textContent = "None.";
        if (d.logs && d.logs.length) {
          logs.innerHTML = "";
          logs.classList.add("description","logs");
          Object.assign(logs.style, {maxHeight: "6em", display: "flex", flexFlow: "column-reverse nowrap"});
          for (const i in d.logs) {
            const item = d.logs[i];
            const logDiv = el("div");
            logDiv.appendChild(document.createTextNode(format.time(item[0])+' ['+item[1]+'] '+item[2]));
            logs.insertBefore(logDiv, logs.firstChild);
          }
        }
        return logs;
      },
      "Additional info:": function(d){
        let t;
        if (("ainfo" in d) && d.ainfo && Object.keys(d.ainfo).length) {
          t = el("table");
          let capa = false;
          if (d.process in mist.data.capabilities.processes){
            capa = mist.data.capabilities.processes[d.process];
          }
          if (d.process+".exe" in mist.data.capabilities.processes){
            capa = mist.data.capabilities.processes[d.process+".exe"];
          }
          for (const i in d.ainfo) {
            let legend = false;
            if (capa && capa.ainfo && capa.ainfo[i]) {
              legend = capa.ainfo[i];
            }
            if (!legend) {
              legend = {
                name: i
              };
            }
            const tr = el("tr");
            tr.appendChild(el("th", null, legend.name+":"));
            const td = el("td");
            td.innerHTML = d.ainfo[i];
            if (legend.unit) td.appendChild(el("span", {class: "unit"}, legend.unit));
            tr.appendChild(td);
            t.appendChild(tr);
          }
        }
        else { t = el("span", {class: "description"}, "N/A"); }
        return t;
      }
    };
    const processes = dynamicUI.dynamic({
      create: function(){
        const table = el("table");
        table._rows = {};
        for (const i in layout) {
          const row = el("tr");
          const th = el("td", {style: {verticalAlign: "top"}});
          th.textContent = i;
          row.appendChild(th);
          table.appendChild(row);
          table._rows[i] = row;
        }
        return table;
      },
      add: {
        create: function(){
          const dummy = el("div");
          dummy.hidden = true;
          dummy.remove = function(){
            for (const i in this._children) {
              this._children[i].remove();
            }
          };
          return dummy;
        },
        add: {
          create: function(i){
            return el("td", {style: {verticalAlign: "top"}});
          },
          update: function(d){
            if (typeof d === 'string' || typeof d === 'number') {
              this.innerHTML = d;
            } else if (d instanceof HTMLElement) {
              this.innerHTML = '';
              this.appendChild(d);
            } else {
              this.innerHTML = d;
            }
          }
        },
        getEntries: function(d,id){
          const out = {};
          for (const i in layout) {
            let v = layout[i](d,id);
            if (v instanceof HTMLElement) {
              v = v.outerHTML;
            }
            out[i] = v;
          }
          return out;
        },
        update: function(d){
          for (const i in this._children) {
            if (!this._children[i].moved) {
              processes._rows[i].appendChild(this._children[i]);
              this._children[i].moved = true;
            }
          }

          return;
        }
      },
      update: function(d){
        if (Object.keys(d).length) {
          if (!processes.parentNode) {
            const firstTr = processes.querySelector("tr");
            if (firstTr) firstTr.classList.add("header");
            procCont.appendChild(processes);
          }
        }
        else {
          processes.remove();
        }
      }
    });

    sockets.http.api.subscribe(function(d){
      if (d.proc_list) {
        processes.update(d.proc_list);
      }
    },{proc_list:streamname});


    const procSection = createStreamSlab({
      className: "processes stream-slab--processes",
      title: "Processes",
      icon: "cpu"
    });
    procSection.slabBody.appendChild(procCont);
    return procSection;
  },
  triggers: function(streamname,tab){
    const triggersDiv = el("div", {class: "triggers", onempty: "None."});

    if (mist.data.config.triggers && Object.keys(mist.data.config.triggers).length) {
      const table = el("table");
      const tbody = el("tbody");
      const rowNames = ["type","streams","handler","blocking","response","actions"];
      const rowLabels = ["Event:","Applies to:","Handler:","Blocking:","Default response:","Actions:"];
      const trs = [];
      for (let ri = 0; ri < rowNames.length; ri++) {
        const tr = el("tr", {class: rowNames[ri]});
        tr.appendChild(el("th", null, rowLabels[ri]));
        tbody.appendChild(tr);
        trs.push(tr);
      }
      table.appendChild(tbody);
      triggersDiv.appendChild(table);

      let count = 0;
      for (const trigger_type in mist.data.config.triggers) {
        const list = mist.data.config.triggers[trigger_type];
        for (const i in list) {
          const trigger = list[i];
          if (trigger.streams.length) {
            if (trigger.streams.indexOf(streamname) < 0) {
              if (trigger.streams.indexOf(streamname.split("+")[0]) < 0) {
                continue;
              }
            }
          }
          count++;
          for (let n = 0; n < trs.length; n++) {
            const td = el("td");
            switch (n) {
              case 0: {
                td.appendChild(el("b", null, trigger_type));
                break;
              }
              case 1: {
                td.textContent = trigger.streams.length ? trigger.streams.join(", ") : "All streams";
                break;
              }
              case 2: {
                td.textContent = trigger.handler;
                break;
              }
              case 3: {
                td.textContent = trigger.sync ? "Blocking" : "Non-blocking";
                break;
              }
              case 4: {
                td.textContent = trigger['default'] ? trigger['default'] : "true";
                break;
              }
              case 5: {
                td.classList.add("buttons");
                td.setAttribute("data-index",i);
                td.setAttribute("data-type",trigger_type);
                const editBtn = el("button", null, "Edit");
                editBtn.addEventListener('click', function(){
                  const t = this.closest(".buttons");
                  editAutomation(t.getAttribute("data-type")+","+t.getAttribute("data-index"), function(){
                    navto(tab, streamname);
                  });
                });
                td.appendChild(editBtn);
                if (trigger.streams.length > 1) {
                  const removeFromBtn = el("button", null, "Remove from stream");
                  removeFromBtn.addEventListener('click', function(){
                    const t = this.closest(".buttons");
                    const type = t.getAttribute("data-type");
                    const streams = mist.data.config.triggers[type][t.getAttribute("data-index")].streams;
                    let idx = streams.indexOf(streamname);
                    if (idx >= 0) {
                      streams.splice(idx,1);
                    }
                    else {
                      idx = streams.indexOf(streamname.split("+")[0]);
                      if (idx >= 0) {
                        streams.splice(idx,1);
                      }
                    }
                    apiClient.send(function(){
                      navto(tab,streamname);
                    },{config:mist.data.config});
                  });
                  td.appendChild(removeFromBtn);
                } else {
                  const removeBtn = el("button", null, "Remove");
                  removeBtn.addEventListener('click', function(){
                    const t = this.closest(".buttons");
                    const type = t.getAttribute("data-type");
                    if (confirm('Are you sure you want to delete this ' + type + ' automation rule?')) {
                      mist.data.config.triggers[type].splice(t.getAttribute("data-index"),1);
                      if (mist.data.config.triggers[type].length == 0) {
                        delete mist.data.config.triggers[type];
                      }

                      apiClient.send(function(d){
                        navto(tab,streamname);
                      },{config:mist.data.config});
                    }
                  });
                  td.appendChild(removeBtn);
                }
                break;
              }
            }
            trs[n].appendChild(td);
          }

        }
      }
      if (count == 0) {
        table.remove();
      }

    }

    const addBtn = el("button", null, "Add automation rule");
    addBtn.addEventListener('click', function(){
      editAutomation('', function(){
        navto(tab, streamname);
      });
    });
    const trigSection = createStreamSlab({
      className: "triggers stream-slab--triggers",
      title: "Automation rules",
      icon: "workflow",
      actions: addBtn
    });
    trigSection.slabBody.appendChild(triggersDiv);
    return trigSection;
  },
  pushes: function(streamname){
    const pushSection = pushesTab({
      stream: streamname,
      logs: false,
      stop_pushes: false
    });
    let pushBody = pushSection;
    if (pushSection && pushSection.querySelector) {
      const nested = pushSection.querySelector("div.pushes");
      if (nested) {
        pushBody = nested;
      }
    }
    const slab = createStreamSlab({
      className: "pushes stream-slab--pushes",
      title: "Pushes and recordings",
      icon: "send"
    });
    var newPushBtn = el('button', {'data-icon': 'plus'}, 'New push');
    newPushBtn.addEventListener('click', function() {
      navto('Push Config', streamname);
    });
    slab.slabBody.appendChild(newPushBtn);
    slab.slabBody.appendChild(pushBody);
    return slab;
  },
  logs: function(streamname, options){
    return logs(streamname, options);
  },
  accesslogs: function(streamname){
    const accesslogs = el("div", {class: "accesslogs", onempty: "None."});

    let tab = false;

    sockets.ws.active_streams.subscribe(function(type,data){
      if (type == "access") {
        const scroll = (accesslogs.scrollTop >= accesslogs.scrollHeight - accesslogs.clientHeight);

        if (data[2] != "" && data[2] != streamname.split("+")[0]) {
          return;
        }


        const up = el("span");
        up.innerHTML = "\u2191";
        up.appendChild(format.bytes(data[6]));
        if (data[6] != 0) {
          up.appendChild(document.createTextNode("("));
          up.appendChild(format.bits(data[6]*8/data[5],true));
          up.appendChild(document.createTextNode(")"));
        }
        const down = el("span");
        down.innerHTML = "\u2193";
        down.appendChild(format.bytes(data[7]));
        if (data[7] != 0) {
          down.appendChild(document.createTextNode("("));
          down.appendChild(format.bits(data[7]*8/data[5],true));
          down.appendChild(document.createTextNode(")"));
        }

        const msg = el("div");
        msg.appendChild(el("span", {class: "description"}, format.dateTime(data[0])));
        const tokenSpan = el("span", {beforeifnotempty: "Token: ", title: data[1], style: {maxWidth: "10em"}});
        tokenSpan.textContent = data[1];
        msg.appendChild(tokenSpan);
        msg.appendChild(el("span", {beforeifnotempty: "Connector: "}, data[3]));
        msg.appendChild(el("span", {beforeifnotempty: "Hostname: "}, data[4]));
        const durSpan = el("span", {beforeifnotempty: "Connected for: "});
        durSpan.innerHTML = format.duration(data[5]);
        msg.appendChild(durSpan);
        msg.appendChild(up);
        msg.appendChild(down);
        accesslogs.appendChild(msg);

        if (scroll) accesslogs.scrollTop = accesslogs.scrollHeight;

        if (tab) {
          try {
          const s = (tab.document.scrollingElement.scrollTop >= tab.document.scrollingElement.scrollHeight - tab.document.scrollingElement.clientHeight);
          tab.document.write(msg.outerHTML);
          if (s) tab.document.scrollingElement.scrollTop = tab.document.scrollingElement.scrollHeight;
          }
          catch (e) {}
        }

      }
      else if (type == "error") {
        accesslogs.appendChild(el("div", null, data));
      }
    });

    const openRawBtn = el("button", null, "Open raw");
    openRawBtn.addEventListener('click', function(){
      tab = window.open("", APP_NAME+" access logs for "+streamname);
      tab.document.write(
        "<html><head><title>"+APP_NAME+" access logs for '"+streamname+"'</title><meta http-equiv=\"content-type\" content=\"application/json; charset=utf-8\"><style>body{padding-left:2em;text-indent:-2em;}body>*>*:not(:last-child):not(:empty){padding-right:.5em;}.description{font-size:.9em;color:#777}[beforeifnotempty]:not(:empty):before{content:attr(beforeifnotempty);color:#777}</style></head><body>"
      );
      tab.document.write(accesslogs.innerHTML);
      tab.document.scrollingElement.scrollTop = tab.document.scrollingElement.scrollHeight;
    });
    const alSection = createStreamSlab({
      className: "accesslogs stream-slab--accesslogs",
      title: "Access logs",
      icon: "earth",
      actions: openRawBtn
    });
    alSection.slabBody.appendChild(accesslogs);
    return alSection;
  },
  preview: function(streamname,MistVideoObject){
    const preview_cont = el('section', {class: "preview"});
    if (!MistVideoObject) {
      window.mv = {};
      MistVideoObject = window.mv;
    }

    mistPlay(streamname,{
      target: preview_cont,
      host: sockets.http_host,
      theme: getActiveThemeId(),
      skin: {
        inherit: "dev",
        colors:{accent:"var(--accentColor)"},
        blueprints: {
          protocol_mismatch_warning: function(){
            const MistVideo = this;
            const ele = document.createElement("div");
            PlayerUtil.event.addListener(MistVideo.options.target,"initializeFailed",function(){
                if (MistVideo.info && MistVideo.info.source) {
                  let msg = "";
                  let html = false;
                  if (MistVideo.info.source.length == 0) {
                    msg = "No stream sources were found.";
                  }
                  else {
                    let protomatch = false;
                    let myprot = location.protocol;
                    for (const i in MistVideo.info.source) {
                      let url = MistVideo.info.source[i].url;
                      url = url.replace(/^ws/,"http");
                      if (url.slice(0,myprot.length) == myprot) {
                        protomatch = true;
                        break;
                      }
                      if (url.slice(0,2) == "//") {
                        protomatch = true;
                        break;
                      }
                    }
                    if (!protomatch) {
                      myprot = myprot.replace(":","").toUpperCase();
                      msg = "No stream sources using "+myprot+" were found. You should configure "+myprot+" to play media from a "+myprot+" webpage.";
                      if (myprot == "HTTPS") {
                        html = document.createElement("a");
                        html.setAttribute("href","https://docs.mistserver.org/howto/https/");
                        html.setAttribute("target","_blank");
                        html.appendChild(document.createTextNode("Documentation about HTTPS"));
                      }
                    }
                  }
                  if (msg != "") {
                    ele.className = "err_balloon orange";
                    ele.style.position = "static";
                    ele.style.margin = "2em 0 3em";
                    ele.appendChild(document.createTextNode("Warning:"));
                    ele.appendChild(document.createElement("br"));
                    ele.appendChild(document.createTextNode(msg));
                    ele.appendChild(document.createElement("br"));
                    if (html) {
                      ele.appendChild(html);
                      ele.appendChild(document.createElement("br"));
                    }
                    const button = document.createElement("button");
                    button.style.marginTop = "1em";
                    button.appendChild(document.createTextNode("Configure protocols"));
                    ele.appendChild(button);
                    button.addEventListener("click",function(){
                      navto("Protocols");
                    });
                  }
                }
              },MistVideo.video);

              return ele;
            }
          }
        },
        loop: true,
        MistVideoObject: MistVideoObject
    });

    return preview_cont;
  },
  playercontrols: function(MistVideoObject,video){
    const controls = el("section", {class: "controls mistvideo input_container"});
    controls.appendChild(el("h3", null, "helloPlayer"));
    controls.appendChild(el("p", null, "Waiting for player.."));

    if (!video) { video = document.querySelector(".dashboard"); }

    if (!document.querySelector("link#devcss")) {
      const cssLink = el("link", {rel: "stylesheet", type: "text/css", href: sockets.http_host+"skins/dev.css", id: "devcss"});
      document.head.appendChild(cssLink);
    }

    function toNode(value) {
      if (value instanceof Node) { return value; }
      if (Array.isArray(value)) {
        const frag = document.createDocumentFragment();
        for (let i = 0; i < value.length; i++) {
          const n = toNode(value[i]);
          if (n) { frag.appendChild(n); }
        }
        return frag;
      }
      if ((typeof value === "string") || (typeof value === "number")) {
        return document.createTextNode(String(value));
      }
      return null;
    }

    function appendMaybe(parent, value) {
      const node = toNode(value);
      if (node) { parent.appendChild(node); }
    }

    function asElement(value, fallbackTag) {
      const node = toNode(value);
      if (node && node.nodeType === 1) { return node; }
      const wrap = el(fallbackTag || "div");
      if (node) { wrap.appendChild(node); }
      return wrap;
    }

    function init() {
      const MistVideo = MistVideoObject.reference;
      function buildBlueprint(obj) {
        return MistVideo.UI.buildStructure.call(MistVideo.UI,obj);
      }

      const name = toNode(buildBlueprint({
        "if": function(){
          return (this.playerName && this.source)
        },
        then: {
          type: "container",
          classes: ["mistvideo-description"],
          style: { display: "block" },
          children: [
            {type: "playername", style: { display: "inline" }},
            {type: "text", text: "is playing", style: {margin: "0 0.2em"}},
            {type: "mimetype"}
          ]
        }
      }));
      const ctrls = toNode(buildBlueprint({
        type: "container",
        classes: ["mistvideo-column","mistvideo-devcontrols"],
        children: [
          {
            type: "protocol_mismatch_warning"
          },{
            type: "text",
            text: "Player control"
          },{
            type: "container",
            classes: ["mistvideo-devbuttons"],
            style: {"flex-wrap": "wrap"},
            children: [
              {
                "if": function(){ return !!(this.player && this.player.api); },
                then: {
                  type: "button",
                  title: "Reload the video source",
                  label: "Reload video",
                  onclick: function(){
                    this.player.api.load();
                  }
                }
              },{
                type: "button",
                title: "Build MistVideo again",
                label: "Reload player",
                onclick: function(){
                  this.reload();
                }
              },{
                type: "button",
                title: "Switch to the next available player and source combination",
                label: "Try next combination",
                onclick: function(){
                  this.nextCombo();
                }
              }
            ]
          },
          {type:"forcePlayer"},
          {type:"forceType"},
          {type:"forceSource"}
        ]
      }));
      const statistics = asElement(buildBlueprint({type:"decodingIssues", style: {"max-width":"30em","flex-flow":"column nowrap"}}), "div");
      const logs = asElement(buildBlueprint({type:"log"}), "div");
      const rawlogsBtn = el("button", {class: "mistplayer-log-raw"}, "Open raw");
      rawlogsBtn.addEventListener('click', function(){
        const streamname = MistVideo.stream;
        const tab = window.open("", "Player logs for "+streamname);
        tab.document.write(
          "<html><head><title>Player logs for '"+streamname+"'</title><meta http-equiv=\"content-type\" content=\"application/json; charset=utf-8\"><style>.timestamp{color:#777;font-size:0.9em;}</style></head><body>"
        );
        if (logs.lastChild && logs.lastChild.outerHTML) {
          tab.document.write(logs.lastChild.outerHTML);
        } else {
          tab.document.write("<pre>"+(logs.textContent || "")+"</pre>");
        }
        tab.document.scrollingElement.scrollTop = tab.document.scrollingElement.scrollHeight;
      });
      logs.classList.add("mistplayer-log-content");

      controls.innerHTML = '';
      const header = el("header", {class: "mistplayer-head"});
      const titleRow = el("div", {class: "mistplayer-title-row"});
      titleRow.appendChild(el("h3", {class: "title"}, "Stream Preview"));
      titleRow.appendChild(el("span", {class: "mistplayer-engine"}, "Engine: MistPlayer"));
      header.appendChild(titleRow);
      if (name) {
        const subtitle = el("div", {class: "mistplayer-subtitle"});
        appendMaybe(subtitle, name);
        header.appendChild(subtitle);
      }
      controls.appendChild(header);

      function createPanel(title, classes, action) {
        const panel = el("section", {class: "mistplayer-panel " + classes});
        const titleEl = el("h4", {class: "mistplayer-panel-title"}, title);
        if (action) { titleEl.appendChild(action); }
        panel.appendChild(titleEl);
        const body = el("div", {class: "mistplayer-panel-body"});
        panel.appendChild(body);
        return { panel: panel, body: body };
      }

      const slabs = el("div", {class: "mistplayer-slabs"});

      const controlsPanel = createPanel("Controls", "mistplayer-panel--controls");
      appendMaybe(controlsPanel.body, ctrls);
      slabs.appendChild(controlsPanel.panel);

      const statsPanel = createPanel("Status", "mistplayer-panel--stats");
      appendMaybe(statsPanel.body, statistics);
      slabs.appendChild(statsPanel.panel);

      const logsPanel = createPanel("Logs", "mistplayer-panel--logs", rawlogsBtn);
      appendMaybe(logsPanel.body, logs);
      slabs.appendChild(logsPanel.panel);

      controls.appendChild(slabs);
    }

    if (MistVideoObject && MistVideoObject.reference && MistVideoObject.reference.skin) {
      init();
    }
    let videoEl = null;
    if (video instanceof HTMLElement) {
      videoEl = video;
    } else if (video && (video[0] instanceof HTMLElement)) {
      videoEl = video[0];
    } else {
      videoEl = document.querySelector(".preview, .dashboard");
    }
    if (videoEl && videoEl.addEventListener) {
      videoEl.addEventListener("initialized",function(){ init(); });
      videoEl.addEventListener("initializedFailed",function(){ init(); });
    }

    return controls;
  },
  embedurls: function(streamname,misthost,urls){
    const cont = el("section", {class: "embedurls stream-embed-shell"});
    if (!urls) urls = {HTTP: [], HTTPS: []};

    const datalist = el("datalist", {id: "urlhints"});
    const allurls = (urls.HTTPS || []).concat(urls.HTTP || []);
    for (const i in allurls) {
      const opt = el("option");
      opt.value = allurls[i];
      datalist.appendChild(opt);
    }

    var storedOpts = UI.stored.getOpts();
    if (!otherhost.host && storedOpts.embedBaseUrl) {
      otherhost.host = storedOpts.embedBaseUrl;
    }
    const otherbase = otherhost.host ? otherhost.host : (allurls.length ? allurls[0] : misthost);

    function createEmbedSlab(title, className) {
      let classes = "embed-card embed-slab stream-slab";
      if (className) {
        classes += " " + className;
      }
      const slab = el("section", {class: classes});
      const head = el("div", {class: "stream-slab-head"});
      head.appendChild(el("h3", {class: "stream-slab-title"}, title));
      const body = el("div", {class: "stream-slab-body"});
      slab.appendChild(head);
      slab.appendChild(body);
      slab.slabBody = body;
      return slab;
    }

    const baseUrlDiv = createEmbedSlab("Base URL", "embed-base-url embed-slab--base");
    const baseIC = el('span', {class: 'input_container'});
    const baseLabel = el('label', {class: 'UIelement'});
    baseLabel.appendChild(el('span', {class: 'label'}, 'Use base URL:'));
    const baseFC = el('span', {class: 'field_container'});
    const baseInput = el('input', {type: 'text', class: 'field', list: 'urlhints'});
    baseInput.value = otherbase;
    baseFC.appendChild(baseInput);
    baseFC.appendChild(datalist);
    const baseUnit = el('span', {class: 'unit'});
    const applyBtn = el('button', null, 'Apply');
    applyBtn.addEventListener('click', function(){
      otherhost.host = this.closest('label').querySelector('input').value;
      if (otherhost.host.slice(-1) != "/") { otherhost.host += "/"; }
      UI.stored.saveOpt('embedBaseUrl', otherhost.host);
      navto('Embed',streamname);
    });
    baseUnit.appendChild(applyBtn);
    baseFC.appendChild(baseUnit);
    baseLabel.appendChild(baseFC);
    baseIC.appendChild(baseLabel);
    baseUrlDiv.slabBody.appendChild(baseIC);
    cont.appendChild(baseUrlDiv);

    const escapedstream = encodeURIComponent(streamname);
    let done = false;
    const defaultembedoptions = {
      forcePlayer: '',
      forceType: '',
      controls: true,
      autoplay: true,
      loop: false,
      muted: false,
      fillSpace: false,
      poster: '',
      urlappend: '',
      setTracks: {}
    };
    let embedoptions = Object.assign({},defaultembedoptions);
    const stored = UI.stored.getOpts();
    if ('embedoptions' in stored) {
      embedoptions = Object.assign(embedoptions,stored.embedoptions,true);
      if (typeof embedoptions.setTracks != 'object') {
        embedoptions.setTracks = {};
      }
    }
    const custom = {};
    switch (embedoptions.controls) {
      case 'stock':
        custom.controls = 'stock';
        break;
      case true:
        custom.controls = 1;
        break;
      case false:
        custom.controls = 0;
        break;
    }


    function embedhtml() {
      function randomstring(length){
        let s = '';
        function randomchar() {
          const n= Math.floor(Math.random()*62);
          if (n < 10) { return n; }
          if (n < 36) { return String.fromCharCode(n+55); }
          return String.fromCharCode(n+61);
        }
        while (length--) { s += randomchar(); }
        return s;
      }
      function maybequotes(val) {
        switch (typeof val) {
          case 'string':
            if (!isNaN(parseFloat(val)) && isFinite(val)) {
              return val;
            }
            return '"'+val+'"';
          case 'object':
            return JSON.stringify(val);
          default:
            return val;
        }
      }
      if (done) {
        UI.stored.saveOpt('embedoptions',embedoptions);
      }

      const target = streamname+'_'+randomstring(12);

      const options = ['target: document.getElementById("'+target+'")'];
      for (const i in embedoptions) {
        if (i == "prioritize_type") {
          if ((embedoptions[i]) && (embedoptions[i] != "")) {
            options.push("forcePriority: "+JSON.stringify({source:[["type",[embedoptions[i]]]]}));
          }
          continue;
        }
        if (i == "monitor_action") {
          if ((embedoptions[i]) && (embedoptions[i] != "")) {
            if (embedoptions[i] == "nextCombo") {
              options.push("monitor: {\n"+
                "          action: function(){\n"+
                '            this.MistVideo.log("Switching to nextCombo because of poor playback in "+this.MistVideo.source.type+" ("+Math.round(this.vars.score*1000)/10+"%)");'+"\n"+
                "            this.MistVideo.nextCombo();\n"+
                "          }\n"+
                "        }");
            }
          }
          continue;
        }
        if ((embedoptions[i] != defaultembedoptions[i]) && (embedoptions[i] != null) && ((typeof embedoptions[i] != 'object') || (JSON.stringify(embedoptions[i]) != JSON.stringify(defaultembedoptions[i])))) {
          options.push(i+': '+maybequotes(embedoptions[i]));
        }
      }

      const output = [];
      output.push('<div class="mistvideo" id="'+target+'">');
      output.push('  <noscript>');
      output.push('    <a href="'+otherbase+escapedstream+'.html'+'" target="_blank">');
      output.push('      Click here to play this video');
      output.push('    </a>');
      output.push('  </noscript>');
      output.push('  <script>');
      output.push('    var a = function(){');
      output.push('      mistPlay("'+streamname+'",{');
      output.push('        '+options.join(",\n        "));
      output.push('      });');
      output.push('    };');
      output.push('    if (!window.mistplayers) {');
      output.push('      var p = document.createElement("script");');

      if (urls.HTTPS.length) {
        output.push('      if (location.protocol == "https:") { p.src = "'+(parseURL(otherbase).protocol == "https://" ? otherbase : urls.HTTPS[0])+'player.js" } ');
        output.push('      else { p.src = "'+((parseURL(otherbase).protocol == "http://" ? otherbase : urls.HTTP[0]))+'player.js" } ');
      }
      else {
        output.push('      p.src = "'+otherbase+'player.js"');
      }

      output.push('      document.head.appendChild(p);');
      output.push('      p.onload = a;');
      output.push('    }');
      output.push('    else { a(); }');
      output.push('  </script>');
      output.push('</div>');

      return output.join("\n");
    }

    function embedhtml_esm() {
      function randomstring(length){
        let s = '';
        function randomchar() {
          const n= Math.floor(Math.random()*62);
          if (n < 10) { return n; }
          if (n < 36) { return String.fromCharCode(n+55); }
          return String.fromCharCode(n+61);
        }
        while (length--) { s += randomchar(); }
        return s;
      }
      function maybequotes(val) {
        switch (typeof val) {
          case 'string':
            if (!isNaN(parseFloat(val)) && isFinite(val)) {
              return val;
            }
            return "'"+val+"'";
          case 'object':
            return JSON.stringify(val);
          default:
            return val;
        }
      }
      if (done) {
        UI.stored.saveOpt('embedoptions',embedoptions);
      }

      const target = streamname+'_'+randomstring(12);

      let importUrl;
      if (urls.HTTPS.length) {
        importUrl = (parseURL(otherbase).protocol == "https://" ? otherbase : urls.HTTPS[0]) + 'player.mjs';
      } else {
        importUrl = otherbase + 'player.mjs';
      }

      const options = [];
      options.push("  target: document.getElementById('"+target+"')");
      options.push("  stream: '"+streamname+"'");
      options.push("  host: '"+otherbase+"'");
      for (const i in embedoptions) {
        if (i == 'setTracks' && (!embedoptions[i] || !Object.keys(embedoptions[i]).length)) continue;
        if (i == "prioritize_type") {
          if ((embedoptions[i]) && (embedoptions[i] != "")) {
            options.push("  forcePriority: "+JSON.stringify({source:[["type",[embedoptions[i]]]]}));
          }
          continue;
        }
        if (i == "monitor_action") {
          if ((embedoptions[i]) && (embedoptions[i] != "")) {
            if (embedoptions[i] == "nextCombo") {
              options.push("  monitor: {\n"+
                "    action() {\n"+
                "      this.MistVideo.log('Switching to nextCombo because of poor playback in ' + this.MistVideo.source.type + ' (' + Math.round(this.vars.score*1000)/10 + '%)');\n"+
                "      this.MistVideo.nextCombo();\n"+
                "    }\n"+
                "  }");
            }
          }
          continue;
        }
        if ((embedoptions[i] != defaultembedoptions[i]) && (embedoptions[i] != null) && ((typeof embedoptions[i] != 'object') || (JSON.stringify(embedoptions[i]) != JSON.stringify(defaultembedoptions[i])))) {
          options.push('  '+i+': '+maybequotes(embedoptions[i]));
        }
      }

      const output = [];
      output.push('<div id="'+target+'"></div>');
      output.push('<script type="module">');
      output.push("  import { createPlayer } from '"+importUrl+"';");
      output.push('');
      output.push('  const player = createPlayer({');
      output.push(options.join(",\n"));
      output.push('  });');
      output.push('</script>');

      return output.join("\n");
    }

    function embedhtml_cdn() {
      function randomstring(length){
        let s = '';
        function randomchar() {
          const n= Math.floor(Math.random()*62);
          if (n < 10) { return n; }
          if (n < 36) { return String.fromCharCode(n+55); }
          return String.fromCharCode(n+61);
        }
        while (length--) { s += randomchar(); }
        return s;
      }
      function maybequotes(val) {
        switch (typeof val) {
          case 'string':
            if (!isNaN(parseFloat(val)) && isFinite(val)) { return val; }
            return '"'+val+'"';
          case 'object':
            return JSON.stringify(val);
          default:
            return val;
        }
      }
      if (done) { UI.stored.saveOpt('embedoptions',embedoptions); }

      const target = streamname+'_'+randomstring(12);
      const cdnUrl = 'https://unpkg.com/@optimist-video/mistplayer-core/dist/player.iife.js';

      const options = ['target: document.getElementById("'+target+'")'];
      options.push('host: "'+otherbase+'"');
      for (const i in embedoptions) {
        if (i == "prioritize_type") {
          if ((embedoptions[i]) && (embedoptions[i] != "")) {
            options.push("forcePriority: "+JSON.stringify({source:[["type",[embedoptions[i]]]]}));
          }
          continue;
        }
        if (i == "monitor_action") {
          if ((embedoptions[i]) && (embedoptions[i] != "")) {
            if (embedoptions[i] == "nextCombo") {
              options.push("monitor: {\n"+
                "          action: function(){\n"+
                '            this.MistVideo.log("Switching to nextCombo because of poor playback in "+this.MistVideo.source.type+" ("+Math.round(this.vars.score*1000)/10+"%)");'+"\n"+
                "            this.MistVideo.nextCombo();\n"+
                "          }\n"+
                "        }");
            }
          }
          continue;
        }
        if ((embedoptions[i] != defaultembedoptions[i]) && (embedoptions[i] != null) && ((typeof embedoptions[i] != 'object') || (JSON.stringify(embedoptions[i]) != JSON.stringify(defaultembedoptions[i])))) {
          options.push(i+': '+maybequotes(embedoptions[i]));
        }
      }

      const output = [];
      output.push('<div id="'+target+'"></div>');
      output.push('<script src="'+cdnUrl+'"><\/script>');
      output.push('<script>');
      output.push('  mistPlay("'+streamname+'",{');
      output.push('    '+options.join(",\n    "));
      output.push('  });');
      output.push('<\/script>');

      return output.join("\n");
    }

    function embedhtml_npm() {
      function randomstring(length){
        let s = '';
        function randomchar() {
          const n= Math.floor(Math.random()*62);
          if (n < 10) { return n; }
          if (n < 36) { return String.fromCharCode(n+55); }
          return String.fromCharCode(n+61);
        }
        while (length--) { s += randomchar(); }
        return s;
      }
      function maybequotes(val) {
        switch (typeof val) {
          case 'string':
            if (!isNaN(parseFloat(val)) && isFinite(val)) { return val; }
            return "'"+val+"'";
          case 'object':
            return JSON.stringify(val);
          default:
            return val;
        }
      }
      if (done) { UI.stored.saveOpt('embedoptions',embedoptions); }

      const target = streamname+'_'+randomstring(12);

      const options = [];
      options.push("  target: document.getElementById('"+target+"')");
      options.push("  host: '"+otherbase+"'");
      options.push("  stream: '"+streamname+"'");
      for (const i in embedoptions) {
        if (i == 'setTracks' && (!embedoptions[i] || !Object.keys(embedoptions[i]).length)) continue;
        if (i == "prioritize_type") {
          if ((embedoptions[i]) && (embedoptions[i] != "")) {
            options.push("  forcePriority: "+JSON.stringify({source:[["type",[embedoptions[i]]]]}));
          }
          continue;
        }
        if (i == "monitor_action") {
          if ((embedoptions[i]) && (embedoptions[i] != "")) {
            if (embedoptions[i] == "nextCombo") {
              options.push("  monitor: {\n"+
                "    action() {\n"+
                "      this.MistVideo.log('Switching to nextCombo because of poor playback in ' + this.MistVideo.source.type + ' (' + Math.round(this.vars.score*1000)/10 + '%)');\n"+
                "      this.MistVideo.nextCombo();\n"+
                "    }\n"+
                "  }");
            }
          }
          continue;
        }
        if ((embedoptions[i] != defaultembedoptions[i]) && (embedoptions[i] != null) && ((typeof embedoptions[i] != 'object') || (JSON.stringify(embedoptions[i]) != JSON.stringify(defaultembedoptions[i])))) {
          options.push('  '+i+': '+maybequotes(embedoptions[i]));
        }
      }

      const output = [];
      output.push('// npm install @optimist-video/mistplayer-core');
      output.push('');
      output.push("import { createPlayer } from '@optimist-video/mistplayer-core';");
      output.push('');
      output.push("const player = createPlayer({");
      output.push(options.join(",\n"));
      output.push('});');

      return output.join("\n");
    }

    let embedFormat = 'iife';
    let embedSource = 'self';
    const stored_fmt = UI.stored.getOpts();
    if (stored_fmt.embedFormat) embedFormat = stored_fmt.embedFormat;
    if (stored_fmt.embedSource) embedSource = stored_fmt.embedSource;

    function getEmbedCode() {
      if (embedSource === 'cdn') return embedhtml_cdn();
      if (embedSource === 'npm') return embedhtml_npm();
      return embedFormat === 'esm' ? embedhtml_esm() : embedhtml();
    }

    const emhtml = getEmbedCode();
    const setTracks = el('div', {style: {display: 'flex', flexFlow: 'column nowrap'}});
    setTracks.textContent = 'Loading..';
    const protocolurls = el('div', {style: {wordBreak: "break-all"}});
    protocolurls.textContent = 'Loading..';
    const forcePlayerOptions = [['','Automatic']];
    for (const i in mistplayers) {
      forcePlayerOptions.push([i,mistplayers[i].name]);
    }

    function updateEmbedCode() {
      const embedField = cont.querySelector('textarea.field.embed_code, textarea.embed_code');
      if (embedField) {
        const code = getEmbedCode();
        embedField.value = code;
        embedField.textContent = code;
        embedField.rows = code.split("\n").length + 3;
        embedField.dispatchEvent(new Event('input', {bubbles: true}));
      }
    }

    const urlsCard = createEmbedSlab("URLs", "embed-slab--urls");
    const urlsUI = formEngine.buildUI([
      {
        label: 'Stream info json',
        type: 'str',
        value: otherbase+'json_'+escapedstream+'.js',
        readonly: true,
        clipboard: true,
        help: 'Information about this stream as a json page.'
      },{
        label: 'Stream info script',
        type: 'str',
        value: otherbase+'info_'+escapedstream+'.js',
        readonly: true,
        clipboard: true,
        help: 'This script loads information about this stream into a mistvideo javascript object.'
      },{
        label: 'HTML page',
        type: 'str',
        value: otherbase+escapedstream+'.html',
        readonly: true,
        qrcode: true,
        clipboard: true,
        help: 'A basic html containing the embedded stream.'
      }
    ]);
    if (urlsUI instanceof HTMLElement) urlsCard.slabBody.appendChild(urlsUI);
    cont.appendChild(urlsCard);

    const embedCard = createEmbedSlab("Embed code", "embed-slab--code");

    const sourceToggle = el('div', {class: 'embed-format-toggle'});
    const selfBtn = el('button', {type: 'button', class: 'embed-format-btn' + (embedSource === 'self' ? ' active' : '')}, 'Self-hosted');
    const cdnBtn = el('button', {type: 'button', class: 'embed-format-btn' + (embedSource === 'cdn' ? ' active' : '')}, 'CDN');
    const npmBtn = el('button', {type: 'button', class: 'embed-format-btn' + (embedSource === 'npm' ? ' active' : '')}, 'npm');

    const subToggle = el('div', {class: 'embed-format-toggle', style: {marginTop: '0.35em'}});
    const iifeBtn = el('button', {type: 'button', class: 'embed-format-btn' + (embedFormat === 'iife' ? ' active' : '')}, 'IIFE');
    const esmBtn = el('button', {type: 'button', class: 'embed-format-btn' + (embedFormat === 'esm' ? ' active' : '')}, 'ESM');
    subToggle.appendChild(iifeBtn);
    subToggle.appendChild(esmBtn);
    if (embedSource !== 'self') subToggle.style.display = 'none';

    const formatHelp = el('p', {class: 'embed-format-help'});
    const cdnNote = el('p', {class: 'embed-format-help', style: {fontSize: '0.85em', opacity: '0.7'}});
    cdnNote.textContent = 'Also available via jsdelivr: https://cdn.jsdelivr.net/npm/@optimist-video/mistplayer-core/dist/player.iife.js';
    if (embedSource !== 'cdn') cdnNote.style.display = 'none';

    function updateFormatHelp() {
      if (embedSource === 'cdn') {
        formatHelp.textContent = 'Load the player from a public CDN. You still need to point host at your MistServer instance.';
      } else if (embedSource === 'npm') {
        formatHelp.textContent = 'Install via npm for use with bundlers (webpack, vite, esbuild, etc.). The ESM export gives you createPlayer plus full access to all player internals.';
      } else if (embedFormat === 'esm') {
        formatHelp.textContent = 'ES Module - use with bundlers, modern apps, or <script type="module">. Gives you a clean API with queries, mutations & callbacks.';
      } else {
        formatHelp.textContent = 'Classic script tag - works everywhere, no build step needed. Loads the player globally and calls mistPlay().';
      }
    }
    updateFormatHelp();

    function setActiveSource(src) {
      embedSource = src;
      UI.stored.saveOpt('embedSource', embedSource);
      selfBtn.classList.toggle('active', src === 'self');
      cdnBtn.classList.toggle('active', src === 'cdn');
      npmBtn.classList.toggle('active', src === 'npm');
      subToggle.style.display = src === 'self' ? '' : 'none';
      cdnNote.style.display = src === 'cdn' ? '' : 'none';
      updateFormatHelp();
      updateEmbedCode();
    }

    selfBtn.addEventListener('click', function(e){ e.preventDefault(); setActiveSource('self'); });
    cdnBtn.addEventListener('click', function(e){ e.preventDefault(); setActiveSource('cdn'); });
    npmBtn.addEventListener('click', function(e){ e.preventDefault(); setActiveSource('npm'); });

    iifeBtn.addEventListener('click', function(e){
      e.preventDefault();
      embedFormat = 'iife';
      UI.stored.saveOpt('embedFormat', embedFormat);
      iifeBtn.classList.add('active');
      esmBtn.classList.remove('active');
      updateFormatHelp();
      updateEmbedCode();
    });
    esmBtn.addEventListener('click', function(e){
      e.preventDefault();
      embedFormat = 'esm';
      UI.stored.saveOpt('embedFormat', embedFormat);
      esmBtn.classList.add('active');
      iifeBtn.classList.remove('active');
      updateFormatHelp();
      updateEmbedCode();
    });

    sourceToggle.appendChild(selfBtn);
    sourceToggle.appendChild(cdnBtn);
    sourceToggle.appendChild(npmBtn);
    embedCard.slabBody.appendChild(sourceToggle);
    embedCard.slabBody.appendChild(subToggle);
    embedCard.slabBody.appendChild(formatHelp);

    const embedUI = formEngine.buildUI([
      {
        label: 'Embed code',
        type: 'textarea',
        value: emhtml,
        rows: emhtml.split("\n").length+3,
        readonly: true,
        classes: ['embed_code'],
        clipboard: true,
        help: 'Include this code on your webpage to embed the stream. The options below can be used to configure how your content is displayed.'
      }
    ]);
    if (embedUI instanceof HTMLElement) embedCard.slabBody.appendChild(embedUI);
    embedCard.slabBody.appendChild(cdnNote);

    const playerInfo = el('p', {class: 'embed-player-info'});
    playerInfo.innerHTML = 'The player is built for <b>white-labelling</b> - use built-in themes, bring your own CSS, or replace any element with custom components. '
      + '<a href="https://www.npmjs.com/package/@optimist-video/mistplayer-core" target="_blank" rel="noopener">npm</a>'
      + ' · <a href="https://docs.mistserver.org/" target="_blank" rel="noopener">Docs</a>';
    embedCard.slabBody.appendChild(playerInfo);

    cont.appendChild(embedCard);

    const optionsCard = createEmbedSlab("Player options", "embed-slab--options");
    const optionsUI = formEngine.buildUI([
      {
        label: 'Prioritize type',
        type: 'select',
        select: [['','Automatic']],
        pointer: { main: embedoptions, index: 'prioritize_type' },
        classes: ['prioritize_type'],
        'function': function(){
          if (!done) { return; }
          embedoptions.prioritize_type = getval(this);
          updateEmbedCode();
        },
        help: 'Try to use this source type first, but fall back to something else if it is not available.'
      },{
        label: 'Force type',
        type: 'select',
        select: [['','Automatic']],
        pointer: { main: embedoptions, index: 'forceType' },
        classes: ['forceType'],
        'function': function(){
          if (!done) { return; }
          embedoptions.forceType = getval(this);
          updateEmbedCode();
        },
        help: 'Only use this particular source.'
      },{
        label: 'Force player',
        type: 'select',
        select: forcePlayerOptions,
        pointer: { main: embedoptions, index: 'forcePlayer' },
        classes: ['forcePlayer'],
        'function': function(){
          if (!done) { return; }
          embedoptions.forcePlayer = getval(this);
          updateEmbedCode();
        },
        help: 'Only use this particular player.'
      },{
        label: 'Controls',
        type: 'select',
        select: [['1',APP_NAME+' Controls'],['stock','Player controls'],['0','None']],
        pointer: { main: custom, index: 'controls' },
        'function': function(){
          const v = getval(this);
          embedoptions.controls = (v == 1);
          switch (v) {
            case 0:
              embedoptions.controls = false;
              break;
            case 1:
              embedoptions.controls = true;
              break;
            case 'stock':
              embedoptions.controls = 'stock';
              break;
          }
          updateEmbedCode();
        },
        help: 'The type of controls that should be shown.'
      },{
        label: 'Autoplay',
        type: 'checkbox',
        pointer: { main: embedoptions, index: 'autoplay' },
        'function': function(){
          embedoptions.autoplay = getval(this);
          updateEmbedCode();
        },
        help: 'Whether or not the video should play as the page is loaded.'
      },{
        label: 'Loop',
        type: 'checkbox',
        pointer: { main: embedoptions, index: 'loop' },
        'function': function(){
          embedoptions.loop = getval(this);
          updateEmbedCode();
        },
        help: 'If the video should restart when the end is reached.'
      },{
        label: 'Start muted',
        type: 'checkbox',
        pointer: { main: embedoptions, index: 'muted' },
        'function': function(){
          embedoptions.muted = getval(this);
          updateEmbedCode();
        },
        help: 'Start the video without audio.'
      },{
        label: 'Fill available space',
        type: 'checkbox',
        pointer: { main: embedoptions, index: 'fillSpace' },
        'function': function(){
          embedoptions.fillSpace = getval(this);
          updateEmbedCode();
        },
        help: 'The video will fit the available space in its container, even if the video stream has a smaller resolution.'
      },{
        label: 'Poster',
        type: 'str',
        pointer: { main: embedoptions, index: 'poster' },
        'function': function(){
          embedoptions.poster = getval(this);
          updateEmbedCode();
        },
        help: 'URL to an image that is displayed when the video is not playing.'
      },{
        label: 'Video URL addition',
        type: 'str',
        pointer: { main: embedoptions, index: 'urlappend' },
        help: 'The embed script will append this string to the video url, useful for sending through params.',
        classes: ['embed_code_forceprotocol'],
        'function': function(){
          embedoptions.urlappend = getval(this);
          updateEmbedCode();
        }
      },{
        label: 'Preselect tracks',
        type: 'DOMfield',
        DOMfield: setTracks,
        help: 'Pre-select these tracks.'
      },{
        label: 'Monitoring action',
        type: 'select',
        select: [['','Ask the viewer what to do'],['nextCombo','Try the next source / player combination']],
        pointer: { main: embedoptions, index: 'monitor_action' },
        'function': function(){
          embedoptions.monitor_action = getval(this);
          updateEmbedCode();
        },
        help: 'What the player should do when playback is poor.'
      }
    ]);
    if (optionsUI instanceof HTMLElement) optionsCard.slabBody.appendChild(optionsUI);
    cont.appendChild(optionsCard);

    const protoCard = createEmbedSlab("Protocol stream URLs", "embed-protocol-urls embed-slab--protocols");
    protoCard.slabBody.appendChild(protocolurls);
    cont.appendChild(protoCard);

    function displaySources(d,overwritebase) {
      const build = [];
      const s_forceType = cont.querySelector('.field.forceType');
      const s_prioritizeType = cont.querySelector('.field.prioritize_type');

      if (overwritebase) {
        overwritebase = overwritebase.replace(parseURL(overwritebase).protocol,"");
        const warnDiv = el("div", {class: "orange", style: {margin: "0.5em 0", maxWidth: "45em", width: "100%", wordBreak: "normal"}});
        warnDiv.innerHTML = "Warning: the provided base URL <a href=\""+otherbase+"\">"+otherbase+"</a> could not be reached. These links are my best guess but will probably not work properly.";
        build.push(warnDiv);
      }

      const grouped = {};
      const groupOrder = [];
      for (const i in d.source) {
        const source = d.source[i];
        const human = uiCore.humanMime(source.type);
        const groupName = human || format.capital(source.type);

        let url = source.url;
        if (overwritebase) {
          url = parseURL(source.url).protocol + overwritebase + source.relurl;
        }

        let tkn = url.match(/[\?\&]tkn=\d+\&?/);
        if (tkn) {
          tkn = tkn[0];
          url = url.replace(tkn,(tkn[0] == "?") && (tkn.slice(-1) == "&") ? "?" : (tkn.slice(-1) == "&" ? "&" : ""));
        }

        if (!grouped[groupName]) {
          grouped[groupName] = [];
          groupOrder.push(groupName);
        }
        grouped[groupName].push({ source: source, url: url, human: human });

        if (!s_forceType.querySelector('option[value="'+source.type+'"]')) {
          const ftOpt = el('option');
          ftOpt.textContent = (human ? human+' ('+source.type+')' : format.capital(source.type));
          ftOpt.value = source.type;
          s_forceType.appendChild(ftOpt);
          const ptOpt = el('option');
          ptOpt.textContent = (human ? human+' ('+source.type+')' : format.capital(source.type));
          ptOpt.value = source.type;
          s_prioritizeType.appendChild(ptOpt);
        }
      }
      for (let gi = 0; gi < groupOrder.length; gi++) {
        const gName = groupOrder[gi];
        const gItems = grouped[gName];
        if (groupOrder.length > 1) {
          build.push(el('h4', {class: 'embed-url-group'}, gName));
        }
        for (let si = 0; si < gItems.length; si++) {
          const item = gItems[si];
          build.push({
            label: (item.human ? item.human+' <span class=description>('+item.source.type+')</span>' : format.capital(item.source.type)),
            type: 'str',
            value: item.url,
            readonly: true,
            clipboard: true
          });
        }
      }
      s_forceType.value = embedoptions.forceType;
      s_prioritizeType.value = embedoptions.prioritize_type;
      const builtProto = formEngine.buildUI(build);
      protocolurls.innerHTML = '';
      if (builtProto instanceof HTMLElement) protocolurls.appendChild(builtProto);
      done = true;
    }
    function displayTracks(msg) {
      const tracks = {};
      for (const i in msg.meta.tracks) {
        const t = msg.meta.tracks[i];
        if (t.codec == "subtitle") {
          t.type = "subtitle";
        }
        if ((t.type != 'audio') && (t.type != 'video') && (t.type != "subtitle")) { continue; }

        if (!(t.type in tracks)) {
          if (t.type == "subtitle") {
            tracks[t.type] = [];
          }
          else {
            tracks[t.type] = [[(''),"Autoselect "+t.type]];
          }
        }
        tracks[t.type].push([t.trackid,format.capital(t.type)+' track '+(tracks[t.type].length+(t.type == "subtitle" ? 1 : 0))]);
      }
      setTracks.innerHTML = '';

      if (Object.keys(tracks).length) {
        const stLabel = setTracks.closest('label');
        if (stLabel) stLabel.hidden = false;
        const trackarray = ["audio","video","subtitle"];
        for (const n in trackarray) {
          const i = trackarray[n];
          if (!tracks[i] || !tracks[i].length) { continue; }
          const sel = el('select', {"data-type": i, style: {flexGrow: "1"}});
          sel.addEventListener('change', function(){
            if (this.value == '') {
              delete embedoptions.setTracks[this.getAttribute('data-type')];
            }
            else {
              embedoptions.setTracks[this.getAttribute('data-type')] = this.value;
            }
            updateEmbedCode();
          });
          setTracks.appendChild(sel);
          if (i == "subtitle") {
            tracks[i].unshift(["","No "+i]);
          }
          else {
            tracks[i].push([-1,'No '+i]);
          }
          for (const j in tracks[i]) {
            const tOpt = el('option');
            tOpt.value = tracks[i][j][0];
            tOpt.textContent = tracks[i][j][1];
            sel.appendChild(tOpt);
          }
          if (i in embedoptions.setTracks) {
            sel.value = embedoptions.setTracks[i];
            if (sel.value == null || sel.value == '') {
              sel.value = '';
              delete embedoptions.setTracks[i];
              updateEmbedCode();
            }
          }
        }
      }
      else {
        const stLabel = setTracks.closest('label');
        if (stLabel) stLabel.hidden = true;
      }
    }

    function connect2info(baseurl) {
      sockets.ws.info_json.subscribe(function(msg){
        try {
          if (msg.type == "error") {
            if (baseurl == otherbase) {
              connect2info(misthost);
            }
            else {
              throw msg;
            }
            return;
          }

          if ("source" in msg) {
            if (!done || msg.source.length) displaySources(msg,baseurl == otherbase ? false : otherbase);
          }
          if (("meta" in msg) && ("tracks" in msg.meta)) {
            displayTracks(msg);
          }
        } catch(e) {
          console.warn('Embed info handler:', e);
        }

      },streamname,baseurl.replace(/^http/,"ws")+ "json_" + encodeURIComponent(streamname) + ".js",false,"?inclzero=1");

    }
    connect2info(otherbase);


    return cont;
  },
  tags: function(options){
    options = Object.assign({
      streamname: false,
      readonly: false,
      context_menu: false,
      onclick: false,
      getStreamstatus: function(){ return 0; }
    },options);

    return dynamicUI.dynamic({
      create: function(){
        const ele = document.createElement("div");
        ele.classList.add("tags");
        return ele;
      },
      add: {
        create: function(id){
          const tag = document.createElement("span");
          tag.classList.add("tag");
          tag.appendChild(document.createTextNode(id));

          tag.remove = function(){
            if (this.parentNode) {
              this.parentNode.removeChild(this);
            }
          };

          if (options.onclick) tag.addEventListener("click",function(e) {
            return options.onclick.apply(this,[e,id])
          });
          if (!options.readonly && options.context_menu) {
            tag.addEventListener("contextmenu",function(e){
              e.stopPropagation();
              e.preventDefault();
              const type = this.values;
              const stream = options.streamname;
              const state = options.getStreamstatus.apply(this,arguments);

              const menu = [];
              if (state != 0) {
                if (type == 0) {
                  menu.push([
                    "Re-activate tag",function(){
                      const save = {tag_stream:{}};
                      save.tag_stream[stream] = id;
                      apiClient.send(function(){
                        options.context_menu.hide();
                      },save);
                    },"\u2795",""
                  ]);
                }
                else {
                  menu.push([
                    "Untag stream",function(){
                      const save = {untag_stream:{}};
                      save.untag_stream[stream] = id;
                      apiClient.send(function(){
                        options.context_menu.hide();
                      },save);
                    },"\u2796","Remove this tag from this stream."+(type == 2 ? "\nIt will not be removed from the config, so it will return when the stream restarts." : "")
                  ]);
                }
              }
              const headerDiv = el("div", {class: "header"});
              headerDiv.appendChild(el("div", null, "#"+id));
              const descDiv = el("div", {class: "description", title: this.getAttribute("title")});
              descDiv.textContent = {
                0: "Inactive tag",
                1: "Transient tag",
                2: "Persistent tag"
              }[type];
              headerDiv.appendChild(descDiv);
              options.context_menu.show([[headerDiv],menu.length ? menu : null],e);
            });
          }

          return tag;
        },
        update: function(type){
          const title = {
            0: "This tag is inactive: it is in the stream config, but it has been removed. It will return when the stream restarts.",
            1: "This tag is transient: it is not in the stream config. It will disappear when the stream becomes inactive.",
            2: "This tag is persistent: it is in the stream config. It will remain when the stream restarts."
          };

          if (this.raw !== type) {
            this.raw = type;
            this.setAttribute("data-type",type);
            this.setAttribute("title",title[type]);
          }
        }
      },
      getEntries: function(data){
        if (!data) { data = {}; }
        if (Array.isArray(data)) {
          data = {stats: data};
        }
        if (!("tags" in data)) {
          data.tags = [];
          const streambase = options.streamname.split("+")[0];
          if (streambase in mist.data.streams) {
            data.tags = mist.data.streams[streambase].tags;
          }
        }

        const out = {};
        for (const i in data.tags) {
          out[data.tags[i]] = 0;
        }
        if (data.stats && (data.stats.length >= 6) && (data.stats[1] != 0)) {
          let transients = data.stats[5];
          if ((typeof transients == "string") && (transients != "")) { transients = transients.split(" "); }
          for (const i in transients) {
            if (transients[i] in out) {
              out[transients[i]] = 2;
            }
            else {
              out[transients[i]] = 1;
            }
          }
        }
        else {
          for (const i in out) {
            out[i] = 2;
          }
        }

        return out;
      }
    });
  },
  clients: function(streamname){
    const layout = {
      Host: function(d){
        return d.host;
      },
      Protocol: function(d){
        return d.protocol.replace("INPUT:","");
      },
      Connected: function(d){
        return format.duration(d.conntime);
      },
      "Data downloaded": function(d){
        return format.bytes(d.down);
      },
      "Current bitrate": function(d){
        return format.bits(d.downbps*8,true);
      },
      "Media time": function(d){
        return d.position ? format.duration(d.position) : "";
      },
      "Packets received": function(d){
        return d.pktcount ? format.number(d.pktcount,{round:false}) : ""
      },
      "Packets lost": function(d){
        return d.pktcount ? format.number(d.pktlost,{round:false}) : ""
      },
      "Packets retransmitted": function(d){
        return d.pktcount ? format.number(d.pktretransmit,{round:false}) : ""
      }

    };
    const inputs = dynamicUI.dynamic({
      create: function(){
        const thead = el("thead");
        const tbody = el("tbody");
        const table = el("table");
        table.appendChild(thead);
        table.appendChild(tbody);
        table._rows = {};
        for (const i in layout) {
          const row = el("tr");
          row.appendChild(el("td", null, i+":"));
          table._rows[i]= row;
          tbody.appendChild(row);
        }
        thead.appendChild(tbody.firstElementChild);
        return table;
      },
      add: {
        create: function(){
          const out = {
            cells: {},
            customAdd: function(table){
              for (const i in this.cells) {
                table._rows[i].appendChild(this.cells[i]);
              }
            },
            remove: function(table){
              for (const i in this.cells) {
                this.cells[i].remove();
                delete this.cells[i];
              }
            }
          };
          for (const i in layout) {
            out.cells[i] = el("td");
          }
          return out;
        },
        update: function(d){
          for (const i in this.cells) {
            const value = layout[i].call(null,d);
            const renderKey = streamCellRenderKey(value);
            if (this.cells[i].raw != renderKey) {
              setStreamCellValue(this.cells[i], value);
              this.cells[i].raw = renderKey;
            }
          }
        }
      },
      update: function(values){
        if (values.length) {
          if (!inputs.parentNode) {
            inputs._parent.appendChild(inputs);
          }
        }
        else if (inputs.parentNode) {
          inputs._parent = inputs.parentNode;
          inputs.remove();
        }
      }
    });

    sockets.http.clients.subscribe(function(d){
      for (let i = d.data.length -1; i >= 0; i--) {
        if (d.data[i].stream != streamname) {
          d.data.splice(i,1);
        }
      }
      inputs.update(d.data);
    },{streams:[streamname],protocols:["INPUT"]});


    const inputsDiv = el("div", {class: "inputs"});
    inputsDiv.appendChild(inputs);
    const clientSection = createStreamSlab({
      className: "clients stream-slab--clients",
      title: "Current input",
      icon: "plug"
    });
    clientSection.slabBody.appendChild(inputsDiv);
    return clientSection;
  }
};

const logHlRe = /(https?:\/\/[^\s"'<>)]+)|(\b\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}(?::\d{1,5})?)|("(?:[^"\\]|\\.)*")|(\/(?:[\w.+\-]+\/)+[\w.+\-]*)|(\b\d[\d,.]*(?:\.\d+)?\b)/g;
const logHlClasses = [null, "log-hl-url", "log-hl-ip", "log-hl-string", "log-hl-path", "log-hl-num"];

function highlightLogText(text){
  const frag = document.createDocumentFragment();
  let last = 0;
  logHlRe.lastIndex = 0;
  let m;
  while ((m = logHlRe.exec(text)) !== null) {
    if (m.index > last) frag.appendChild(document.createTextNode(text.slice(last, m.index)));
    let cls = "";
    for (let i = 1; i < logHlClasses.length; i++) {
      if (m[i]) { cls = logHlClasses[i]; break; }
    }
    if (cls === "log-hl-url") {
      const a = document.createElement("a");
      a.href = m[0];
      a.target = "_blank";
      a.rel = "noopener";
      a.className = cls;
      a.textContent = m[0];
      frag.appendChild(a);
    } else {
      const span = document.createElement("span");
      span.className = cls;
      span.textContent = m[0];
      frag.appendChild(span);
    }
    last = logHlRe.lastIndex;
  }
  if (last < text.length) frag.appendChild(document.createTextNode(text.slice(last)));
  return frag;
}

export function logs(streamname, options){
  options = options || {};
  let scroll = true;
  const logsDiv = el("div", {class: "logs", onempty: "None.", "data-scrolling": "true"});
  logsDiv.addEventListener("scroll",function(){
    if (this.scrollTop + this.clientHeight >= this.scrollHeight - 5) {
      scroll = true;
    }
    else {
      scroll = false;
    }
    logsDiv.setAttribute("data-scrolling",scroll);
  });

  let tab = false;
  let busy = false;
  const queue = [];
  let filterTerm = '';
  let maxBuffer = parseInt(localStorage.getItem("logs-buffer-size")) || 1000;

  function addMessages(){
    busy = true;

    requestAnimationFrame(function(){
      while (queue.length) {
        var item = queue.shift();
        if (filterTerm && item.textContent.toLowerCase().indexOf(filterTerm) === -1) {
          item.hidden = true;
        }
        logsDiv.appendChild(item);
      }

      if (maxBuffer > 0) {
        while (logsDiv.children.length > maxBuffer && logsDiv.firstElementChild) {
          logsDiv.firstElementChild.remove();
        }
      }

      if (scroll) logsDiv.scrollTop = logsDiv.scrollHeight;

      if (tab) {
        try {
          let s = (tab.document.scrollingElement.scrollTop >= tab.document.scrollingElement.scrollHeight - tab.document.scrollingElement.clientHeight);
          if (queue.length) tab.document.write(queue[queue.length-1].outerHTML);
          if (s) tab.document.scrollingElement.scrollTop = tab.document.scrollingElement.scrollHeight;
        }
        catch (e) {}
      }
      busy = false;

    });
  }

  sockets.ws.active_streams.subscribe(function(type,data){
    let msg;
    if (type == "log") {
      if (streamname && (data[3] != "") && (data[3] != streamname.split("+")[0])) {
        return;
      }
      if (data[1] == "ACCS") { return; }

      msg = el("div", {class: "message", "data-debuglevel": data[1]});
      const timeSpan = el("span", {class: "time"});
      timeSpan.appendChild(el("span", {class: "description"}, "["));
      timeSpan.appendChild(el("span", null, format.dateTime(data[0])));
      timeSpan.appendChild(el("span", {class: "description"}, "] "));
      msg.appendChild(timeSpan);
      msg.appendChild(el("span", {class: "binary", title: "binary name"}, data[5]));
      const streamSpan = el("span", {class: "stream", title: "stream name"});
      if (data[3]) {
        streamSpan.appendChild(el("span", {class: "description"}, ":"));
        streamSpan.appendChild(el("span", null, data[3]));
      }
      msg.appendChild(streamSpan);
      const pidSpan = el("span", {class: "pid", title: "pid"});
      if (data[4]) {
        pidSpan.appendChild(el("span", {class: "description"}, " ("));
        pidSpan.appendChild(el("span", null, String(data[4])));
        pidSpan.appendChild(el("span", {class: "description"}, ")"));
      }
      msg.appendChild(pidSpan);
      const dlSpan = el("span", {class: "debuglevel", title: "debug level"});
      dlSpan.appendChild(el("span", {class: "description"}, " ["));
      dlSpan.appendChild(el("span", null, data[1]));
      dlSpan.appendChild(el("span", {class: "description"}, "]"));
      msg.appendChild(dlSpan);
      const msgSpan = el("span", {class: "message"});
      msgSpan.appendChild(document.createTextNode(" "));
      msgSpan.appendChild(highlightLogText(data[2]));
      msg.appendChild(msgSpan);
      const lineSpan = el("span", {class: "line copy_but_hide", title: "line number"});
      lineSpan.innerHTML = data[6] ? "&nbsp;("+data[6]+")" : "";
      msg.appendChild(lineSpan);

    }
    else if (type == "error") {
      msg = el("div", null, data);
    }

    if (msg) {
      queue.push(msg);
      if (!busy) addMessages();
    }

  });

  const logSection = createStreamSlab({
    className: "logs stream-slab--logs",
    title: APP_NAME+" logs",
    icon: "terminal-square"
  });
  const openRawBtn = el("button", null, "Open raw");
  openRawBtn.addEventListener('click', function(){
    tab = window.open("", APP_NAME+" logs"+(streamname ? " for "+streamname : ""));
    tab.document.write(
      "<html><head><title>"+APP_NAME+" logs"+(streamname ? " for '"+streamname+"'" : "")+"</title><meta http-equiv=\"content-type\" content=\"application/json; charset=utf-8\"><style>body{padding-left:2em;text-indent:-2em;font-family:monospace}.description,.message :is(.time,.binary,.pid,.line){font-size:.9em;color:#777}.message:is([data-debuglevel=\"WARN\"],[data-debuglevel=\"ERROR\"],[data-debuglevel=\"FAIL\"]){font-weight:bold;}</style></head><body>"
    );
    tab.document.write(logsDiv.innerHTML);
    tab.document.scrollingElement.scrollTop = tab.document.scrollingElement.scrollHeight;
  });
  const downBtn = el("button", {class: "down", "data-icon": "down", title: "Snap to bottom"});
  downBtn.addEventListener('click', function(){
    scroll = true;
    logsDiv.scrollTop = logsDiv.scrollHeight;
  });
  const filterInput = el("input", {type: "text", class: "logs-filter", placeholder: "Filter logs\u2026"});
  filterInput.addEventListener('input', function() {
    filterTerm = this.value.toLowerCase();
    var msgs = logsDiv.children;
    for (var i = 0; i < msgs.length; i++) {
      msgs[i].hidden = filterTerm && msgs[i].textContent.toLowerCase().indexOf(filterTerm) === -1;
    }
  });

  const bufferSelect = el("select", {class: "logs-buffer-select", title: "Client-side log buffer size"});
  const bufferOptions = [
    {value: "1000", label: "1K lines"},
    {value: "5000", label: "5K lines"},
    {value: "10000", label: "10K lines"},
    {value: "50000", label: "50K lines"},
    {value: "0", label: "Unlimited"}
  ];
  for (const opt of bufferOptions) {
    const o = el("option", {value: opt.value}, opt.label);
    if (parseInt(opt.value) === maxBuffer) o.selected = true;
    bufferSelect.appendChild(o);
  }
  bufferSelect.addEventListener("change", function(){
    maxBuffer = parseInt(this.value);
    localStorage.setItem("logs-buffer-size", maxBuffer);
  });

  const tools = el("div", {class: "stream-slab-tools"});
  tools.appendChild(filterInput);
  if (options.showRawInTools !== false) {
    tools.appendChild(openRawBtn);
  }
  tools.appendChild(bufferSelect);
  tools.appendChild(downBtn);
  logSection.slabBody.appendChild(tools);
  logSection.slabBody.appendChild(logsDiv);
  logSection.rawButton = openRawBtn;
  return logSection;
};
