import { uiHelpers } from '../core/ui_helpers.js';
import { apiClient } from '../core/api_client.js';
import { mistHelpers } from '../core/mist_helpers.js';
import { uiCore } from '../core/ui_core.js';
import { copy } from '../core/clipboard.js';
import { el } from '../core/dom_helpers.js';
import { parseURL } from '../core/app_state.js';

export const streamHints = {
findFolderSubstreams: function(stream,callback){

  function createWcStreamObject(streamname,parent) {
    let wcstream = Object.assign({},parent);
    delete wcstream.meta;
    delete wcstream.error;
    wcstream.online = 2; //should either be available (2) or active (1)
    wcstream.name = streamname;
    wcstream.ischild = true;
    return wcstream;
  }

  function hasMatchingInput(filename){
    for (const j in mist.data.capabilities.inputs) {
      if ((j.indexOf('Buffer') >= 0) || (j.indexOf('Buffer.exe') >= 0) || (j.indexOf('Folder') >= 0) || (j.indexOf('Folder.exe') >= 0)) { continue; }
      if (mistHelpers.inputMatch(mist.data.capabilities.inputs[j].source_match,"/"+filename)) {
        return true;
      }
    }
    return false;
  }

  apiClient.send(function(d,opts){
    const s = stream.name;
    let matches = 0;
    const output = {};
    for (const i in d.browse.files) {
      if (hasMatchingInput(d.browse.files[i])) {
        const streamname = s+'+'+d.browse.files[i];
        output[streamname] = createWcStreamObject(streamname,stream);
        output[streamname].source = stream.source+d.browse.files[i];

        matches++;
        /*
          if (matches >= 50) {
            //stop retrieving more file names TODO properly display when this happens
            output[s+"+zzzzzzzzz"] = {
              name: "... (too many substreams found)",
              online: -1
            };
            break;
          }
        */
      }
    }
    if (('files' in d.browse) && (d.browse.files.length)) {
      stream.filesfound = true;
    }
    else {
      stream.filesfound = false;
    }
    callback(output);
  },{browse:stream.source});
},
findStreamKeys: function(streamname) {
  if ("streamkeys" in mist.data) {
    let streamkeys = [];
    if (typeof mist.data.streamkeys == "object"){
      for (const key in mist.data.streamkeys) {
        if (mist.data.streamkeys[key] == streamname) { streamkeys.push(key); }
      }
    }
    return streamkeys;
  }
  throw "Please request streamkeys first.";
},
findInputBySource: function(source) {
  if (source == '') { return null; }
  if ("capabilities" in mist.data) {

    //check for cache
    let matches = {};
    if ("matches" in mist.data.capabilities.inputs) {
      matches = mist.data.capabilities.inputs.matches;
    }
    else {
      //gather a list of match strings
      for (const i in mist.data.capabilities.inputs) {
        let input = mist.data.capabilities.inputs[i];
        if (typeof input.source_match == 'undefined') { continue; }
        let m = input.source_match;
        if (typeof m == "string") {
          m = [m];
        }
        for (const str of m) {
          if ((str in matches) && (matches[str] != i)) {
            //uhoh: duplicate match string for input i and matches[str]: use input with highest input.priority
            if (input.priority > ((a)=>a ? a : 0)(mist.data.capabilities.inputs[matches[str]].priority)) {
              matches[str] = i;
            }
          }
          else {
            matches[str] = i;
          }
        }
      }
    }

    let sorted;
    if ("matches_sorted" in mist.data.capabilities.inputs) {
      sorted = mist.data.capabilities.inputs.matches_sorted;
    }
    else {

      //sort them by length: a more detailed match gets priority
      sorted = Object.keys(matches).sort((a,b) => b.length - a.length);

      //now sort them by input.name+":": these are overrides and should take priority
      let inputs = {};
      for (const input of Object.keys(mist.data.capabilities.inputs)) {
        inputs[input.replace(".exe","").toLowerCase()] = 1;
      }
      function isOverride(str) {
        if (str.slice(-2) == ":*") {
          let input_name = str.slice(0,-2);
          if (input_name in inputs) return 1;
        }
        return 0;
      }
      sorted.sort((a,b)=>{
        return isOverride(b) - isOverride(a);
      });

      //for caching purposes :)
      //these properties will not be enumerable
      //they will be overwritten (with nothing) when capabilities are refreshed
      Object.defineProperty(mist.data.capabilities.inputs,"matches",{
        value: matches
      });
      Object.defineProperty(mist.data.capabilities.inputs,"matches_sorted",{
        value: sorted
      });
    }

    for (const match_string of sorted) {
      if (mistHelpers.inputMatch(match_string,source)) {
        let input_name = matches[match_string];
        mist.data.capabilities.inputs[input_name].index = input_name; //'INPUT.exe' for windows, whereas input.name would be 'INPUT'
        return mist.data.capabilities.inputs[input_name];
      }
    }
    return null; //no matching input for this source
  }
  else {
    throw "Please request capabilities first.";
  }
},
updateLiveStreamHint: function(streamname,source,$cont,input,streamkeys) {
  let rawmode;
  if ($cont == "raw") {
    rawmode = {};
    $cont = false;
  }
  let cont = $cont || el('span');
  cont.innerHTML = "";
  if (!streamname || !source) {
    return rawmode || cont;
  }
  if (!input) {
    input = streamHints.findInputBySource(source);
    if (input === null) {
      return rawmode || cont;
    }
  }
  if (!streamkeys) {
    streamkeys = streamHints.findStreamKeys(streamname);
  }

  let show;
  switch (input.name) {
    case 'Buffer':
    case 'Buffer.exe':
      show = ["RTMP","TSSRT","RTSP","WebRTC"];
      break;
    case 'TS':
    case 'TS.exe':
      if ((source.charAt(0) != "/") && (source.slice(0,7) != "ts-exec")) {
        show = ["TS"];
      }
      break;
    case 'TSSRT':
    case 'TSSRT.exe': {
      show = ["TSSRT"];
      break;
    }
  }
  if (!show) {
    return rawmode || cont;
  }

  const host = parseURL(mist.user.host);
  //var source = $main.find('[name=source]').val();
  let passw = source.match(/@.*/);
  if (passw) { passw = passw[0].substring(1); }
  let ip = source.replace(/(?:.+?):\/\//,'');
  ip = ip.split('/');
  ip = ip[0];
  ip = ip.split(':');
  ip = ip[0];
  const custport = source.match(/:\d+/)?.[0];

  const matchhost = source.match(/^push:\/\/([^:@\/]*)/)?.[1];
  if (matchhost != "invalid,host") {
    streamkeys = [streamname].concat(streamkeys.map((v)=>encodeURIComponent(v)));
  }

  let trythese = ['RTMP','RTSP','TSSRT','WebRTC','HTTPS','HTTP'];
  for (let i = trythese.length - 1; i >= 0; i--) {
    trythese.push(trythese[i]+".exe");
  }
  //add .exe to each for windowz

  let mistdefport = {}; //retrieve MistServer's default ports from capabilities
  for (let i = trythese.length - 1; i >= 0; i--) {
    let p = trythese[i];
    if (p in mist.data.capabilities.connectors) {
      mistdefport[p.replace(".exe","")] = mist.data.capabilities.connectors[p].optional.port['default'];
    }
    else {
      //not in capabilities, stop trying
      trythese.splice(i,1);
    }
  }
  let defport = { //these are the default ports used when using the protocol - not necesarily the same as MistServer's
    RTMP: 1935,
    RTSP: 554,
    HTTP: 80,
    HTTPS: 443,
    TSSRT: -1,
    TS: -1,
  };
  let ports = {};
  for (const protocol of trythese) {
    let protocolname = protocol.replace(".exe","");
    ports[protocolname] = [];
    for (const i in mist.data.config.protocols) {
      const p = mist.data.config.protocols[i];
      if (p.connector == protocol) {
        let port = false;
        if (protocolname in mistdefport) port = ":"+mistdefport[protocolname];
        if ("port" in p) port = ":"+p.port;
        if (port == (":"+defport[protocolname])) port = "";

        switch (protocolname) {
          case "TSSRT": {
            if (p.acceptable == 1) port = false; //this SRT config only allows outgoing connections
            if (port !== false) {
              port = {
                port: port,
                passphrase: p.passphrase
              }
            }
            break;
          }
          case "HTTP":
          case "HTTPS": {
            if (("pubaddr" in p) && (p.pubaddr.length)) {
              ports[protocolname].push(p.pubaddr);
              //the ports array will contain strings: the ports, and arrays, the public urls
            }
            break;
          }
        }

        if (port !== false) ports[protocolname].push(port);
      }
    }
  }
  //TODO webrtc input

  const context_menu = rawmode ? null : new uiCore.context_menu();

  function createSection(kind) {
    let label = kind;
    function createSpan(text,help) {
      if (rawmode) return true;

      const span = el('span', {class: 'value clickable', title: help});
      span.textContent = text;

      span.addEventListener('contextmenu', function(e) {
        e.preventDefault();

        const headerDiv = el('div', {class: 'header'});
        const textDiv = el('div');
        textDiv.textContent = text;
        headerDiv.appendChild(textDiv);
        if (help) {
          const descDiv = el('div', {class: 'description'});
          descDiv.textContent = help;
          headerDiv.appendChild(descDiv);
        }

        context_menu.show([
          [headerDiv],
          [[
            "Copy to clipboard",function(){
              copy(text).then(()=>{
                this._setText("Copied!")
                setTimeout(function(){ context_menu.hide(); },300);
              }).catch((e)=>{
                this._setText("Copy: "+e);
                setTimeout(function(){ context_menu.hide(); },300);
                uiHelpers.openCopyFallback({
                  title: "Copy to clipboard",
                  error: e,
                  label: "Text",
                  text: text
                });
              });
            },"copy","Copy "+text+" to the clipboard"
          ]]
        ],e);
      });

      span.addEventListener('click', function(e) {
        this.dispatchEvent(new MouseEvent("contextmenu",e));
        e.stopPropagation();
      });

      return span;
    }

    let valuesContainer = el('span', {class: 'values'});
    let values = [];
    switch (kind) {
      case "RTMP": {
        function buildRTMPurl(nopass,port) {
          return "rtmp://"+host.host+port+"/"+(passw && !nopass ? passw : "live")+"/";
        }
        if (rawmode) {
          values = {
            full_url: [],
            pairs: {}
          };
          for (const port of ports["RTMP"]) {
            for (const key of streamkeys) {
              values.full_url.push(buildRTMPurl(key != streamname,port) + key);
            }
            for (const key of streamkeys) {
              const url = buildRTMPurl(key != streamname,port);
              if (!(url in values.pairs)) { values.pairs[url] = []; }
              values.pairs[url].push(key);
            }
          }
          break;
        }

        for (const port of ports["RTMP"]) {
          let subDiv = el('div', {class: 'sub'});
          for (const key of streamkeys) {
            const labelEl = el('label');
            const labelSpan = el('span', {class: 'label'});
            labelEl.appendChild(labelSpan);
            labelEl.appendChild(createSpan(buildRTMPurl(key != streamname,port) + key,"Use this RTMP url if your client doens't ask for a stream key"));
            subDiv.appendChild(labelEl);
          }
          subDiv.children[0].children[0].textContent = "Full url:";
          valuesContainer.appendChild(subDiv);

          subDiv = el('div', {class: 'sub'});
          let theurl;
          let n = 0;
          for (const key of streamkeys) {
            let newurl = buildRTMPurl(key != streamname,port);
            if (theurl != newurl) {
              if (subDiv.children.length) valuesContainer.appendChild(subDiv);
              subDiv = el('div', {class: 'sub'});

              const label1 = el('label');
              const labelSpan1 = el('span', {class: 'label'});
              labelSpan1.textContent = n ? "Or url:" : "Url:";
              label1.appendChild(labelSpan1);
              label1.appendChild(createSpan(newurl,"Use this RTMP url if your client also asks for a stream key"));
              subDiv.appendChild(label1);

              const label2 = el('label');
              const labelSpan2 = el('span', {class: 'label'});
              labelSpan2.textContent = "with key:";
              label2.appendChild(labelSpan2);
              label2.appendChild(createSpan(key));
              subDiv.appendChild(label2);

              theurl = newurl;
              n++;
            }
            else {
              const labelEl = el('label');
              const labelSpan = el('span', {class: 'label'});
              labelEl.appendChild(labelSpan);
              labelEl.appendChild(createSpan(key));
              subDiv.appendChild(labelEl);
            }
          }
          valuesContainer.appendChild(subDiv);
        }

        break;
      }
      case "TSSRT": {
        label = "SRT";
        if (source.slice(0,6) == "srt://") { 
          let out;
          if (custport) {
            let source_parsed = parseURL(source.replace());
            if (source_parsed.host == "") {
              //url is invalid, parser gets funky
              source_parsed = parseURL(source.replace(/^srt:\/\//,"http://localhost"));
              source_parsed.host = source_parsed.host.replace(/^localhost/,"");
            }
            if ((source_parsed.host != "") && (!source_parsed.search || !source_parsed.searchParams || source_parsed.searchParams.get("mode") != "listener")) {
              out = "Caller mode: pulling stream from provided source.";
            }
            else if (source_parsed.search && source_parsed.searchParams && (source_parsed.searchParams.get("mode") == "caller")) {
              out = "Caller mode: you should probably add an address.";
            }
            else {
              out = rawmode ? 'srt://'+host.host+custport : createSpan('srt://'+host.host+custport)
            }
            //if adres -> caller of ?mode=caller, geen push url
            //als ?mode=listener, wel push url
          }
          else {
            out = "You must specify a port.";
          }
          values.push(out);
          if (typeof out == "string") {
            const valueSpan = el('span', {class: 'value'});
            valueSpan.textContent = out;
            valuesContainer.appendChild(valueSpan);
          } else {
            valuesContainer.appendChild(out);
          }
        }
        else {
          for (const port of ports["TSSRT"]) {
            for (const key of streamkeys) {
              if (passw && (key == streamname)) {
                //if there is a @password, SRT cannot set it. So this will only work if there is a streamkey
                if (streamkeys.length <= 1) {
                  let out = "SRT cannot be used for input if there is a @password. Did you want to configure an SRT passphrase instead?";
                  values.push(out);
                  const valueSpan = el('span', {class: 'value'});
                  valueSpan.textContent = out;
                  valuesContainer.appendChild(valueSpan);
                }
                continue;
                //TODO remove?
                if ((passw.length < 10) || (passw.length > 79)) {
                  let out = "For SRT, the password length must be between 10 and 79 characters.";
                  values.push(out);
                  const valueSpan = el('span', {class: 'value'});
                  valueSpan.textContent = out;
                  valuesContainer.appendChild(valueSpan);
                  continue;
                }
              }
              let out = 'srt://'+host.host+port.port+'?streamid='+key+(port.passphrase ? "&passphrase="+port.passphrase : "");
              values.push(out);
              valuesContainer.appendChild(createSpan(out));
            }
          }
        }
        break;
      }
      case "WebRTC": {
        label += " (WHIP)";
        
        //http(s)://localhost/webrtc/streamname
        //TODO skip if passw
        
        //gather hosts
        let hosts = {}; //in an object to de-duplicate
        for (const kind of ["HTTP","HTTPS"]) {
          for (const port of ports[kind]) {
            if (typeof port == "string") {
              //it's a port
              hosts[kind.toLowerCase()+"://"+host.host+port] = 1;
            }
            else {
              //it's an array of public urls
              for (const url of port) {
                hosts[url] = 1;
              }
            }
          }
        }
        hosts = Object.keys(hosts);

        for (const host of hosts) {
          for (const key of streamkeys) {
            if (passw && (key == streamname)) {
              //@password is not implemented for WebRTC WHIP input
              if (streamkeys.length <= 1) {
                let out = label+" cannot be used for input if there is a @password.";
                values.push(out);
                const valueSpan = el('span', {class: 'value'});
                valueSpan.textContent = out;
                valuesContainer.appendChild(valueSpan);
              }
              continue;
            }

            let out = host+(host[host.length-1] == "/" ? "" : "/")+"webrtc/"+key;
            values.push(out);
            valuesContainer.appendChild(createSpan(out));
          }
        }

        break;
      }
      case "RTSP": {
        for (const port of ports["RTSP"]) {
          for (const key of streamkeys) {
            let out = 'rtsp://'+host.host+port+'/'+(key)+(passw && (key == streamname) ? '?pass='+passw : '')
            values.push(out);
            valuesContainer.appendChild(createSpan(out));
          }
        }
        break;
      }
      case "TS": {
        if ((source.charAt(0) == "/") || (source.slice(0,7) == "ts-exec")) {
          return; //do not return section
        }
        if (source.slice(0,8) == "tsudp://") {
          let out = 'udp://'+(ip == '' ? host.host : ip)+(custport ? custport : ":[port]")+'/';
          values.push(out);
          valuesContainer.appendChild(createSpan(out));
        }
        else { return; }
        break;
      }
    }

    if (rawmode) {
      rawmode[label] = values;
      return;
    }

    const section = el('label');
    const labelSpan = el('span', {class: 'label'});
    labelSpan.textContent = label+":";
    section.appendChild(labelSpan);
    section.appendChild(valuesContainer);

    return section;
  }


  if (rawmode) {
    for (const kind of show) {
      createSection(kind);
    }
  }
  else {
    // One flush section (not a nested slab). By default CSS collapses the body
    // once the source is live (ancestor [data-state]) and expands it otherwise;
    // the toggle lets the user override that default in either direction. The
    // toggle's label is CSS-driven so it always matches the body's visibility.
    const canCollapse = streamkeys.length > 0;
    cont.classList.add("livestreamhint");
    cont.classList.toggle("livestreamhint--collapsible", canCollapse);

    const head = el('div', {class: 'livestreamhint-head'});
    const title = el('h4', {class: 'livestreamhint-title'}, 'Configure your source to push to');
    head.appendChild(title);

    const body = el('div', {class: 'livestreamhint-body'});
    if (streamkeys.length == 0) {
      const noEndpointDiv = el('div', {class: 'livestreamhint-empty'});
      noEndpointDiv.textContent = "There is no valid push endpoint with your current settings.";
      body.appendChild(noEndpointDiv);
    }
    else {
      for (const kind of show) {
        body.appendChild(createSection(kind));
      }
      body.appendChild(context_menu.ele);
    }

    if (canCollapse) {
      const toggleBtn = el('button', {class: 'livestreamhint-toggle', type: 'button', 'aria-label': 'Toggle push endpoints'});
      toggleBtn.addEventListener('click', function(e){
        e.preventDefault();
        e.stopPropagation();
        // Flip based on what is actually rendered, so a click always does the
        // opposite of the current state regardless of the live-state default.
        const hidden = window.getComputedStyle(body).display === 'none';
        cont.classList.toggle('livestreamhint--open', hidden);
        cont.classList.toggle('livestreamhint--closed', !hidden);
      });
      head.appendChild(toggleBtn);
    }

    const infoWrap = el('span', {class: 'livestreamhint-info'});
    const infoSpan = el('span', {class: 'info', 'data-icon': 'info'});
    infoWrap.appendChild(infoSpan);
    head.appendChild(infoWrap);

    cont.appendChild(head);
    cont.appendChild(body);
  }
  return rawmode || cont;
}
};
