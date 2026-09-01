/**
 * Bootstrap: detect HTTP endpoint, then wire up all panels.
 */

import { APP_NAME, APP_TITLE } from '@brand';
import { connectionStatus, detectHttpHost } from './api.js';
import { loadConfig, createConfigPanel } from './config.js';
import { createPlayerPanel, remountPlayer } from './player_panel.js';
import { createThemePanel } from './theme_panel.js';
import { createPublisherPanel } from './publisher.js';
import { initStreams, refreshStreams } from './streams.js';

function docReady(fn) {
  if (document.readyState === 'complete' || document.readyState === 'interactive') {
    setTimeout(fn, 1);
  } else {
    document.addEventListener('DOMContentLoaded', fn);
  }
}

docReady(function () {
  document.title = APP_TITLE;
  const h1 = document.querySelector('#pg-header h1');
  if (h1) h1.textContent = APP_NAME;

  const header = document.getElementById('pg-header');
  if (header) header.appendChild(connectionStatus);

  detectHttpHost().then(function () {
    loadConfig();
    createConfigPanel();
    createPlayerPanel();
    createThemePanel(remountPlayer);
    createPublisherPanel();
    initStreams();

    refreshStreams();
    setInterval(refreshStreams, 5000);
  });
});
