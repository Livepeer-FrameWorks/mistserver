import { APP_NAME } from '@brand';
import { registerTab } from '../core/tab_registry.js';
import { uiHelpers } from '../core/ui_helpers.js';
import * as format from '../core/formatters.js';
import { apiClient } from '../core/api_client.js';
import { navto } from '../core/navigation.js';
import { tabView } from '../core/tab_view.js';
import { el } from '../core/dom_helpers.js';
import { parseURL } from '../core/app_state.js';
import { updateHintsFooter } from '../components/hints_footer.js';
import { defineSection } from '../core/section_registry.js';

defineSection({
  id: 'overview/stats',
  page: 'Overview',
  title: 'Server Statistics',
  keywords: ['configured streams', 'active streams', 'connections', 'viewers', 'clients'],
  requires: ['active_streams', 'totals'],
  render: function($container) {
    var h = uiHelpers;
    var configuredCount = mist.data.streams ? Object.keys(mist.data.streams).length : 0;
    var activeCount = mist.data.active_streams ? mist.data.active_streams.length : 0;
    var clients = 0;
    if (mist.data.totals && mist.data.totals.all_streams) {
      var c = mist.data.totals.all_streams.all_protocols.clients;
      if (c && c.length) clients = c[c.length - 1][1];
    }
    var stats = el('div', {class: 'overview-stats'});
    stats.appendChild(h.statCard('list', el('span', {class: 'stat-value'}, String(configuredCount)), 'Configured streams'));
    stats.appendChild(h.statCard('activity', el('span', {class: 'stat-value'}, String(activeCount)), 'Active streams'));
    stats.appendChild(h.statCard('users', el('span', {class: 'stat-value'}, String(clients)), 'Current connections'));
    $container.appendChild(stats);
  },
  navTo: { tab: 'Overview', other: '' }
});

defineSection({
  id: 'overview/server-info',
  page: 'Overview',
  title: 'Server Information',
  keywords: ['version', 'server time', 'license', 'licensed', 'uptime'],
  requires: ['config'],
  render: function($container) {
    var h = uiHelpers;
    var section = el('section', {class: 'overview-section'});
    section.appendChild(h.infoRow('Version', el('span', null, mist.data.config.version || 'Unknown')));
    section.appendChild(h.infoRow('Server time', el('span', null, format.dateTime(mist.data.config.time, 'long'))));
    if (mist.data.config.license) {
      section.appendChild(h.infoRow('Licensed to', el('span', null, mist.data.config.license.user || '')));
    }
    $container.appendChild(section);
  },
  navTo: { tab: 'Overview', other: '' }
});

defineSection({
  id: 'overview/protocols-status',
  page: 'Overview',
  title: 'Enabled Protocols',
  subtitle: 'Protocol connector status',
  keywords: ['protocol', 'enabled', 'disabled', 'connector', 'HTTPS'],
  requires: ['config', 'capabilities'],
  render: function($container) {
    var on = [];
    var off = [];
    var connectors = mist.data.capabilities && mist.data.capabilities.connectors;
    function isPushOnly(name) {
      return connectors && connectors[name] && ('PUSHONLY' in connectors[name]);
    }
    if (mist.data.config.protocols) {
      for (var i = 0; i < mist.data.config.protocols.length; i++) {
        var p = mist.data.config.protocols[i];
        if (isPushOnly(p.connector)) continue;
        if (on.indexOf(p.connector) < 0) on.push(p.connector);
      }
    }
    if (connectors) {
      for (var name in connectors) {
        if (isPushOnly(name)) continue;
        if (on.indexOf(name) < 0) off.push(name);
      }
    }
    var section = el('section', {class: 'overview-section'});
    var onRow = el('div', {class: 'overview-protocols-list'});
    for (var j = 0; j < on.length; j++) {
      onRow.appendChild(el('span', {class: 'cam-pill enabled'}, on[j]));
    }
    if (!on.length) onRow.textContent = 'None.';
    section.appendChild(el('div', {class: 'proto-heading'}, 'Enabled'));
    section.appendChild(onRow);
    if (off.length) {
      var offRow = el('div', {class: 'overview-protocols-list'});
      for (var k = 0; k < off.length; k++) {
        offRow.appendChild(el('span', {class: 'cam-pill disabled'}, off[k]));
      }
      section.appendChild(el('div', {class: 'proto-heading'}, 'Disabled'));
      section.appendChild(offRow);
    }
    $container.appendChild(section);
  },
  navTo: { tab: 'Overview', other: '' }
});

registerTab('Overview', function(tab, other, prev, $main, $pageHeader, $pageFooter) {

  if (typeof mist.data.bandwidth == 'undefined') {
    apiClient.send(function(d){
      navto(tab);
    },{bandwidth: true});
    $main.appendChild(document.createTextNode('Loading..'));
    return;
  }

  const versioncheck = el('span');
  versioncheck.textContent = 'Loading..';
  const streamsactive = el('span');
  const errors = el('div', {class: 'logs'});
  const viewers = el('span');
  const servertime = el('span');
  const activeproducts = el('span');
  activeproducts.textContent = 'Unknown';
  const protocols_on = el('span', {class: 'overview-protocols-list'});
  const protocols_off = el('span', {class: 'overview-protocols-list'});

  let host = parseURL(mist.user.host);
  host = host.protocol+host.host+host.port;

  // --- Stat cards ---
  const configuredVal = el('span', {class: 'stat-value'});
  configuredVal.textContent = mist.data.streams ? Object.keys(mist.data.streams).length : 0;
  const activeVal = el('span', {class: 'stat-value'});
  activeVal.appendChild(streamsactive);
  const connectionsVal = el('span', {class: 'stat-value'});
  connectionsVal.appendChild(viewers);

  const h = uiHelpers;

  const stats = el('div', {class: 'overview-stats'});
  stats.appendChild(h.statCard('list', configuredVal, 'Configured streams'));
  stats.appendChild(h.statCard('activity', activeVal, 'Active streams'));
  stats.appendChild(h.statCard('users', connectionsVal, 'Current connections'));

  const serverInfo = el('section', {class: 'overview-section'});
  const serverH3 = el('h3');
  serverH3.textContent = 'Server';
  serverInfo.appendChild(serverH3);
  const versionSpan = el('span');
  versionSpan.textContent = mist.data.config.version;
  serverInfo.appendChild(h.infoRow('Version', versionSpan));
  serverInfo.appendChild(h.infoRow('Version check', versioncheck));
  serverInfo.appendChild(h.infoRow('Server time', servertime));

  if ("license" in mist.data.config) {
    const licensedSpan = el('span');
    licensedSpan.textContent = mist.data.config.license.user || "";
    serverInfo.appendChild(h.infoRow('Licensed to', licensedSpan));
    serverInfo.appendChild(h.infoRow('Active licenses', activeproducts));
  }

  // --- Protocols section ---
  const protocolsSection = el('section', {class: 'overview-section'});
  const protoH3 = el('h3');
  protoH3.textContent = 'Protocols';
  protocolsSection.appendChild(protoH3);

  const protoGroupOn = el('div', {class: 'overview-protocols-group'});
  const protoHeadingOn = el('div', {class: 'proto-heading'});
  protoHeadingOn.textContent = 'Enabled';
  protoGroupOn.appendChild(protoHeadingOn);
  protoGroupOn.appendChild(protocols_on);
  protocolsSection.appendChild(protoGroupOn);

  const protoGroupOff = el('div', {class: 'overview-protocols-group'});
  const protoHeadingOff = el('div', {class: 'proto-heading'});
  protoHeadingOff.textContent = 'Disabled';
  protoGroupOff.appendChild(protoHeadingOff);
  protoGroupOff.appendChild(protocols_off);
  protocolsSection.appendChild(protoGroupOff);

  const grid = el('div', {class: 'overview-grid'});
  grid.appendChild(serverInfo);
  grid.appendChild(protocolsSection);

  // --- Recent problems section ---
  const problemsSection = el('section', {class: 'overview-section overview-problems'});
  const problemsH3 = el('h3');
  problemsH3.textContent = 'Recent problems';
  problemsSection.appendChild(problemsH3);
  problemsSection.appendChild(errors);

  function updateHints() {
    updateHintsFooter('Overview');
  }

  // --- Config action buttons (in page header) ---
  function setActionState(button, icon, label) {
    button.setAttribute("data-icon", icon);
    button.textContent = label;
  }
  function lockAction(button) {
    if (button._busy) { return false; }
    button._busy = true;
    button.disabled = true;
    return true;
  }
  function releaseAction(button, icon, label, delay) {
    setTimeout(function(){
      button._busy = false;
      button.disabled = false;
      setActionState(button, icon, label);
    }, delay || 0);
  }
  function headerButton(icon, label, tip) {
    const btn = el("button", {class: "overview-header-action"});
    btn.setAttribute("data-icon", icon);
    btn.textContent = label;
    btn.addEventListener("mouseenter", function(e){
      UI.tooltip.show(e, tip);
    });
    btn.addEventListener("mousemove", function(e){
      UI.tooltip.show(e, tip);
    });
    btn.addEventListener("focus", function(e){
      UI.tooltip.show(e, tip);
    });
    btn.addEventListener("mouseleave", function(){
      UI.tooltip.hide();
    });
    btn.addEventListener("blur", function(){
      UI.tooltip.hide();
    });
    return btn;
  }

  const saveBtn = headerButton("disk","Save","Force an immediate save to the config.json "+APP_NAME+" uses to save your settings.");
  saveBtn.addEventListener('click', function(){
    const me = this;
    if (!lockAction(me)) { return; }
    setActionState(me,"loader","Saving...");
    apiClient.send(function(){
      setActionState(me,"check","Saved!");
      releaseAction(me,"disk","Save",5e3);
    },{save:true});
  });

  const downloadBtn = headerButton("down","Download","Download the current "+APP_NAME+" configuration file as a backup.");
  downloadBtn.addEventListener('click', function(){
    const me = this;
    if (!lockAction(me)) { return; }
    setActionState(me,"loader","Loading...");
    apiClient.send(function(d){
      setActionState(me,"check","Done!");

      const file = new Blob([JSON.stringify(d.config_backup)], {type: "text/plain"});
      const filename = APP_NAME+"_config_"+(mist.data.config.serverid ? mist.data.config.serverid+"_" : "")+(new Date().toISOString())+".json";
      function showErr(err) {
        setActionState(me,"cross",err.name == "AbortError" ? "Aborted" : "Failed");
        releaseAction(me,"down","Download",5e3);
      }
      if (window.showSaveFilePicker) {
        try {
          showSaveFilePicker({
            startIn: "downloads",
            suggestedName: filename
          }).then(function(handle){
            handle.createWritable().then(function(writable){
              writable.write(file).then(function(){
                writable.close().then(function(){
                  setActionState(me,"check","Saved!");
                  releaseAction(me,"down","Download",5e3);
                }).catch(showErr);
              }).catch(showErr);
            }).catch(showErr);
          }).catch(showErr);
          return;
        }
        catch (err) {
          showErr(err);
        }
      }
      if (window.navigator.msSaveOrOpenBlob) {
        window.navigator.msSaveOrOpenBlob(file,filename);
      }
      else {
        const a = document.createElement("a");
        const url = URL.createObjectURL(file);
        a.href = url;
        a.download = filename;
        document.body.appendChild(a);
        a.click();
        setTimeout(function() {
          document.body.removeChild(a);
          window.URL.revokeObjectURL(url);
          setActionState(me,"check","Saved!");
        },0);
      }

      releaseAction(me,"down","Download",5e3);
    },{config_backup:true});
  });

  const uploadBtn = headerButton("up","Upload","Upload a "+APP_NAME+" configuration file to restore a backup or import from another instance. Your current config will be overwritten!");
  uploadBtn.addEventListener('click', function(){
    const me = this;
    if (!lockAction(me)) { return; }
    function resetUploadButton() {
      releaseAction(me,"up","Upload",5e3);
    }
    function showErr(err) {
      let aborted = false;
      if (err && (typeof err == "object") && ("name" in err)) {
        aborted = (err.name == "AbortError");
      }
      else if ((typeof err == "string") && (err == "Upload canceled")) {
        aborted = true;
      }
      setActionState(me,"cross",aborted ? "Aborted" : "Failed");
      resetUploadButton();
    }

    function compareAndSubmit(file) {
      return new Promise(function(resolve,reject){
        file.text().then(function(text){
          let json;
          try {
            json = JSON.parse(text);
          }
          catch (e) {
            reject("Selected file does not contain json");
          }
          if (json){
            apiClient.send(function(d){
              const currentconfig = d.config_backup;

              const out = [];
              const map = {
                streams:   function(d){ return d.streams ? Object.keys(d.streams).length : 0 },
                protocols: function(d){ return d.config && d.config.protocols ? d.config.protocols.length : 0 },
                "automatic pushes":    function(d){ return d.auto_push ? Object.keys(d.auto_push).length : 0 },
                triggers:  function(d){ return d.config && d.config.triggers ? Object.entries(d.config.triggers).map(function(a){ return a[1].length}).reduce(function(sum,a){ return sum+a; },0) : 0 }
              };
              for (const kind in map) {
                const o = map[kind](currentconfig);
                const n = map[kind](json);
                if (o != n) {
                  out.push("- "+format.capital(kind)+": "+o+" to "+n);
                }
              }
              let msg = "Are you sure you want to apply this config? Your current config will be overwritten!\n\n";
              if (out.length) {
                msg += "These are some of the changes:\n";
                msg += out.join("\n")+"\n";
              }
              else {
                const kinds = Object.keys(map);
                msg += "There are no changes in the amount of "+kinds.slice(0,-1).join(", ")+" or "+kinds.slice(-1)+" configured.\n";
              }
              const lcurrent = new TextEncoder().encode(JSON.stringify(currentconfig)).length;
              const lnew = file.size;
              if (lcurrent != lnew) {
                let perc = lnew/lcurrent;
                msg += "The config file size will be "
                if (perc > 1) {
                  perc--;
                  msg += "increased";
                }
                else {
                  perc = 1 - perc;
                  msg += "decreased";
                }
                msg += " by "+Math.round(perc*100)+"%.\n";
              }
              else {
                msg += "The config file size will not change.\n"
              }


              if (confirm(msg)) {
                setActionState(me,"loader","Uploading...");
                apiClient.send(function(){
                  setActionState(me,"check","Applied!");
                  resolve();
                  navto('Overview');
                },{config_restore: json});
              }
              else {
                reject("Upload canceled");
              }
            },{config_backup: true});

          }
          else {
            reject("Selected file does not contain json");
          }
        }).catch(reject);
      });
    }

    setActionState(me,"loader","Selecting...");
    if (window.showOpenFilePicker) {
      try {
        showOpenFilePicker({
          startIn: "downloads",
          types: [
            {
              description: "Configuration files",
              accept: {
                "text/*": [".json",".config",".cfg",".conf",".txt"]
              }
            }
          ]
        }).then(function(handles){
          handles[0].getFile().then(function(file){
            compareAndSubmit(file).catch(showErr);
          }).catch(showErr);
        }).catch(showErr);
      }
      catch (e) {
        showErr(e);
      }
    }
    else {
      let selectionHandled = false;
      const fileInput = document.createElement("input");
      fileInput.setAttribute("type","file");
      fileInput.hidden = true;
      fileInput.addEventListener('change', function(e){
        selectionHandled = true;
        if (this.files && this.files.length) {
          compareAndSubmit(this.files[0]).catch(showErr).finally(function(){
            window.removeEventListener("focus", onPickerClose);
            fileInput.remove();
          });
          return;
        }
        showErr({name:"AbortError"});
        window.removeEventListener("focus", onPickerClose);
        fileInput.remove();
      });
      function onPickerClose() {
        window.removeEventListener("focus", onPickerClose);
        setTimeout(function(){
          if (!selectionHandled) {
            showErr({name:"AbortError"});
            fileInput.remove();
          }
        },150);
      }
      window.addEventListener("focus", onPickerClose);
      document.body.appendChild(fileInput);
      fileInput.click();
    }
  });

  h.appendPageActions($pageHeader, [saveBtn, downloadBtn, uploadBtn]);

  // --- Assemble page ---
  $main.classList.add('page-body--flex-col', 'overview-page-body', 'overview-page-shell', 'slab-shell', 'slab-shell--seamed');
  $main.appendChild(stats);
  $main.appendChild(grid);
  $main.appendChild(problemsSection);

  // --- Version check / update logic ---
  function update_update(d) {
    function update_progress(d) {
      if (!d.update) {
        tabView.showTab("Overview");
        return;
      }
      let perc = "";
      if ("progress" in d.update) {
        perc = " ("+d.update.progress+"%)";
      }
      versioncheck.textContent = "Updating.."+perc;
      add_logs(d.log);
      if (d.update.installing){
        setTimeout(function(){
          apiClient.send(function(d){
            update_progress(d);
          },{update:true});
        },1e3);
      }
    }
    function add_logs(log) {
      let msgs = log.filter(function(a){return a[1] == "UPDR"});

      let last;
      let n = 1;
      for (const i in msgs) {
        const row = msgs[i];
        const text = row[2];
        if (text == last) {
          n++;
          row[2] = text + " (\u00d7"+n+")";
          if (i > 0) msgs[i-1] = null;
        }
        else {
          last = text;
          n = 1;
        }
      }

      msgs = msgs.filter(function(a){return a !== null;} );

      if (msgs.length) {
        const cont = el("div");
        versioncheck.appendChild(cont);
        for (const i in msgs) {
          const logDiv = el("div");
          logDiv.textContent = msgs[i][2];
          cont.appendChild(logDiv);
        }
      }
    }

    if (!("update" in d) || (d.update && !('uptodate' in d.update))) {
      versioncheck.textContent = 'Unknown, checking..';
        apiClient.send(function(d){
          if ("update" in d) {
            update_update(d);
          }
        },{update:true});
      return;
    }
    else if (!d.update) {
      versioncheck.textContent = 'Unknown, checking..';
        apiClient.send(function(d){
          if (("update" in d) && (d.update)) {
            update_update(d);
          }
          else {
            versioncheck.classList.add('red');
            versioncheck.textContent = "Failed to check for updates.";
          }
        },{checkupdate:true});
      return;
    }
    else if (d.update.error) {
      versioncheck.classList.add('red');
      versioncheck.textContent = d.update.error;
      return;
    }
    else if (d.update.uptodate) {
      versioncheck.textContent = 'Your version is up to date.';
      versioncheck.classList.add('green');
      return;
    }
    else if (d.update.progress) {
      versioncheck.classList.add('orange');
      versioncheck.classList.remove('red');
      versioncheck.textContent = 'Updating..';
      update_progress(d);
    }
    else {
      versioncheck.textContent = "";
      const updateNotice = el("span", {class: 'red'});
      updateNotice.textContent = 'On '+new Date(d.update.date).toLocaleDateString()+' version '+d.update.version+' became available.';
      versioncheck.appendChild(updateNotice);
      if (!d.update.url || (d.update.url.slice(-4) != ".zip")) {
        const rollingBtn = el('button');
        rollingBtn.textContent = 'Rolling update';
        rollingBtn.style.fontSize = '1em';
        rollingBtn.style.marginLeft = '1em';
        rollingBtn.addEventListener('click', function(){
          if (confirm('Are you sure you want to execute a rolling update?')) {
            versioncheck.classList.add('orange');
            versioncheck.classList.remove('red');
            versioncheck.textContent = 'Rolling update command sent..';

            apiClient.send(function(d){
              update_progress(d);
            },{autoupdate: true});
          }
        });
        versioncheck.appendChild(rollingBtn);
      }
      const a = el("a");
      a.setAttribute("href", d.update.url);
      a.setAttribute("target", "_blank");
      a.textContent = "Manual download";
      a.protocol = "https:";
      const aWrapper = el("div");
      aWrapper.appendChild(a);
      versioncheck.appendChild(aWrapper);
    }
    add_logs(d.log);
  }

  update_update(mist.data);

  // --- License information ---
  if ("license" in mist.data.config) {
    if (("active_products" in mist.data.config.license) && (Object.keys(mist.data.config.license.active_products).length)) {
      const licTable = el("table");
      licTable.style.textIndent = "0";
      activeproducts.innerHTML = '';
      activeproducts.appendChild(licTable);
      const licHeaderRow = el("tr");
      const thProduct = el("th");
      thProduct.textContent = "Product";
      const thUpdates = el("th");
      thUpdates.textContent = "Updates until";
      const thUse = el("th");
      thUse.textContent = "Use until";
      const thInstances = el("th");
      thInstances.textContent = "Max. simul. instances";
      licHeaderRow.appendChild(thProduct);
      licHeaderRow.appendChild(thUpdates);
      licHeaderRow.appendChild(thUse);
      licHeaderRow.appendChild(thInstances);
      licTable.appendChild(licHeaderRow);
      for (const i in mist.data.config.license.active_products) {
        const p = mist.data.config.license.active_products[i];
        const pRow = el("tr");
        const tdName = el("td");
        tdName.textContent = p.name;
        const tdUpd = el("td");
        tdUpd.innerHTML = (p.updates_final ? p.updates_final : "&infin;");
        const tdUse = el("td");
        tdUse.innerHTML = (p.use_final ? p.use_final : "&infin;");
        const tdAmt = el("td");
        tdAmt.innerHTML = (p.amount ? p.amount : "&infin;");
        pRow.appendChild(tdName);
        pRow.appendChild(tdUpd);
        pRow.appendChild(tdUse);
        pRow.appendChild(tdAmt);
        licTable.appendChild(pRow);
      }
    }
    else {
      activeproducts.textContent = "None. ";
    }
    const moreLink = el("a");
    moreLink.textContent = "More details";
    moreLink.setAttribute("href", "https://shop.mistserver.org/myinvoices");
    moreLink.setAttribute("target", "_blank");
    activeproducts.appendChild(moreLink);
  }

  // --- Dynamic stats updates ---
  function updateViewers() {
    const request = {
      totals:{
        fields: ['clients'],
        start: -10
      },
      active_streams: true
    };
    if (!('capabilities' in mist.data)) {
      request.capabilities = true;
    }
    apiClient.send(function(d){
      enterStats()
    },request);
  }
  function enterStats() {
    let active;
    if ('active_streams' in mist.data) {
      active = (mist.data.active_streams ? mist.data.active_streams.length : 0);
    }
    else {
      active = '?';
    }
    streamsactive.textContent = active;
    let clients;
    if (('totals' in mist.data) && ('all_streams' in mist.data.totals)) {
      clients = mist.data.totals.all_streams.all_protocols.clients;
      clients = (clients.length ? format.number(clients[clients.length-1][1]) : 0);
    }
    else {
      clients = 'Loading..';
    }
    viewers.textContent = clients;
    servertime.textContent = format.dateTime(mist.data.config.time,'long');

    errors.innerHTML = '';
    let n = 0;
    if (("license" in mist.data.config) && ("user_msg" in mist.data.config.license)) {
      mist.data.log.unshift([mist.data.config.license.time,"ERROR",mist.data.config.license.user_msg]);
    }
    for (const i in mist.data.log) {
      const l = mist.data.log[i];
      if (['FAIL','ERROR'].indexOf(l[1]) > -1) {
        n++;
        const contentSpan = el('span', {class: 'content red'});
        const split = l[2].split('|');
        for (const j in split) {
          const partSpan = el('span');
          partSpan.textContent = split[j];
          contentSpan.appendChild(partSpan);
        }
        const logRow = el('div');
        const timeSpan = el('span');
        timeSpan.textContent = format.time(l[0]);
        timeSpan.style.marginRight = "0.5em";
        logRow.appendChild(timeSpan);
        logRow.appendChild(contentSpan);
        errors.appendChild(logRow);
        if (n == 5) { break; }
      }
    }
    if (n == 0) {
      errors.innerHTML = 'None.';
    }

    const protocols = {
      on: [],
      off: []
    };
    function isPushOnlyConnector(connectorName) {
      if (!mist.data.capabilities || !mist.data.capabilities.connectors) { return false; }
      if (!(connectorName in mist.data.capabilities.connectors)) { return false; }
      return ("PUSHONLY" in mist.data.capabilities.connectors[connectorName]);
    }
    for (const i in mist.data.config.protocols) {
      const p = mist.data.config.protocols[i];
      if (isPushOnlyConnector(p.connector)) { continue; }
      if (protocols.on.indexOf(p.connector) > -1) { continue; }
      protocols.on.push(p.connector);
    }
    protocols_on.innerHTML = '';
    if (protocols.on.length) {
      for (let i = 0; i < protocols.on.length; i++) {
        const pillOn = el('span', {class: 'cam-pill enabled', style: 'cursor:pointer', onclick: function(){ navto('Protocols'); }});
        pillOn.textContent = protocols.on[i];
        protocols_on.appendChild(pillOn);
      }
    } else {
      protocols_on.textContent = 'None.';
    }

    let httpsFound = false;
    for (let i = 0; i < protocols.on.length; i++) {
      if (/^HTTPS(\.exe)?$/i.test(protocols.on[i])) { httpsFound = true; break; }
    }
    const existingWarn = protocolsSection.querySelector('.overview-https-warning');
    if (existingWarn) existingWarn.remove();
    if (!httpsFound) {
      const warn = el('div', {class: 'overview-https-warning'});
      warn.setAttribute('data-icon', 'alert-triangle');
      warn.textContent = 'HTTPS is not enabled. Enable HTTPS or use a reverse proxy for secure playback.';
      protocolsSection.appendChild(warn);
    }

    if ('capabilities' in mist.data) {
      for (const i in mist.data.capabilities.connectors) {
        if (isPushOnlyConnector(i)) { continue; }
        if (protocols.on.indexOf(i) == -1) {
          protocols.off.push(i);
        }
      }
      protocols_off.innerHTML = '';
      if (protocols.off.length) {
        for (let i = 0; i < protocols.off.length; i++) {
          const pillOff = el('span', {class: 'cam-pill disabled', style: 'cursor:pointer', onclick: function(){ navto('Protocols'); }});
          pillOff.textContent = protocols.off[i];
          protocols_off.appendChild(pillOff);
        }
      } else {
        protocols_off.textContent = 'None.';
      }
    }
    else {
      protocols_off.textContent = 'Loading..';
    }
    updateHints();
  }
  updateViewers();
  enterStats();
  UI.interval.set(updateViewers,30e3);

});
