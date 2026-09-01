/**
 * FrameWorks Player wrapper.
 * Uses <fw-player> custom element from @livepeer-frameworks/player-wc.
 * CSS is encapsulated in shadow DOM - no external stylesheet needed.
 */

import '@livepeer-frameworks/player-wc/define';
import { getHttpHost } from './api.js';
import { getConfig } from './config.js';

let playerEl = null;

export function mountFWPlayer(container, streamName, endpoints) {
  destroyFWPlayer();
  var cfg = getConfig();
  playerEl = document.createElement('fw-player');
  playerEl.contentId = streamName;
  playerEl.contentType = 'live';
  playerEl.endpoints = endpoints || undefined;
  playerEl.autoplay = cfg.autoplayMuted;
  playerEl.muted = cfg.autoplayMuted;
  playerEl.controls = true;
  playerEl.devMode = true;
  playerEl.debug = true;
  playerEl.mistUrl = getHttpHost();

  if (cfg.thumbnailUrl) {
    playerEl.thumbnailUrl = cfg.thumbnailUrl;
  }

  container.appendChild(playerEl);
  return playerEl;
}

export function destroyFWPlayer() {
  if (playerEl) {
    playerEl.destroy();
    playerEl.remove();
    playerEl = null;
  }
}
