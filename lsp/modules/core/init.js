import { APP_NAME, APP_TITLE } from '@brand';
import { AppShell } from './appshell.js';
import { apiClient } from './api_client.js';
import { tabView } from './tab_view.js';
import { uiCore } from './ui_core.js';
import { initSearchUI } from './search_ui.js';

let lastpage = [];

document.addEventListener('DOMContentLoaded', function(){
  document.title = APP_TITLE;
  document.querySelector('.logo').textContent = APP_NAME;

  UI.elements = {
    menu: document.querySelector('nav > .menu'),
    main: document.querySelector('main'),
    header: document.querySelector('header'),
    connection: {
      status: document.getElementById('connection'),
      user_and_host: document.getElementById('user_and_host'),
      msg: document.getElementById('message')
    },
    context_menu: []
  };
  uiCore.buildMenu();
  UI.stored.getOpts();
  AppShell.init();
  initSearchUI();

  UI.closeHelpBalloons = function(){
    document.querySelectorAll('.ih_balloon:popover-open').forEach(function(el){
      el.hidePopover();
    });
  };

  document.body.setAttribute("data-browser",function(){
    const ua = window.navigator.userAgent;

    if ((ua.indexOf("MSIE ") >= 0) || (ua.indexOf("Trident/") >= 0)) {
      return "ie";
    }
    if (ua.indexOf("Edge/") >= 0) {
      return "edge";
    }
    if ((ua.indexOf("Opera") >= 0) || (ua.indexOf('OPR') >= 0)) {
      return "opera";
    }
    if (ua.indexOf("Chrome") >= 0) {
      return "chrome";
    }
    if (ua.indexOf("Safari") >= 0) {
      return "safari";
    }
    if (ua.indexOf("Firefox") >= 0) {
      return "firefox";
    }
    return false;
  }());

  document.body.addEventListener("keydown",function(e){
    switch (e.key) {
      case "Escape": {
        for (let menu of UI.elements.context_menu) {
          menu.hide();
        }
        UI.closeHelpBalloons();
        break;
      }
    }
  });

  UI.elements.main.addEventListener("click",function(e){
    if (!e.defaultPrevented) {
      for (let menu of UI.elements.context_menu) {
        menu.hide();
      }
    }
  });
  const long_click = { timeout: false, delay: 1500 };
  UI.elements.main.addEventListener("mousedown",function(e){
    const ele = e.target;
    long_click.timeout = setTimeout(function(){
      long_click.timeout = false;
      const event = new Event("contextmenu",{bubbles:true});
      event.pageX = e.pageX;
      event.pageY = e.pageY;
      ele.dispatchEvent(event);

      function captureClick(e) {
        e.preventDefault();
      }
      function cleanUp() {
        window.removeEventListener('click',captureClick,true);
        document.removeEventListener('mouseup',cleanUp);
      }
      window.addEventListener('click',captureClick,true);
      document.addEventListener('mouseup',function(e){
        requestAnimationFrame(cleanUp);
      });
    },long_click.delay);
  });
  UI.elements.main.addEventListener("mouseleave",function(e){
    if (long_click.timeout) {
      clearTimeout(long_click.timeout);
      long_click.timeout = false;
    }
  });
  UI.elements.main.addEventListener("mouseup",function(e){
    if (long_click.timeout) {
      clearTimeout(long_click.timeout);
      long_click.timeout = false;
    }
  });

  try {
    if ('mistLogin' in sessionStorage) {
      const stored = JSON.parse(sessionStorage['mistLogin']);
      mist.user.name = stored.name;
      mist.user.password = stored.password;
      mist.user.host = stored.host;
    }
  }
  catch (e) {}

  if (location.hash) {
    const hash = decodeURIComponent(location.hash).substring(1).split('@');
    const user = hash[0].split('&');
    mist.user.name = user[0];
    if (user[1]) { mist.user.host = user[1]; }
  }

  apiClient.send(function(d){
    window.dispatchEvent(new Event('hashchange'));
  },{},{timeout: 5, hide: true});

});

window.addEventListener('hashchange', function(e) {
  const loc = decodeURIComponent(location.hash).substring(1).split('@');
  if (!loc[1]) { loc[1] = ''; }
  const tab = loc[1].split('&');
  if (tab[0] == '') { tab[0] = 'Overview'; }
  tabView.showTab(tab[0],tab[1],lastpage);
  if (lastpage[0] != tab[0] || lastpage[1] != tab[1]) lastpage = [tab[0],tab[1]];
});
