import { MistIcons } from './icons.js';
import { modeAwareTabs } from './mode_dispatch.js';
import { tabView } from './tab_view.js';
import { sockets } from './sockets.js';
import { navto } from './navigation.js';
import { q } from './dom_helpers.js';
import { getActiveThemeId, getActiveMode, getAvailableModes, applyTheme, toggleMode } from './themes.js';

const AppShell = {};

AppShell.normalizeMode = function(mode) {
  if (mode === 'simple') return 'guided';
  if ((mode !== 'guided') && (mode !== 'advanced')) return 'guided';
  return mode;
};

AppShell.getMode = function() {
  return AppShell.normalizeMode(document.body.getAttribute('data-mode'));
};

AppShell.isGuided = function() {
  return AppShell.getMode() === 'guided';
};

AppShell.getModeStorageKey = function() {
  let user = '';
  let host = '';

  if (window.mist && mist.user) {
    user = mist.user.name || '';
    host = mist.user.host || '';
  }

  if (!user || !host) {
    try {
      const loc = decodeURIComponent(location.hash || '').replace(/^#/, '').split('@');
      const userHost = (loc[0] || '').split('&');
      if (!user && userHost[0]) user = userHost[0];
      if (!host && userHost[1]) host = userHost[1];
    } catch (e) {}
  }

  if (!user || !host) {
    try {
      const login = JSON.parse(sessionStorage.getItem('mistLogin') || '{}');
      if (!user && login.name) user = login.name;
      if (!host && login.host) host = login.host;
    } catch (e) {}
  }

  if (!user && !host) return 'mist-mode';
  return 'mist-mode:' + encodeURIComponent(user + '@' + host);
};

AppShell.modeAffectsTab = function(tabName) {
  return !!modeAwareTabs[tabName];
};

AppShell.getModeHint = function() {
  const isAdvanced = (AppShell.getMode() === 'advanced');
  if (isAdvanced) {
    return 'Advanced mode is active. Switch to Guided for a simplified UI.';
  }
  return 'Guided mode is active. Switch to Advanced for full controls.';
};

AppShell.showModeHint = function(evt) {
  if (!window.UI || !UI.tooltip || (typeof UI.tooltip.show !== 'function')) return;

  let pageX = evt && evt.pageX;
  let pageY = evt && evt.pageY;
  if ((typeof pageX !== 'number') || (typeof pageY !== 'number')) {
    const btn = q('#mode-toggle');
    if (!btn) return;
    const rect = btn.getBoundingClientRect();
    pageX = rect.left + (rect.width / 2) + (window.scrollX || window.pageXOffset || 0);
    pageY = rect.top + rect.height + (window.scrollY || window.pageYOffset || 0);
  }

  UI.tooltip.show({ pageX: pageX, pageY: pageY }, AppShell.getModeHint());
};

AppShell.hideModeHint = function() {
  if (!window.UI || !UI.tooltip || (typeof UI.tooltip.hide !== 'function')) return;
  UI.tooltip.hide();
};

AppShell.init = function() {
  const sidebar = q('#sidebar');
  const overlay = q('#sidebar-overlay');

  q('#sidebar-toggle').addEventListener('click', function() {
    sidebar.classList.add('open');
    overlay.classList.add('visible');
  });

  const closeSidebarHandler = function() {
    sidebar.classList.remove('open');
    overlay.classList.remove('visible');
  };

  q('#sidebar-close').addEventListener('click', closeSidebarHandler);
  overlay.addEventListener('click', closeSidebarHandler);

  sidebar.addEventListener('click', function(e) {
    if (e.target.closest('.menu .button') && window.innerWidth <= 1024) {
      sidebar.classList.remove('open');
      overlay.classList.remove('visible');
    }
  });

  window.addEventListener('hashchange', function() {
    if (window.innerWidth <= 1024) {
      AppShell.closeSidebar();
    }
  });

  applyTheme(getActiveThemeId(), getActiveMode());
  AppShell.updateThemeLabel(getActiveMode());
  AppShell.updateThemeToggleVisibility();

  q('#theme-toggle').addEventListener('click', function() {
    toggleMode();
    AppShell.updateThemeLabel(getActiveMode());
  });

  document.addEventListener('themechange', function(e) {
    AppShell.updateThemeLabel(e.detail.mode);
    AppShell.updateThemeToggleVisibility();
  });

  const modeKey = AppShell.getModeStorageKey();
  let savedMode = localStorage.getItem(modeKey);
  if ((savedMode === null) && (modeKey !== 'mist-mode')) {
    savedMode = localStorage.getItem('mist-mode');
  }
  savedMode = AppShell.normalizeMode(savedMode || document.body.getAttribute('data-mode') || 'guided');
  document.body.setAttribute('data-mode', savedMode);
  localStorage.setItem(modeKey, savedMode);
  AppShell.updateModeLabel(savedMode);

  q('#mode-toggle').addEventListener('click', function() {
    const m = AppShell.getMode() === 'advanced' ? 'guided' : 'advanced';
    document.body.setAttribute('data-mode', m);
    localStorage.setItem(AppShell.getModeStorageKey(), m);
    AppShell.updateModeLabel(m);
    AppShell.hideModeHint();

    const loc = decodeURIComponent(location.hash).substring(1).split('@');
    if (loc[1]) {
      const parts = loc[1].split('&');
      if (parts[0] && AppShell.modeAffectsTab(parts[0])) {
        tabView.showTab(parts[0], parts[1] || '', []);
      }
    }
  });
  q('#mode-toggle').addEventListener('mouseenter', AppShell.showModeHint);
  q('#mode-toggle').addEventListener('mousemove', AppShell.showModeHint);
  q('#mode-toggle').addEventListener('focus', AppShell.showModeHint);
  q('#mode-toggle').addEventListener('mouseleave', AppShell.hideModeHint);
  q('#mode-toggle').addEventListener('blur', AppShell.hideModeHint);

  const statusDot = q('#status-dot');
  const statusDetails = statusDot.parentElement.querySelector('.status-details');
  statusDot.setAttribute('aria-expanded', 'false');

  statusDot.addEventListener('click', function(e) {
    e.stopPropagation();
    statusDetails.classList.toggle('open');
    statusDot.setAttribute('aria-expanded', statusDetails.classList.contains('open') ? 'true' : 'false');
  });

  document.addEventListener('click', function() {
    statusDetails.classList.remove('open');
    statusDot.setAttribute('aria-expanded', 'false');
  });

  statusDetails.addEventListener('click', function(e) {
    e.stopPropagation();
  });

  q('#header-disconnect').addEventListener('click', function(e) {
    e.stopPropagation();
    mist.user.password = '';
    delete mist.user.authstring;
    delete mist.user.loggedin;
    mist.data = {};
    sockets.http_host = null;
    sessionStorage.removeItem('mistLogin');
    navto('Login');
  });

  MistIcons.init();

  const connEl = document.getElementById('connection');
  if (connEl) {
    new MutationObserver(function() {
      statusDot.classList.toggle('green', connEl.classList.contains('green'));
    }).observe(connEl, { attributes: true, attributeFilter: ['class'] });
  }
};

AppShell.updateThemeLabel = function(t) {
  const btn = q('#theme-toggle');
  const isDark = (t === 'dark');
  btn.setAttribute('title', (isDark ? 'Dark' : 'Light') + ' theme');
  btn.setAttribute('aria-pressed', isDark ? 'true' : 'false');
  btn.setAttribute('aria-label', isDark ? 'Switch to light theme' : 'Switch to dark theme');
};

AppShell.updateThemeToggleVisibility = function() {
  var modes = getAvailableModes(getActiveThemeId());
  var toggleControl = q('#theme-toggle').closest('.header-control');
  if (modes.length < 2) {
    toggleControl.setAttribute('hidden', '');
  } else {
    toggleControl.removeAttribute('hidden');
  }
};

AppShell.updateModeLabel = function(m) {
  m = AppShell.normalizeMode(m);
  const isAdvanced = (m === 'advanced');
  const btn = q('#mode-toggle');
  btn.setAttribute('title', isAdvanced ? 'Advanced mode' : 'Guided mode');
  btn.setAttribute('aria-pressed', isAdvanced ? 'true' : 'false');
  btn.setAttribute('aria-label', isAdvanced ? 'Switch to guided mode' : 'Switch to advanced mode');
};

AppShell.closeSidebar = function() {
  q('#sidebar').classList.remove('open');
  q('#sidebar-overlay').classList.remove('visible');
};

export { AppShell };
