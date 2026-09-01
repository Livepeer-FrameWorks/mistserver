/**
 * MistServer embed player wrapper.
 * Uses mistPlay() directly via ESM import - no runtime script loading.
 */

import { mistPlay } from '@player';
import { getHttpHost } from './api.js';
import { getConfig } from './config.js';
import { getSkinName, getThemeName, getThemeMode, onPlayerMounted } from './theme_panel.js';

export function mountMistPlayer(container, streamName) {
  var cfg = getConfig();
  var skin = getSkinName();
  var theme = getThemeName();
  var opts = {
    target: container,
    host: getHttpHost(),
    controls: true,
    muted: cfg.autoplayMuted,
    skin: skin,
  };
  if (theme && theme !== 'default') {
    opts.theme = theme;
    opts.themeMode = getThemeMode();
  }
  mistPlay(streamName, opts);
  onPlayerMounted();
  return Promise.resolve();
}

export function destroyMistPlayer(container) {
  if (container.MistVideo) container.MistVideo.unload();
  while (container.firstChild) container.removeChild(container.firstChild);
}
