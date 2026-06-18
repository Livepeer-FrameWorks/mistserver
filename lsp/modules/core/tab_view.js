import { APP_TITLE } from '@brand';
import { getTabHandler } from './tab_registry.js';
import { uiHelpers } from './ui_helpers.js';
import { logs } from '../streams/stream_utils.js';
import { apiClient } from './api_client.js';
import { formEngine } from './form_engine.js';
import { navto } from './navigation.js';
import { sockets } from './sockets.js';
import { el, getval } from './dom_helpers.js';
import { getHintsFooter, updateHintsFooter } from '../components/hints_footer.js';

export const tabView = {
  showTab: function(tab,other,prev) {
    document.querySelectorAll('dialog.popup[open]').forEach(function(d){ d.close(); });
    if (typeof prev == "undefined") { prev = []; }

    if (mist.user.loggedin) {
      if (!('ui_settings' in mist.data)) {
        UI.elements.main.textContent = 'Loading..';
        apiClient.send(function(){
          tabView.showTab(tab,other);
        },{ui_settings: true});
        return;
      }

      if (mist.data.config.serverid) {
        document.title = mist.data.config.serverid+" - "+APP_TITLE;
      }
    }

    document.body.classList.remove('login-mode');
    UI.elements.menu.classList.remove('hide');
    let currbut = null;
    const plains = Array.from(UI.elements.menu.querySelectorAll('.plain'));
    for (let pi = 0; pi < plains.length; pi++) {
      if (plains[pi].textContent === tab) {
        currbut = plains[pi].closest('.button');
        break;
      }
    }
    if (currbut) {
      const activeButtons = UI.elements.menu.querySelectorAll('.button.active');
      for (let ai = 0; ai < activeButtons.length; ai++) {
        activeButtons[ai].classList.remove('active');
      }
      currbut.classList.add('active');
      const submenu = currbut.closest('[data-param]');
      if (submenu) {
        submenu.setAttribute('data-param', other);
      }
    }

    if ((window.mv) && (window.mv.reference)) {
      window.mv.reference.unload();
    }

    UI.interval.clear();
    UI.websockets.clear();
    if (sockets && sockets.http) {
      if (sockets.http.api && sockets.http.api.clear) {
        sockets.http.api.clear();
      }
      if (sockets.http.clients && sockets.http.clients.clear) {
        sockets.http.clients.clear();
      }
    }
    UI.elements.contextmenu = [];

    let $pageHeader;
    let $pageBody;
    let $pageFooter;
    const shell = uiHelpers.createPageShell({
      title: tab,
      subtitle: UI.pageSubtitles[tab] || false
    });
    $pageHeader = shell.header;
    $pageBody = shell.body;
    $pageFooter = shell.footer;
    UI.elements.main.setAttribute('data-tab', tab);
    UI.elements.main.textContent = '';
    UI.elements.main.appendChild(shell.element);

    switch (tab) {
      case 'Disconnect':
        mist.user.password = '';
        delete mist.user.authstring;
        delete mist.user.loggedin;
        mist.data = {};
        sockets.http_host = null;
        sessionStorage.removeItem('mistLogin');
        navto('Login');
        break;
      case 'Logs': {
        let $logs = logs(null, {showRawInTools: false});
        $pageHeader.classList.add('logs-page-header');
        $pageBody.classList.add('logs-page-shell', 'slab-shell');
        if ($logs.rawButton) {
          $logs.rawButton.classList.add('save');
          $logs.rawButton.setAttribute('data-icon', 'external-link');
          uiHelpers.appendPageActions($pageHeader, $logs.rawButton);
        }
        $logs.classList.add('logs-page-slab');
        const intro = formEngine.buildUI([{type:'help',classes:['page-intro'],help:'Server log output. Use the actions to inspect or export log contents.'}]);
        if (intro && intro.classList) {
          intro.classList.add('logs-page-intro');
        }
        $pageBody.appendChild(intro);
        $pageBody.appendChild($logs);
        break;
      }
      default:
        if (getTabHandler(tab)) {
          getTabHandler(tab)(tab, other, prev, $pageBody, $pageHeader, $pageFooter);
        } else {
          $pageBody.append(el('p', null, 'This tab does not exist.'));
        }
        break;
    }

    if ($pageFooter && ($pageFooter.childNodes.length || $pageFooter.textContent.trim().length)) {
      $pageFooter.hidden = false;
    }

    if (tab !== 'Login' && tab !== 'Disconnect') {
      var footer = getHintsFooter();
      UI.elements.main.appendChild(footer);
      updateHintsFooter(tab);
    }

    const fields = UI.elements.main.querySelectorAll('.field');
    for (let i = 0; i < fields.length; i++) {
      const field = fields[i];
      const val = getval(field);
      if (val === '' || val === null) {
        let focusTarget = null;
        if (field.matches('input, select, textarea')) {
          focusTarget = field;
        } else {
          const inp = field.querySelector('input, select, textarea');
          if (inp) {
            focusTarget = inp;
          }
        }
        if (focusTarget) {
          if (!window.matchMedia('(max-width: 767px)').matches) {
            focusTarget.focus();
          }
          break;
        }
      }
    }
  }
};
