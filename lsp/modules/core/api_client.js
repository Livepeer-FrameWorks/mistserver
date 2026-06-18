import { APP_NAME } from '@brand';
import * as format from './formatters.js';
import { navto } from './navigation.js';
import { el, deepExtend } from './dom_helpers.js';
import { log } from './app_state.js';
import { MD5 } from '../../plugins/md5.js';

export const apiClient = {
send: function(callback,sendData,opts){
  sendData = sendData || {};
  opts = opts || {};
  opts = deepExtend({
    timeOut: 30e3,
    sendData: sendData
  },opts);
  const data = {
    authorize: {
      password: (mist.user.authstring ? MD5(mist.user.password+mist.user.authstring) : ''),
      username: mist.user.name
    }
  };
  deepExtend(data,sendData);
  log('Send',deepExtend({},sendData));

  const controller = new AbortController();
  const timeoutMs = (opts.timeout || 30) * 1000;
  const timeoutId = setTimeout(function(){ controller.abort(); }, timeoutMs);

  if (!opts.hide) {
    const cancelLink = el('a', null, 'Cancel request');
    cancelLink.addEventListener('click', function(){
      controller.abort();
    });
    const msgEl = UI.elements.connection.msg;
    msgEl.classList.remove('red');
    msgEl.textContent = 'Data sent, waiting for a reply..';
    msgEl.appendChild(el('br'));
    msgEl.appendChild(cancelLink);
  }

  fetch(mist.user.host, {
    method: 'POST',
    headers: {'Content-Type': 'application/json'},
    body: JSON.stringify(data),
    signal: controller.signal
  }).then(function(response) {
    clearTimeout(timeoutId);
    if (!response.ok) throw new Error(response.statusText || ('HTTP ' + response.status));
    return response.json();
  }).then(function(d) {
    // success handler
    log('Receive',deepExtend({},d),'as reply to',opts.sendData);
    delete mist.user.loggedin;
    switch (d.authorize.status) {
      case 'OK':
        {if ('streams' in d) {
          if (d.streams) {
            if ('incomplete list' in d.streams) {
              delete d.streams['incomplete list'];
              Object.assign(mist.data.streams,d.streams);
            }
            else {
              mist.data.streams = d.streams;
            }
          }
          else {
            mist.data.streams = {};
          }
        }

        if ('camera_list' in d) {
          mist.data.camera_list = d.camera_list || [];
        }

        const save = Object.assign({},d);
        const keep = ['config','capabilities','ui_settings','LTS','active_streams','browse','log','totals','bandwidth','variable_list','external_writer_list','streamkeys','camera_list','clients'];
        for (const i in save) {
          if (keep.indexOf(i) == -1) {
            delete save[i];
          }
        }
        if (("bandwidth" in data) && (!("bandwidth" in d))) {
          save.bandwidth = null;
        }
        if (opts.sendData.capabilities && (opts.sendData.capabilities !== true)) {
          delete save.capabilities;
        }

        // Server replies may contain nullable fields while still being valid.
        // Normalize these to stable collection/object types expected by the UI.
        if ('active_streams' in save && !Array.isArray(save.active_streams)) {
          save.active_streams = [];
        }
        if ('camera_list' in save && !Array.isArray(save.camera_list)) {
          save.camera_list = [];
        }
        if ('capabilities' in save && (!save.capabilities || (typeof save.capabilities !== 'object'))) {
          save.capabilities = {};
        }
        if ('config' in save && (!save.config || (typeof save.config !== 'object'))) {
          save.config = {};
        }

        Object.assign(mist.data,save);

        mist.user.loggedin = true;
        UI.elements.connection.status.textContent = 'Connected';
        UI.elements.connection.status.classList.remove('red');
        UI.elements.connection.status.classList.add('green');
        UI.elements.connection.user_and_host.textContent = mist.user.name+' @ '+mist.user.host;
        const msgEl = UI.elements.connection.msg;
        msgEl.classList.remove('red');
        msgEl.innerHTML = '';
        msgEl.appendChild(
          el("span", {"class": "status-last-seen"},
            'Updated '+format.time((new Date).getTime()/1000)
          )
        );

        msgEl.appendChild(
          el("span", {"class": "status-recent-label"}, 'Recent logs')
        );

        const logsContainer = el("div", {"class": "status-log-list"});
        if (d.log && d.log.length) {
          const recentlogs = d.log.slice(Math.max(0,d.log.length-5));
          for (let logn = 0; logn < recentlogs.length; logn++) {
            const logline = recentlogs[logn];
            const logtime = format.time(logline[0]);
            const loglevel = (logline[1] || 'INFO')+'';
            const loglevelclass = loglevel.toLowerCase().replace(/[^\w-]+/g,'-');
            const logmsg = ((logline[2] == null) ? '' : (logline[2]+''));

            logsContainer.appendChild(
              el("div", {"class": "status-log-line"}, [
                el("span", {"class": "status-log-time"}, logtime),
                el("span", {"class": "status-log-level status-log-level-"+loglevelclass}, '['+loglevel+']'),
                el("span", {"class": "status-log-msg"}, logmsg)
              ])
            );
          }
        }
        else {
          logsContainer.appendChild(
            el("div", {"class": "status-log-line status-log-line-empty"}, [
              el("span", {"class": "status-log-msg"}, 'No recent logs.')
            ])
          );
        }
        msgEl.appendChild(logsContainer);
        if ('totals' in d) {

          function reformat(main) {
            function insertZero(overridetime) {
              if (typeof overridetime == 'undefined') {
                overridetime = time;
              }

              for (const j in main.fields) {
                obj[main.fields[j]].push([time,0]);
              }
            }

            const obj = {};
            for (const i in main.fields) {
              obj[main.fields[i]] = [];
            }
            let insert = 0;
            let time;

            if (!main.data) {
              time = (mist.data.config.time - 600)*1e3;
              insertZero();
              time = (mist.data.config.time - 15)*1e3;
              insertZero();
            }
            else {
              if (main.start > (mist.data.config.time - 600)) {
                time = (mist.data.config.time - 600)*1e3;
                insertZero();
                time = main.start*1e3;
                insertZero();
              }
              else {
                time = main.start*1e3;
              }

              time = main.start*1e3;
              let interval_n = 0;
              for (const i in main.data) {
                if (i != 0) {
                  time += main.interval[interval_n][1]*1e3;
                  main.interval[interval_n][0]--;
                  if (main.interval[interval_n][0] <= 0) {
                    interval_n++;
                    if (interval_n < main.interval.length-1) { insert += 2; }
                  }
                }

                if (insert % 2 == 1) {
                  insertZero();
                  insert--;
                }

                for (const j in main.data[i]) {
                  obj[main.fields[j]].push([time,main.data[i][j]]);
                }

                if (insert) {
                  insertZero();
                  insert--;
                }
              }

              if ((mist.data.config.time - main.end) > 20) {
                insertZero();
                time = (mist.data.config.time -15) * 1e3;
                insertZero();
              }
            }
            return obj;
          }
          function savereadable(streams,protocols,data){
            const obj = reformat(data);
            const stream = (streams ? streams.join(' ') : 'all_streams');
            const protocol = (protocols ? protocols.join('_') : 'all_protocols');

            if (!(stream in mist.data.totals)) {
              mist.data.totals[stream] = {};
            }
            if (!(protocol in mist.data.totals[stream])) {
              mist.data.totals[stream][protocol] = {};
            }
            Object.assign(mist.data.totals[stream][protocol],obj);
          }

          mist.data.totals = {};
          if (d.totals && (typeof d.totals === 'object')) {
            if ('fields' in d.totals) {
              savereadable(sendData.totals.streams,sendData.totals.protocols,d.totals);
            }
            else {
              for (const i in d.totals) {
                if (!sendData.totals || !sendData.totals[i]) { continue; }
                savereadable(sendData.totals[i].streams,sendData.totals[i].protocols,d.totals[i]);
              }
            }
          }
        }

        if (callback) { callback(d,opts); }
        break;
      }case 'CHALL':
        if (d.authorize.challenge == mist.user.authstring) {
          if (mist.user.password != '') {
            UI.elements.connection.msg.textContent = 'The credentials you provided are incorrect.';
          UI.elements.connection.msg.classList.add('red');
          }
          navto('Login');
        }
        else if (mist.user.password == '') {
          navto('Login');
        }
        else{
          mist.user.authstring = d.authorize.challenge;
          apiClient.send(callback,sendData,opts);

          const store = {
            host: mist.user.host,
            name: mist.user.name,
            password: mist.user.password
          };
          sessionStorage.setItem('mistLogin',JSON.stringify(store));
        }
        break;
      case 'NOACC':
        navto('Create a new account');
        break;
      case 'ACC_MADE':
        delete sendData.authorize;
        apiClient.send(callback,sendData,opts);
        break;
      default:
        navto('Login');
    }
  }).catch(function(err) {
    clearTimeout(timeoutId);
    console.warn("connection failed :(", err);

    delete mist.user.loggedin;
    if (typeof opts.onError == 'function') {
      try { opts.onError(err, opts); } catch (e) {}
    }

    if (!opts.hide) {
      let statusEl;
      if (err.name === 'AbortError') {
        statusEl = el('i', null, 'The connection timed out or was aborted. ');
      } else {
        statusEl = el('i', {style: {textTransform: 'capitalize'}}, (err.message || 'Connection error') + '. ');
      }
      const retryLink = el('a', null, 'Send server request again');
      retryLink.addEventListener('click', function(){
        apiClient.send(callback,sendData,opts);
      });
      const msgEl = UI.elements.connection.msg;
      msgEl.classList.add('red');
      msgEl.textContent = 'An error occurred while attempting to communicate with '+APP_NAME+':';
      msgEl.appendChild(el('br'));
      msgEl.appendChild(el("span", null, [statusEl]));
      msgEl.appendChild(retryLink);
    }

    if (!opts.noLoginOnError) {
      navto('Login');
    }
  });
},

parseURL: function(url,set) {
let a;
if ("URL" in window && (typeof window.URL == "function")) {
  try {
    a = new URL(url);
  }
  catch (e) {
    a = document.createElement('a');
    a.href = url;
  }
}
else {
  a = document.createElement('a');
  a.href = url;
}
if (set) {
  for (const i in set) {
    a[i] = set[i];
  }
}
return {
  full: a.href,
  protocol: a.protocol+'//',
  host: a.hostname,
  port: (a.port ? ':'+a.port : ''),
  pathname: a.pathname ? a.pathname : null,
  search: a.search ? a.search.replace(/^\?/,"") : null,
  searchParams : a.search ? a.searchParams : null
};
}
};
