import { APP_NAME } from '@brand';
import { registerTab } from '../core/tab_registry.js';
import { accordionTree } from '../components/accordion_tree.js';
import { register } from '../core/mode_dispatch.js';
import { uiHelpers } from '../core/ui_helpers.js';
import { variables } from './variables.js';
import { streamkeys } from '../streams/streamkeys.js';
import * as format from '../core/formatters.js';
import { apiClient } from '../core/api_client.js';
import { formEngine } from '../core/form_engine.js';
import { navto } from '../core/navigation.js';
import { tabView } from '../core/tab_view.js';
import { uiCore } from '../core/ui_core.js';
import { copy, upload } from '../core/clipboard.js';
import { el, getval, setval } from '../core/dom_helpers.js';
import { THEMES, getActiveThemeId, getActiveMode, getAvailableModes, applyTheme } from '../core/themes.js';

const pageIntro = uiHelpers.pageIntro;
const pageActionRow = uiHelpers.pageActionRow;
const appendPageActions = uiHelpers.appendPageActions;

function saveWithFeedback(btnEl, payload, targetTab) {
  if (btnEl._saving) { return; }
  btnEl._saving = true;
  btnEl.disabled = true;
  btnEl.textContent = "Saving..";
  apiClient.send(function() {
    btnEl.textContent = "Saved!";
    setTimeout(function() {
      navto(targetTab || 'General');
    },900);
  },payload);
}

function buildSectionEmptyState(icon, title, description) {
  return el('div', {class: 'variables-empty-state'}, [
    el('span', {'data-icon': icon}),
    el('h3', {}, title),
    el('p', {}, description)
  ]);
}

function buildThemePicker() {
  var wrap = el('div', {class: 'theme-picker'});
  var activeId = getActiveThemeId();

  for (var i = 0; i < THEMES.length; i++) {
    (function(theme) {
      var palette = theme.palettes[theme.modes[0]];
      var chip = el('button', {
        class: 'theme-chip' + (activeId === theme.id ? ' active' : ''),
        type: 'button',
        'data-theme-id': theme.id,
        style: {
          background: palette.backgroundColor,
          color: palette.textColor,
          borderColor: palette.accentColor
        },
        onclick: function() {
          var modes = getAvailableModes(theme.id);
          var prefersDark = window.matchMedia('(prefers-color-scheme: dark)').matches;
          var mode;
          if (prefersDark && modes.indexOf('dark') !== -1) { mode = 'dark'; }
          else if (!prefersDark && modes.indexOf('light') !== -1) { mode = 'light'; }
          else { mode = modes[0]; }
          applyTheme(theme.id, mode);
          var chips = wrap.querySelectorAll('.theme-chip');
          for (var j = 0; j < chips.length; j++) {
            chips[j].classList.toggle('active', chips[j].getAttribute('data-theme-id') === theme.id);
          }
        }
      }, [
        theme.icon
          ? el('span', {class: 'theme-chip-icon', 'data-icon': theme.icon, style: {color: palette.accentColor}})
          : null,
        el('span', {class: 'theme-chip-text'}, [
          el('span', {class: 'theme-chip-label'}, theme.label),
          theme.modes.length > 1
            ? el('span', {class: 'theme-chip-modes'}, 'dark / light')
            : el('span', {class: 'theme-chip-modes'}, theme.modes[0])
        ])
      ]);
      wrap.appendChild(chip);
    })(THEMES[i]);
  }

  return wrap;
}

function renderBandwidthForm($container, introText) {
  $container.appendChild(pageIntro(introText));
  var bwDiv = el("div");
  bwDiv.textContent = "Loading..";
  $container.appendChild(bwDiv);
  apiClient.send(function(d){
    let b = {limit:"",exceptions:[]};
    if ("bandwidth" in d) {
      b = d.bandwidth;
      if (b == null) { b = {}; }
      if (!b.limit) { b.limit = ""; }
      if (!b.exceptions) { b.exceptions = []; }
    }
    bwDiv.innerHTML = '';
    bwDiv.appendChild(formEngine.buildUI([
      {
        type: "selectinput",
        label: "Server's bandwidth limit",
        selectinput: [
          ["","Default (1 Gbit/s)"],
          [{
            label: "Custom",
            type: "number",
            min: 0,
            unit: [
              [.125,"bit/s"],
              [125,"kbit/s"],
              [125e3,"Mbit/s"],
              [125e6,"Gbit/s"]
            ]
          },"Custom"]
        ],
        pointer: {
          main: b,
          index: "limit"
        },
        help: "This is the amount of traffic this server is willing to handle."
      },{
        type: "inputlist",
        label: "Bandwidth exceptions",
        pointer: {
          main: b,
          index: "exceptions"
        },
        help: "Data sent to the hosts and subnets listed here will not count towards reported bandwidth usage.<br>Examples:<ul><li>192.168.0.0/16</li><li>localhost</li><li>10.0.0.0/8</li><li>fe80::/16</li></ul>"
      },{
        type: 'buttons',
        buttons: [{
          type: 'save',
          label: 'Save',
          'function': function(ele){
            var bandwidth = {};
            bandwidth.limit = b.limit;
            bandwidth.exceptions = b.exceptions;
            if (bandwidth.exceptions === null) {
              bandwidth.exceptions = [];
            }
            saveWithFeedback(ele,{bandwidth: bandwidth},'General');
          }
        }]
      }
    ]));
  },{bandwidth:true});
}

function generalGuided(tab, other, prev, $main) {
  $main.classList.add('page-body--flex-col', 'general-page-body', 'general-page-shell', 'slab-shell');
  $main.innerHTML = '';
  $main.appendChild(pageIntro('Essential settings for your '+APP_NAME+' instance. For session bundling, variables, location settings, external writers, and JWK configuration, switch to advanced mode.'));
  $main.appendChild(accordionTree({page: 'General', mode: 'guided', groupId: 'general-guided', sections: [
    {
      title: 'General settings',
      subtitle: 'Name, debug level, access logs',
      keywords: ['server name', 'debug level', 'access log', 'fallback stream'],
      requires: ['config'],
      render: function($inner) {
        var s = {
          serverid: mist.data.config.serverid,
          debug: mist.data.config.debug,
          accesslog: mist.data.config.accesslog,
          defaultStream: mist.data.config.defaultStream
        };
        $inner.appendChild(formEngine.buildUI([
          pageIntro('The server name is for your reference only - it doesn\'t affect networking. Debug level 3 is recommended for production; higher levels produce more log output.'),
          {
            type: 'str',
            label: 'Server name',
            pointer: { main: s, index: 'serverid' },
            help: 'A human-readable name for this '+APP_NAME+' instance.'
          },
          {
            type: 'debug',
            label: 'Debug level',
            pointer: { main: s, index: 'debug' },
            help: 'Controls how much detail '+APP_NAME+' logs. Higher levels produce more output.'
          },
          {
            type: 'selectinput',
            label: 'Access log',
            selectinput: [
              ['', 'Do not track'],
              ['LOG', 'Log to '+APP_NAME+' log'],
              [{ type: 'str', label: 'Path', LTSonly: true }, 'Log to file']
            ],
            pointer: { main: s, index: 'accesslog' },
            help: 'Enable access logs for viewer connections.',
            LTSonly: true
          },
          {
            type: 'str',
            validate: ['streamname_with_wildcard_and_variables'],
            label: 'Fallback stream',
            pointer: { main: s, index: 'defaultStream' },
            help: 'When a viewer requests a stream that does not exist or is offline, they will be redirected to this stream instead.',
            LTSonly: true
          },
          {
            type: 'buttons',
            buttons: [{
              type: 'save',
              label: 'Save',
              'function': function(ele) {
                saveWithFeedback(ele,{ config: s },'General');
              }
            }]
          }
        ]));
      }
    },
    {
      title: 'Bandwidth',
      subtitle: 'Traffic limits and reporting exceptions',
      keywords: ['bandwidth limit', 'bandwidth exceptions', 'traffic'],
      render: function($inner) {
        renderBandwidthForm($inner, "The bandwidth limit (default: 1 Gbit/s) is used for statistics and load balancing. Add exceptions for internal subnets whose traffic should not count toward reported bandwidth usage.");
      }
    },
    {
      title: 'Appearance',
      subtitle: 'Color theme for this browser',
      keywords: ['theme', 'color', 'dark', 'light'],
      render: function($inner) {
        $inner.appendChild(formEngine.buildUI([
          pageIntro('Choose a color theme. This preference is saved in your browser and does not affect the server.'),
          buildThemePicker()
        ]));
      }
    }
  ]}).$el);
}

function renderExternalWritersTable($target, d) {
  var writerKeys = (d.external_writer_list && typeof d.external_writer_list === 'object')
    ? Object.keys(d.external_writer_list)
    : [];
  if (!writerKeys.length) {
    $target.innerHTML = '';
    $target.appendChild(
      buildSectionEmptyState(
        'up',
        'No external writers configured',
        'Add one to enable custom push targets such as s3:// and other external URI protocols.'
      )
    );
    return;
  }
  var tbodyUp = el("tbody");
  var tableUp = el("table");
  var theadUp = el("thead");
  var trUp = el("tr");
  var thName = el("th");
  thName.textContent = "Name";
  var thCmd = el("th");
  thCmd.textContent = "Command line";
  var thProto = el("th");
  thProto.textContent = "URI protocols handled";
  var thEmpty = el("th");
  trUp.appendChild(thName);
  trUp.appendChild(thCmd);
  trUp.appendChild(thProto);
  trUp.appendChild(thEmpty);
  theadUp.appendChild(trUp);
  tableUp.appendChild(theadUp);
  tableUp.appendChild(tbodyUp);
  var scrollDiv = el('div', {class: 'table-scroll'});
  scrollDiv.appendChild(tableUp);
  $target.innerHTML = '';
  $target.appendChild(scrollDiv);
  for (var i of writerKeys) {
    var uploader = d.external_writer_list[i];
    var row = el("tr", {class: "uploader"});
    row.setAttribute("data-name", i);
    var tdN = el("td");
    tdN.textContent = uploader[0];
    var tdC = el("td");
    var codeEl = el("code");
    codeEl.innerHTML = uploader[1];
    tdC.appendChild(codeEl);
    var tdP = el("td", {class: "desc"});
    tdP.textContent = uploader[2] ? uploader[2].join(", ") : "none";
    var tdA = el("td");
    var editBtn = el("button");
    editBtn.textContent = "Edit";
    editBtn.addEventListener('click', function(){
      var idx = this.closest("tr").getAttribute("data-name");
      showEditExternalWriterPopup(idx);
    });
    var removeBtn = el("button");
    removeBtn.textContent = "Remove";
    removeBtn.addEventListener('click', function(){
      var idx = this.closest("tr").getAttribute("data-name");
      var name = d.external_writer_list[idx][0];
      if (confirm("Are you sure you want to remove the Uploader '"+name+"'?")) {
        apiClient.send(function(){
          tabView.showTab("General");
        },{external_writer_remove:name});
      }
    });
    tdA.appendChild(editBtn);
    tdA.appendChild(removeBtn);
    row.appendChild(tdN);
    row.appendChild(tdC);
    row.appendChild(tdP);
    row.appendChild(tdA);
    tbodyUp.appendChild(row);
  }
}

function renderJwksTable($target, d) {
  var jwkEntries = Array.isArray(d.jwks) ? d.jwks : [];
  if (!jwkEntries.length) {
    $target.innerHTML = '';
    $target.appendChild(
      buildSectionEmptyState(
        'key',
        'No JSON web keys configured',
        'Add a JWK key or JWKS URL to enable JWT-based permissions for viewing, input, and API access.'
      )
    );
    return;
  }
  var tbodyJwk = el("tbody");
  var context_menu = new uiCore.context_menu();
  for (var entryIdx = 0; entryIdx < jwkEntries.length; entryIdx++) {
    var entry = jwkEntries[entryIdx];
    var key = entry[0];
    var permissions = entry.length > 1 ? entry[1] : false;
    if (!permissions) {
      permissions = {
        input: true,
        output: true,
        admin: false,
        stream: "*"
      };
    }
    var kidEl;
    if (typeof key == "string") {
      kidEl = el("a");
      kidEl.setAttribute("href", key);
      kidEl.setAttribute("target", "_blank");
      kidEl.textContent = key;
    }
    else {
      var kid = key?.kid ? key.kid : JSON.stringify(key,null,2);
      kidEl = el("div", {class: "key clickable"});
      kidEl.textContent = kid;
      kidEl.setAttribute("title", kid);
    }
    (function(key, kidEl, entryIdx) {
      var jwkRow = el("tr");
      jwkRow.setAttribute("title", typeof key == "string" ? key : JSON.stringify(key,null,2));
      jwkRow.addEventListener("contextmenu", function(e){
        e.preventDefault();
        var headerDiv = el("div", {class: "header"});
        headerDiv.appendChild(kidEl.cloneNode(true));
        context_menu.show([[
          headerDiv
        ],[
          ["Copy "+(typeof key == "string" ? "url" : "key"),function(){
            var text = (typeof key == "string" ? key : JSON.stringify(key));
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
          },"copy","Copy this "+(typeof key == "string" ? "url" : "key")+" to the clipboard."],
          ["Edit",function(){
            var tr = e.target.closest("tr");
            var idx = Array.from(tr.parentElement.children).indexOf(tr);
            showEditJWKPopup(idx);
          },"Edit","Edit this "+(typeof key == "string" ? "url" : "key")+" or its permissions."]
        ]],e);
      });
      var tdKid = el("td");
      tdKid.appendChild(kidEl);
      jwkRow.appendChild(tdKid);
      var tdType = el("td");
      tdType.textContent = typeof key == "string" ? "url" : key.kty;
      jwkRow.appendChild(tdType);
      var tdPerm = el("td", {class: "permissions"});
      if (permissions.output) {
        var outputDiv = el("div", {class: "output"});
        outputDiv.setAttribute("title", "Viewing");
        tdPerm.appendChild(outputDiv);
      }
      if (permissions.input) {
        var inputDiv = el("div", {class: "input"});
        inputDiv.setAttribute("title", "Input");
        tdPerm.appendChild(inputDiv);
      }
      if (permissions.admin) {
        var adminDiv = el("div", {class: "admin"});
        adminDiv.setAttribute("title", "MI and API");
        tdPerm.appendChild(adminDiv);
      }
      if (!permissions.stream) permissions.stream = "*";
      var streamsDiv = el("div", {class: "streams"});
      streamsDiv.textContent = (Array.isArray(permissions.stream) ? "For streams: "+permissions.stream.join(", ") : (permissions.stream == "*" ? "For all streams" : "For streams: "+permissions.stream));
      tdPerm.appendChild(streamsDiv);
      jwkRow.appendChild(tdPerm);
      var tdActions = el("td");
      var editJwkBtn = el("button");
      editJwkBtn.setAttribute("data-icon", "Edit");
      editJwkBtn.textContent = "Edit";
      editJwkBtn.addEventListener('click', function(){
        var tr = this.closest("tr");
        var idx = Array.from(tr.parentElement.children).indexOf(tr);
        showEditJWKPopup(idx);
      });
      var deleteJwkBtn = el("button");
      deleteJwkBtn.setAttribute("data-icon", "trash");
      deleteJwkBtn.textContent = "Delete";
      deleteJwkBtn.addEventListener('click', function(){
        if (confirm("Are you sure you want to delete this key?\n"+(typeof key == "string" ? key : kidEl.textContent))) {
          apiClient.send(function(){
            navto("General");
          },{
            deletejwks: (typeof key == "string" ? key : (key.kid ? key.kid : key))
          });
        }
      });
      tdActions.appendChild(editJwkBtn);
      tdActions.appendChild(deleteJwkBtn);
      jwkRow.appendChild(tdActions);
      tbodyJwk.appendChild(jwkRow);
    })(key, kidEl, entryIdx);
  }
  var jwkTable = el("table", {class: "JWKs"});
  var jwkThead = el("thead");
  var jwkHeadRow = el("tr");
  var thKeyId = el("th");
  thKeyId.textContent = "Key id or url to key set";
  var thKeyType = el("th");
  thKeyType.textContent = "Key type";
  var thCanPermit = el("th");
  thCanPermit.textContent = "Can permit";
  var thJwkEmpty = el("th");
  jwkHeadRow.appendChild(thKeyId);
  jwkHeadRow.appendChild(thKeyType);
  jwkHeadRow.appendChild(thCanPermit);
  jwkHeadRow.appendChild(thJwkEmpty);
  jwkThead.appendChild(jwkHeadRow);
  jwkTable.appendChild(jwkThead);
  jwkTable.appendChild(tbodyJwk);
  var jwkScroll = el('div', {class: 'table-scroll'});
  jwkScroll.appendChild(jwkTable);
  $target.innerHTML = '';
  $target.appendChild(jwkScroll);
  $target.appendChild(context_menu.ele);
}

function generalAdvanced(tab, other, prev, $main, $pageHeader) {
  $main.classList.add('page-body--flex-col', 'general-page-body', 'general-advanced-shell', 'slab-shell');

  $main.innerHTML = '';
  $main.appendChild(pageIntro('All server-wide settings, organized by category. Expand each section to view and edit its options.'));
  $main.appendChild(accordionTree({page: 'General', mode: 'advanced', groupId: 'general-advanced', sections: [
    {
      title: 'General settings',
      subtitle: 'Name, debug level, access logs',
      keywords: ['server name', 'human readable name', 'debug level', 'access log', 'trusted proxies', 'fallback stream', 'prometheus'],
      requires: ['config'],
      render: function($inner) {
        var s_general = {
          serverid: mist.data.config.serverid,
          debug: mist.data.config.debug,
          accesslog: mist.data.config.accesslog,
          prometheus: mist.data.config.prometheus,
          defaultStream: mist.data.config.defaultStream,
          trustedproxy: mist.data.config.trustedproxy
        };
        $inner.appendChild(formEngine.buildUI([pageIntro("Core identity and logging settings. Set a human-readable name, control debug log verbosity (level 3 recommended for production), and optionally enable access logging. If "+APP_NAME+" is behind a reverse proxy, add its address to Trusted Proxies so viewer IPs are reported correctly - requests from localhost bypass authentication entirely."),{
          type: 'str',
          label: 'Human readable name',
          pointer: {
            main: s_general,
            index: 'serverid'
          },
          help: 'You can name your '+APP_NAME+' here for personal use. You\'ll still need to set host name within your network yourself.'
        },{
          type: 'debug',
          label: 'Debug level',
          pointer: {
            main: s_general,
            index: 'debug'
          },
          help: 'You can set the amount of debug information '+APP_NAME+' saves in the log. A full reboot of '+APP_NAME+' is required before some components of '+APP_NAME+' can post debug information.'
        },{
          type: "selectinput",
          label: "Access log",
          selectinput: [
            ["","Do not track"],
            ["LOG","Log to "+APP_NAME+" log"],
            [{
              type:"str",
              label:"Path",
              LTSonly: true
            },"Log to file"]
          ],
          pointer: {
            main: s_general,
            index: "accesslog"
          },
          help: "Enable access logs.",
          LTSonly: true
        },{
          type: "inputlist",
          label: "Trusted proxies",
          help: "List of proxy server addresses that are allowed to override the viewer IP address to arbitrary values.<br>You may use a hostname or IP address.",
          pointer: {
            main: s_general,
            index: "trustedproxy"
          }
        },{
          type: "str",
          validate: ['streamname_with_wildcard_and_variables'],
          label: 'Fallback stream',
          pointer: {
            main: s_general,
            index: "defaultStream"
          },
          help: "When this is set, if someone attempts to view a stream that does not exist, or is offline, they will be redirected to this stream instead. $stream may be used to refer to the original stream name.",
          LTSonly: true
        },{
          type: 'buttons',
          buttons: [{
            type: 'save',
            label: 'Save',
            'function': function(ele){
              saveWithFeedback(ele,{config: s_general},'General');
            }
          }]
        }]));
      }
    },
    {
      title: 'Bandwidth',
      subtitle: 'Traffic limits and reporting exceptions',
      keywords: ['bandwidth limit', 'bandwidth exceptions', 'traffic'],
      render: function($inner) {
        renderBandwidthForm($inner, "The bandwidth limit (default: 1 Gbit/s) is used for statistics and load balancing. It does not hard-cap traffic on its own. Add exceptions for internal subnets whose traffic should not count toward reported bandwidth usage.");
      }
    },
    {
      title: 'Appearance',
      subtitle: 'Color theme for this browser',
      keywords: ['theme', 'color', 'dark', 'light'],
      render: function($inner) {
        $inner.appendChild(formEngine.buildUI([
          pageIntro('Choose a color theme. This preference is saved in your browser and does not affect the server.'),
          buildThemePicker()
        ]));
      }
    },
    {
      title: 'Sessions',
      subtitle: 'Session bundling and token handling',
      keywords: ['session', 'viewer sessions', 'input sessions', 'output sessions', 'token', 'cookie', 'bundle'],
      requires: ['config'],
      render: function($inner) {
        var s_sessions = {
          sessionViewerMode: mist.data.config.sessionViewerMode,
          sessionInputMode: mist.data.config.sessionInputMode,
          sessionOutputMode: mist.data.config.sessionOutputMode,
          sessionUnspecifiedMode: mist.data.config.sessionUnspecifiedMode,
          tknMode: mist.data.config.tknMode,
          sessionStreamInfoMode: mist.data.config.sessionStreamInfoMode
        };
        $inner.appendChild(formEngine.buildUI([pageIntro('Sessions determine when a connection counts as a new viewer versus a returning one - this affects viewer counts, analytics, and when auth triggers like USER_NEW fire. Connections are grouped by up to four dimensions: stream name, IP, token, and protocol. The defaults work for most setups; only change these if you need specific counting behavior (e.g. excluding protocol switches from viewer counts). Sessions time out after 15 seconds of inactivity; denied sessions are blocked for 10 minutes.'),{
          type: 'bitmask',
          label: 'Bundle viewer sessions by',
          bitmask: [
            [8,"Stream name"],
            [4,"IP address"],
            [2,"Token"],
            [1,"Protocol"]
          ],
          pointer: {
            main: s_sessions,
            index: 'sessionViewerMode'
          },
          help: 'Change the way viewer connections are bundled into sessions.<br>Default: stream name, viewer IP and token'
        },{
          type: 'bitmask',
          label: 'Bundle input sessions by',
          bitmask: [
            [8,"Stream name"],
            [4,"IP address"],
            [2,"Token"],
            [1,"Protocol"]
          ],
          pointer: {
            main: s_sessions,
            index: 'sessionInputMode'
          },
          help: 'Change the way input connections are bundled into sessions.<br>Default: stream name, input IP, token and protocol'
        },{
          type: 'bitmask',
          label: 'Bundle output sessions by',
          bitmask: [
            [8,"Stream name"],
            [4,"IP address"],
            [2,"Token"],
            [1,"Protocol"]
          ],
          pointer: {
            main: s_sessions,
            index: 'sessionOutputMode'
          },
          help: 'Change the way output connections are bundled into sessions.<br>Default: stream name, output IP, token and protocol'
        },{
          type: 'bitmask',
          label: 'Bundle unspecified sessions by',
          bitmask: [
            [8,"Stream name"],
            [4,"IP address"],
            [2,"Token"],
            [1,"Protocol"]
          ],
          pointer: {
            main: s_sessions,
            index: 'sessionUnspecifiedMode'
          },
          help: 'Change the way unspecified connections are bundled into sessions.<br>Default: none'
        },{
          type: 'select',
          label: 'Treat HTTP-only sessions as',
          select: [
            [1, 'A viewer session'],
            [2, 'An output session: skip executing the USER_NEW and USER_END triggers'],
            [4, 'A separate \'unspecified\' session: skip executing the USER_NEW and USER_END triggers'],
            [3, 'Do not start a session: skip executing the USER_NEW and USER_END triggers and do not count for statistics']
          ],
          pointer: {
            main: s_sessions,
            index: 'sessionStreamInfoMode'
          },
          help: 'Change the way the stream info connection gets treated.<br>Default: as a viewer session'
        },{
          type: "bitmask",
          label: "Communicate session token",
          bitmask: [
            [8,"Write to cookie"],
            [4,"Write to URL parameter"],
            [2,"Read from cookie"],
            [1,"Read from URL parameter"]
          ],
          pointer: {
            main: s_sessions,
            index: "tknMode"
          },
          help: "Change the way the session token gets passed to and from "+APP_NAME+", which can be set as a cookie or URL parameter named `tkn`. Reading the session token as a URL parameter takes precedence over reading from the cookie.<br>Default: all"
        },{
          type: 'buttons',
          buttons: [{
            type: 'save',
            label: 'Save',
            'function': function(ele){
              saveWithFeedback(ele,{config: s_sessions},'General');
            }
          }]
        }]));
      }
    },
    {
      title: 'Custom variables',
      subtitle: 'Define constants and dynamic variables for substitution',
      keywords: ['variable', 'substitution', 'dynamic', 'static', 'recpath'],
      render: function($inner) {
        var newVarBtn = el("button");
        newVarBtn.setAttribute("data-icon", "plus");
        newVarBtn.textContent = "New variable";
        newVarBtn.addEventListener('click', function(){
          variables.editPopup('', {
            onSave: function() { tabView.showTab('General'); }
          });
        });
        var variablesDiv = el('div');
        variables.renderTable(variablesDiv, {
          onMutate: function() { tabView.showTab('General'); }
        });
        $inner.appendChild(formEngine.buildUI([
          pageIntro("Define reusable values that get substituted in push targets, stream sources, recording paths, and other text fields. Static variables are simple string replacements (e.g. <code>$recpath</code> &rarr; <code>/mnt/recordings</code>). Dynamic variables run a shell command or fetch a URL on a schedule - useful for values that change over time. Variables can reference other variables. Be careful: dynamic variables execute with "+APP_NAME+"'s full system permissions, and a slow command blocks other variable updates."),
          pageActionRow(newVarBtn),
          variablesDiv
        ]));
      }
    },
    {
      title: 'Load balancer',
      subtitle: 'Location settings for proximity routing',
      keywords: ['latitude', 'longitude', 'location', 'geo', 'proximity'],
      requires: ['config'],
      render: function($inner) {
        var s_balancer = {
          location: "location" in mist.data.config ? mist.data.config.location : {}
        };
        $inner.appendChild(formEngine.buildUI([
          pageIntro("Geo coordinates and location name are used by the load balancer for proximity-based routing. These settings have no effect if you're not using a load balancer."),
          {
            type: "int",
            step: 0.00000001,
            label: "Server latitude",
            pointer: {
              main: s_balancer.location,
              index: "lat"
            },
            help: "This setting is only useful when "+APP_NAME+" is combined with a load balancer. When this is set, the balancer can send users to a server close to them."
          },{
            type: "int",
            step: 0.00000001,
            label: "Server longitude",
            pointer: {
              main: s_balancer.location,
              index: "lon"
            },
            help: "This setting is only useful when "+APP_NAME+" is combined with a load balancer. When this is set, the balancer can send users to a server close to them."
          },{
            type: "str",
            label: "Server location name",
            pointer: {
              main: s_balancer.location,
              index: "name"
            },
            help: "This setting is only useful when "+APP_NAME+" is combined with a load balancer. This will be displayed as the server's location."
          },{
            type: 'buttons',
            buttons: [{
              type: 'save',
              label: 'Save',
              'function': function(ele){
                saveWithFeedback(ele,{config: s_balancer},'General');
              }
            }]
          }
        ]));
      }
    },
    {
      title: 'External writers',
      subtitle: 'Custom output handlers for unsupported targets',
      keywords: ['external writer', 's3', 'uploader', 'push target', 'URI protocol'],
      render: function($inner) {
        var newWriterBtn = el("button");
        newWriterBtn.setAttribute("data-icon", "plus");
        newWriterBtn.textContent = "New external writer";
        newWriterBtn.addEventListener('click', function(){
          showEditExternalWriterPopup("");
        });
        var uploadersDiv = el("div");
        uploadersDiv.textContent = "Loading..";
        $inner.appendChild(formEngine.buildUI([
          pageIntro("External writers handle push targets that "+APP_NAME+" doesn't natively support, like S3 cloud storage or custom file formats. When a push target matches a registered URI protocol (e.g. <code>s3://</code>), "+APP_NAME+" pipes the media data to the writer's stdin. The writer binary must be installed and accessible on the system. Add protocols here before they'll appear as push target options."),
          pageActionRow(newWriterBtn),
          uploadersDiv
        ]));
        apiClient.send(function(d){
          renderExternalWritersTable(uploadersDiv, d);
        },{ external_writer_list: true });
      }
    },
    {
      title: 'JSON web keys (JWK)',
      subtitle: 'Public keys for JWT validation',
      keywords: ['JWT', 'JWK', 'JWKS', 'JSON web key', 'token', 'authentication', 'permissions'],
      render: function($inner) {
        var addJwkBtn = el("button");
        addJwkBtn.setAttribute("data-icon", "plus");
        addJwkBtn.textContent = "Add JWK";
        addJwkBtn.addEventListener('click', function(){
          showEditJWKPopup("");
        });
        var jwksDiv = el("div");
        jwksDiv.textContent = "Loading..";
        $inner.appendChild(formEngine.buildUI([
          pageIntro("JSON Web Tokens (JWT) provide standards-based access control without needing an external auth server. Add your public keys (JWK) or a URL to a key set (JWKS) here, then configure what each key permits: viewing streams, pushing/inputting streams, or accessing the Management API. Scoping can limit keys to specific streams. This is an alternative to trigger-based auth (USER_NEW) - both can be used together."),
          pageActionRow(addJwkBtn),
          jwksDiv
        ]));
        apiClient.send(function(d){
          renderJwksTable(jwksDiv, d);
        },{ jwks: true });
      }
    }
  ]}).$el);

}

register('General', {
  guided:   generalGuided,
  advanced: generalAdvanced
});

function showEditExternalWriterPopup(other) {
  const editing = (other != '' && other != null);
  let modal;

  function buildForm() {
    const saveas = {};
    if (mist.data.external_writer_list && (other in mist.data.external_writer_list)) {
      const uploader = mist.data.external_writer_list[other];
      saveas.name = uploader[0];
      saveas.cmdline = uploader[1];
      saveas.protocols = uploader[2];
    }

    modal = uiHelpers.openFormModal({
      size: 'sm',
      title: editing ? 'Edit external writer' : 'New external writer',
      form: [
      {
        type: "str",
        label: "Human readable name",
        help: "A human readable name for the external writer.",
        validate: ['required'],
        pointer: { main: saveas, index: "name" }
      },{
        type: "str",
        label: "Command line",
        help: "Command line for a local command (with optional arguments) which will write media data to the target.",
        validate: ['required'],
        pointer: { main: saveas, index: "cmdline" }
      },{
        type: "inputlist",
        label: "URI protocols handled",
        help: "URI protocols which the external writer will be handling.",
        validate: ['required',function(val){
          for (const i in val) {
            const v = val[i];
            if (v.match(/^([a-z\d\+\-\.])+?$/) === null) {
              return {
                classes: ["red"],
                msg: "There was a problem with the protocol URI '"+function(s){ const d = document.createElement("div"); d.textContent = s; return d.innerHTML; }(v)+"':<br>A protocol URI may only contain lower case letters, digits, and the following special characters . + and -"
              }
              break;
            }
          }
        }],
        input: { type: "str", unit: "://" },
        pointer: { main: saveas, index: "protocols" }
      }
      ],
      buttons: [{
        type: 'cancel',
        label: 'Cancel'
      },{
        type: 'save',
        label: 'Save',
        'function': function(){
          const o = {external_writer_add:saveas};
          let prev_name = null;
          if ((other != "") && (other in mist.data.external_writer_list)) {
            prev_name = mist.data.external_writer_list[other][0];
          }
          if ((prev_name !== null) && (saveas.name != prev_name)) {
            o.external_writer_remove = prev_name;
          }
          apiClient.send(function(){
            modal.close();
            tabView.showTab('General');
          },o);
        }
      }]
    });
  }

  if ("external_writer_list" in mist.data) {
    buildForm();
  } else {
    apiClient.send(function(){ buildForm(); },{external_writer_list:true});
  }
}

registerTab("Edit external writer", function(tab, other) {
  tabView.showTab('General');
  showEditExternalWriterPopup(other);
});

registerTab('Edit variable', function(tab, other) {
  tabView.showTab('General');
  variables.editPopup(other, {
    onSave: function() { tabView.showTab('General'); }
  });
});

function showEditJWKPopup(other) {
  let editing = false;
  if (other != '' && other != null) {
    editing = true;
  }

  let modal;

  function buildForm(d){
    if (editing && d && d.jwks) {
      editing = d.jwks?.[Number(other)];
      if (editing && ((editing.length < 2) || !editing[1])) {
        editing[1] = {
          input: true,
          output: true,
          admin: false,
          stream: "*"
        };
      }
    }

    let saveas = {
      urls: [],
      keys: [],
      permissions: {}
    };
    if (editing) {
      if (typeof editing[0] == "string") {
        saveas.urls = [editing[0]];
      }
      else {
        saveas.keys = [JSON.stringify(editing[0],null,2)];
      }
      saveas.permissions = editing[1];
    }

    const h3Keys = el("h3");
    h3Keys.textContent = "Keys";

    const uploadJwksBtn = el("button");
    uploadJwksBtn.setAttribute("data-icon", "up");
    uploadJwksBtn.textContent = "Upload JWKS file";
    uploadJwksBtn.style.fontSize = "1.25em";
    uploadJwksBtn.style.marginLeft = "0";
    uploadJwksBtn.addEventListener('click', function(){
      let me = this;
      let fieldContainer = this.closest(".input_container");
      let field = fieldContainer ? fieldContainer.querySelector('.field[name="keys"]') : null;
      let ihc = me.parentElement._help_container;
      let uid = me.parentElement._uid;
      if (ihc) {
        const errBalloon = ihc.querySelector(".err_balloon[data-uid='"+uid+"']");
        if (errBalloon) errBalloon.remove();
      }

      function showErr(err) {
        console.warn(err);
        let msg = "Upload failed";
        if (err.name == "AbortError") { msg = "Upload aborted"; }
        me.setAttribute("data-icon", "cross");
        me.textContent = msg;
        if (ihc) {
          let errEl = el('span', {class: 'err_balloon orange'});
          errEl.setAttribute("data-uid", uid);
          errEl.innerHTML = err;
          ihc.insertBefore(errEl, ihc.firstChild);
        }
        setTimeout(function(){ me.setAttribute("data-icon", "up"); me.textContent = "Upload JWKS file"; },5e3);
      }

      upload({
        description: "JWKS files",
        accept: { "*/json": [".jwk",".jwks",".json"] }
      }).then(function(result){
        result.text().then(function(text){
          try {
            let json = JSON.parse(text);
            let out = getval(field);
            let foundkeys = false;
            function add(json) {
              if (("kty" in json) && ("alg" in json)) {
                out.push(JSON.stringify(json,null,2));
                foundkeys = true;
              }
            }
            if ("keys" in json) { for (let i in json.keys) { add(json.keys[i]); } }
            else if (Array.isArray(json)) { for (let i in json) { add(json[i]); } }
            else { add(json); }
            if (!foundkeys) throw "Could not find any keys in this file ("+result.name+")";
            setval(field, out);
          }
          catch(e) { showErr(e); }
        });
      }).catch(showErr);
    });

    const h3Grant = el("h3");
    h3Grant.textContent = "Grant permissions";

    modal = uiHelpers.openFormModal({
      size: 'md',
      title: editing ? (typeof editing[0] == "string" ? "Edit url to JWKS" : "Edit JSON web key") : "Add JSON web key(s)",
      form: [{
      type: "help",
      help: "You can use JSON web tokens (JWT) to control permissions for viewing certain streams, inputting a stream or even access to this Management Interface.<br>Here, you can add your public keys (JWK) that will be used to validate the JWT's. You can also supply an url from which "+APP_NAME+" can download a key set (JWKS)."
    },h3Keys,!editing || (typeof editing[0] == "string") ? {
      label: "Url(s) to JSON web key set (JWKS)",
      help: "Enter one or more urls where "+APP_NAME+" can download a JSON web key set.",
      pointer: { main: saveas, index: "urls" },
      type: "inputlist",
      validate: editing ? ["required"] : [],
      input: {
        type: "str",
        validate: [function(val){
          if (val == "") return;
          function isValidHttpUrl(string) {
            let url;
            try { url = new URL(string); } catch (e) { return false; }
            return url.protocol === "http:" || url.protocol === "https:";
          }
          if (!isValidHttpUrl(val)) {
            return { msg: "Please enter a valid url.", classes: ["red"] };
          }
        }]
      }
    } : false,!editing ? {
      type: "help",
      help: "- and / or -",
      css: { margin: "1em 0" }
    } : false,!editing || (typeof editing[0] != "string") ? {
      type: "span",
      label: "JSON web key (set)",
      value: uploadJwksBtn,
      help: "Upload a JWKS file from your computer.",
    } : false, !editing || (typeof editing[0] != "string") ? {
      type: "inputlist",
      pointer: { main: saveas, index: "keys" },
      help: "Enter a JWK or a JWKS: a json object or array of objects.",
      validate: editing ? ["required"] : [],
      input: {
        type: "textarea",
        placeholder: JSON.stringify({alg:"youralg",kid:"youruuid",kty:"oct",k:42},null,2),
        rows: 6,
        validate: [function (val,me){
          let listitem = me.closest(".listitem");
          let fieldn = 1 + Array.from(listitem.parentElement.children).indexOf(listitem);
          if (val == "") { return; }
          let json;
          try { json = JSON.parse(val); } catch (e) {}
          if (!json) { return { msg: "Field "+fieldn+": Invalid json.", classes: ["red"] }; }
          if ((json === null) || (typeof json != "object")) {
            return { msg: "Field "+fieldn+": Please enter a JSON object or array of objects.", classes: ["red"] };
          }
          if (Array.isArray(json)) {
            for (let key of json) {
              if (!("kty" in key)) {
                return { msg: "Field "+fieldn+": All keys should contain the 'kty' index.", classes: ["red"] };
              }
            }
          }
          else if (!("kty" in json)) {
            return { msg: "Field "+fieldn+": All keys should contain the 'kty' index.", classes: ["red"] };
          }
        }]
      }
    } : false,h3Grant,{
      label: "Use keys to permit viewing",
      type: "checkbox",
      pointer: { main: saveas.permissions, index: "output" },
      help: "When a valid JWT - signed with one of these keys - is provided, access should be granted to view the stream.",
      value: true
    },{
      label: "Use keys to permit stream input",
      type: "checkbox",
      pointer: { main: saveas.permissions, index: "input" },
      help: "When a valid JWT - signed with one of these keys - is provided, access should be granted to input the stream.",
      value: true
    },{
      label: "Apply to",
      type: "selectinput",
      pointer: { main: saveas.permissions, index: "stream" },
      help: "The keys listed above should only grant access to the streams configured here.<br>You may use STREAMNAME+* to include all wildcard children.",
      selectinput: [
        ["*","All streams"],
        [{ type: "inputlist" },"These stream names .."]
      ]
    },{
      label: "API authentication",
      type: "checkbox",
      pointer: { main: saveas.permissions, index: "admin" },
      help: "When a valid JWT - signed with one of these keys - is provided, access should be granted to the "+APP_NAME+" Management Interface and API."
    }],
      buttons: [{
        type: "save",
        label: editing ? "Save" : "Add",
        icon: editing ? "check" : "plus",
        "function": function(me){
          for (let i in saveas.keys) {
            saveas.keys[i] = JSON.parse(saveas.keys[i]);
          }
          let concatenated = saveas.urls.concat(saveas.keys);

          if (concatenated.length == 0) {
            let container = me.closest(".input_container");
            let fields = container.querySelectorAll(".field[name]");
            Array.from(fields).forEach(function(fieldEl){
              let fs = fieldEl._validate_functions;
              fs.push(function(val,me){
                if (val.length == 0) {
                  return { msg: "Please enter something in one of these fields.", classes: ["orange"] };
                }
              });
              fieldEl._validate_functions = fs;
              fieldEl._validate(fieldEl,true);
              fs.pop();
              fieldEl._validate_functions = fs
            });
            return;
          }

          let command = { addjwks: [] }
          for (let i in concatenated) {
            command.addjwks.push([concatenated[i],saveas.permissions]);
          }

          if (editing) {
            command.deletejwks = (typeof editing[0] == "string" ? editing[0] : (editing[0].kid ? editing[0].kid : editing[0]));
          }

          apiClient.send(function(d){
            modal.close();
            tabView.showTab("General");
          },command);
        }
      },{
        type: "cancel",
        label: "Cancel"
      }]
    });
  }

  if (editing) {
    apiClient.send(buildForm,{jwks:true});
  } else {
    buildForm();
  }
}

registerTab('Edit JWK', function(tab, other) {
  tabView.showTab('General');
  showEditJWKPopup(other);
});

registerTab('Stream keys', function(tab, other, prev, $main, $pageHeader) {
  $main.classList.add('page-body--flex-col', 'streamkeys-page-shell', 'slab-shell');
  const returnBtn = el("button");
  returnBtn.setAttribute("data-icon", "return");
  returnBtn.classList.add('streamkeys-return-btn');
  returnBtn.textContent = "Back to Streams";
  returnBtn.addEventListener('click', function(){
    navto("Streams");
  });
  appendPageActions($pageHeader, returnBtn);
  $main.appendChild(streamkeys(undefined,undefined,{pageMode: true}));

});
